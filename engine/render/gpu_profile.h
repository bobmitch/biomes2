// ---------------------------------------------------------------------------
// Per-pass GPU timing and pipeline statistics, for the report modes.
//
// Off by default and free when off: every call returns at once. When on
// (`--shot ... --gpu-profile`), each mark() puts a NULL UAV barrier in front
// of its timestamp. Without it a timestamp is written while the work before
// it may still be running -- the atmosphere once read 0 ms with its time
// counted inside the mist's (CLAUDE.md) -- so the barrier SERIALISES the
// passes: each segment is the work between two marks alone, and the
// profiled frame is a little slower than a real one, where passes overlap.
// The report prints both totals, so the cost of that serialisation is seen.
//
// A segment is named by the mark that ENDS it. Names may repeat within a
// frame (the cascades); their times add. stat_begin/stat_end wrap draws in
// a pipeline-statistics query: pixel-shader invocations are the overdraw
// that was actually PAID, after whatever early depth rejection the hardware
// managed.
// ---------------------------------------------------------------------------
#ifndef ENGINE_GPU_PROFILE_H
#define ENGINE_GPU_PROFILE_H

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace engine {

class GpuProfiler {
public:
    static const UINT kMaxMarks = 128;
    static const UINT kMaxStats = 48;
    static const UINT kFramesInFlight = 2;

    struct Segment {
        double ms = 0.0;        // summed over every harvested frame
        double ms_max = 0.0;    // the largest single frame's
        unsigned frames = 0;    // frames it appeared in
        unsigned order = 0;     // first seen, for printing in frame order
    };
    struct Stat {
        double vs = 0.0, ia_prims = 0.0, c_prims = 0.0, ps = 0.0;
        unsigned frames = 0;
        unsigned order = 0;
    };

    bool init(ID3D12Device* dev, ID3D12CommandQueue* queue) {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kMaxMarks * kFramesInFlight;
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&ts_heap_)))) return false;
        qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        qd.Count = kMaxStats * kFramesInFlight;
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&st_heap_)))) return false;
        const UINT64 ts_bytes = UINT64(kMaxMarks) * kFramesInFlight * 8;
        const UINT64 st_bytes = UINT64(kMaxStats) * kFramesInFlight * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        if (!readback(dev, ts_bytes, ts_rb_) || !readback(dev, st_bytes, st_rb_)) return false;
        queue->GetTimestampFrequency(&freq_);
        ready_ = freq_ != 0;
        return ready_;
    }

    void set_enabled(bool on) { on_ = on; }
    bool enabled() const { return on_ && ready_; }
    // Detail off: only the frame's first and last marks, no barriers between
    // passes and no statistics -- the frame's own GPU time, passes
    // overlapping as they do when nobody is measuring.
    void set_detail(bool on) { detail_ = on; }
    bool detailed() const { return enabled() && detail_; }

    void begin_frame(ID3D12GraphicsCommandList* cmd, UINT fi) {
        if (!enabled()) return;
        fi %= kFramesInFlight;
        harvest(fi);   // the caller has waited for this frame index's fence
        cur_ = fi;
        Frame& f = frames_[fi];
        f.marks.clear();
        f.stats.clear();
        f.open_stat = -1;
        in_frame_ = true;
        mark(cmd, "(frame start)");
    }

    void mark(ID3D12GraphicsCommandList* cmd, const char* name) {
        if (!enabled() || !in_frame_) return;
        Frame& f = frames_[cur_];
        if (!detail_ && !f.marks.empty() && std::strcmp(name, "(frame end)") != 0) return;
        if (f.marks.size() >= kMaxMarks) return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd->ResourceBarrier(1, &b);
        cmd->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, cur_ * kMaxMarks + UINT(f.marks.size()));
        f.marks.push_back(name);
    }

    void stat_begin(ID3D12GraphicsCommandList* cmd, const char* name) {
        if (!detailed() || !in_frame_) return;
        Frame& f = frames_[cur_];
        if (f.open_stat >= 0 || f.stats.size() >= kMaxStats) return;
        f.open_stat = int(f.stats.size());
        cmd->BeginQuery(st_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, cur_ * kMaxStats + UINT(f.open_stat));
        f.stats.push_back(name);
    }
    void stat_end(ID3D12GraphicsCommandList* cmd) {
        if (!enabled() || !in_frame_) return;
        Frame& f = frames_[cur_];
        if (f.open_stat < 0) return;
        cmd->EndQuery(st_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, cur_ * kMaxStats + UINT(f.open_stat));
        f.open_stat = -1;
    }

    void end_frame(ID3D12GraphicsCommandList* cmd) {
        if (!enabled() || !in_frame_) return;
        mark(cmd, "(frame end)");
        Frame& f = frames_[cur_];
        if (f.open_stat >= 0) stat_end(cmd);
        cmd->ResolveQueryData(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, cur_ * kMaxMarks,
                              UINT(f.marks.size()), ts_rb_.Get(), UINT64(cur_) * kMaxMarks * 8);
        if (!f.stats.empty())
            cmd->ResolveQueryData(st_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, cur_ * kMaxStats,
                                  UINT(f.stats.size()), st_rb_.Get(),
                                  UINT64(cur_) * kMaxStats * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        f.pending = true;
        in_frame_ = false;
    }

    // Reads a frame index's results into the totals. Only once its fence
    // has passed: after wait_for_gpu, or at the start of that index's next
    // frame.
    void harvest(UINT fi) {
        Frame& f = frames_[fi % kFramesInFlight];
        if (!f.pending) return;
        f.pending = false;
        if (skip_ > 0) { --skip_; return; }
        const UINT n = UINT(f.marks.size());
        UINT64* ts = nullptr;
        D3D12_RANGE r = {0, SIZE_T(kMaxMarks) * kFramesInFlight * 8};
        if (n >= 2 && SUCCEEDED(ts_rb_->Map(0, &r, reinterpret_cast<void**>(&ts)))) {
            const UINT64* t = ts + size_t(fi % kFramesInFlight) * kMaxMarks;
            const double k = 1000.0 / double(freq_);
            std::map<std::string, double> frame_sum;
            for (UINT i = 1; i < n; ++i) frame_sum[f.marks[i]] += double(t[i] - t[i - 1]) * k;
            for (std::map<std::string, double>::const_iterator it = frame_sum.begin(); it != frame_sum.end(); ++it) {
                Segment& s = seg_[it->first];
                if (s.frames == 0 && s.ms == 0.0) s.order = order_of(it->first, f.marks);
                s.ms += it->second;
                s.ms_max = (std::max)(s.ms_max, it->second);
                ++s.frames;
            }
            total_ms_ += double(t[n - 1] - t[0]) * k;
            D3D12_RANGE w = {0, 0};
            ts_rb_->Unmap(0, &w);
        }
        D3D12_QUERY_DATA_PIPELINE_STATISTICS* ps = nullptr;
        D3D12_RANGE r2 = {0, SIZE_T(kMaxStats) * kFramesInFlight * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        if (!f.stats.empty() && SUCCEEDED(st_rb_->Map(0, &r2, reinterpret_cast<void**>(&ps)))) {
            const D3D12_QUERY_DATA_PIPELINE_STATISTICS* p = ps + size_t(fi % kFramesInFlight) * kMaxStats;
            for (size_t i = 0; i < f.stats.size(); ++i) {
                Stat& s = stat_[f.stats[i]];
                if (s.frames == 0) s.order = next_order_++;
                s.vs += double(p[i].VSInvocations);
                s.ia_prims += double(p[i].IAPrimitives);
                s.c_prims += double(p[i].CPrimitives);
                s.ps += double(p[i].PSInvocations);
                ++s.frames;
            }
            D3D12_RANGE w = {0, 0};
            st_rb_->Unmap(0, &w);
        }
        ++harvested_;
    }
    void harvest_all() { for (UINT k = 0; k < kFramesInFlight; ++k) harvest(k); }

    // Clears the totals; the next `skip` harvested frames are dropped too
    // (the first frames after a switch are not representative).
    void reset(unsigned skip = 0) {
        seg_.clear();
        stat_.clear();
        total_ms_ = 0.0;
        harvested_ = 0;
        skip_ = skip;
    }

    unsigned frames() const { return harvested_; }
    double total_ms() const { return total_ms_; }
    const std::map<std::string, Segment>& segments() const { return seg_; }
    const std::map<std::string, Stat>& stats() const { return stat_; }

    // Segments in the order they first ran, for a report.
    std::vector<std::pair<std::string, Segment> > ordered_segments() const {
        std::vector<std::pair<std::string, Segment> > v(seg_.begin(), seg_.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, Segment>& a,
                                         const std::pair<std::string, Segment>& b) {
            return a.second.order < b.second.order;
        });
        return v;
    }
    std::vector<std::pair<std::string, Stat> > ordered_stats() const {
        std::vector<std::pair<std::string, Stat> > v(stat_.begin(), stat_.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, Stat>& a,
                                         const std::pair<std::string, Stat>& b) {
            return a.second.order < b.second.order;
        });
        return v;
    }

private:
    struct Frame {
        std::vector<std::string> marks;
        std::vector<std::string> stats;
        int open_stat = -1;
        bool pending = false;
    };

    static bool readback(ID3D12Device* dev, UINT64 bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&out)));
    }
    unsigned order_of(const std::string& name, const std::vector<std::string>& marks) const {
        // Position of its first mark in this frame, after everything seen
        // in earlier frames: a segment that only runs some frames (a far
        // cascade) still prints where it runs.
        for (size_t i = 0; i < marks.size(); ++i)
            if (marks[i] == name) return unsigned(i);
        return 1u << 30;
    }

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> ts_heap_, st_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource>  ts_rb_, st_rb_;
    UINT64 freq_ = 0;
    bool ready_ = false, on_ = false, in_frame_ = false, detail_ = true;
    UINT cur_ = 0;
    Frame frames_[kFramesInFlight];
    std::map<std::string, Segment> seg_;
    std::map<std::string, Stat> stat_;
    double total_ms_ = 0.0;
    unsigned harvested_ = 0, skip_ = 0, next_order_ = 0;
};

}  // namespace engine

#endif  // ENGINE_GPU_PROFILE_H
