#pragma once
// ---------------------------------------------------------------------------
// The near field: light under the top surface (dug pits and tunnels, and
// caves when they come back), where the heightfield GI cannot see.
//
// A camera-centred volume of the terrain as it is NOW -- 128 x 64 x 128
// voxels of 1 m, the field and the player's edits -- and a grid of probes
// at 2 m over it. The volume is toroidal: world voxel w lives at storage
// w mod N, so following the camera means refilling the 8 m blocks that
// scrolled in, never moving anything. A dig refills the blocks it touched.
// Each probe marches six cones through the occupancy mips until they rise
// above the heightfield's top surface, and carries back the heightfield's
// own sky visibility and bounce for that direction, times what got
// through: so a pit's floor sees the sky its walls leave, a tunnel darkens
// metre by metre from its mouth, and a sealed chamber gets nothing.
// hfgi.hlsli reads the probes for any point under the top surface.
//
// A ray that meets rock carries back what that wall sends: the sun on it,
// the local lights on it, and the probes round it (every further bounce).
// Deep and unlit stays dark: nothing there to send anything.
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

class BrickStore;
class WorldGpu;

class NearField {
public:
    static const int kNx = 128, kNy = 64, kNz = 128;   // voxels of 1 m
    static const int kBlock = 8;                        // voxels a block side
    static const int kBx = kNx / kBlock, kBy = kNy / kBlock, kBz = kNz / kBlock;
    static const int kBlocks = kBx * kBy * kBz;
    static const int kPx = kNx / 2, kPy = kNy / 2, kPz = kNz / 2;   // probes of 2 m
    static const int kProbes = kPx * kPy * kPz;
    static const int kProbeFloat4s = 5;
    static const int kMips = 5;
    // Heap: [40] the probes, [41] the occupancy (all mips) and [42] the top
    // surface per column, the range every lit pipeline's table already
    // declares (t26..t28, once DDGI's); then this class's own: the density's
    // SRV and UAV, one UAV a mip, the top surface's UAV.
    static const UINT kSrvProbes = 40, kSrvOcc = 41, kSrvTop = 42;
    static const UINT kFirst = 112;
    static const UINT kSrvDensity = kFirst, kUavDensity = kFirst + 1, kUavOcc = kFirst + 2;
    static const UINT kUavTop = kUavOcc + kMips;
    static const UINT kHeapEnd = kUavTop + 1;
    // Per-frame budgets: blocks filled from the field, probes re-traced.
    static const int kBlocksPerFrame = 48;
    static const int kProbesPerFrame = 4096;
    static const int kMaxEdits = 65536;
    // Probes round the camera traced on a TIMER, so a carried torch's bounce
    // follows it: a box this many probes a side (2 m each), in 8 interleaved
    // slices (every other probe each way), as many slices a frame as its
    // time has paid for.
    static const int kCameraBox = 8;
    static const int kBoxSlices = 8;
    // How often the box is traced and how long probes remember -- in Hz and
    // seconds, so the look and the cost a second do not depend on the frame
    // rate (at 144 fps the box is not traced 144 times a second). A probe's
    // blend over its history is exp(-interval / memory). Since the kernel
    // went to a thread a ray the rate barely moves the cost (0.050 ms at 20
    // Hz, 0.053 at 60, in a tunnel at 1440p); a higher rate is less flicker
    // (pixels a frame jumping 20 levels: 8 at 60 Hz, ~40 at 20).
    struct Schedule {
        float box_rate = 60.0f;      // Hz: the camera's box traced this often
        float near_memory = 0.35f;   // s: the box's probes' history (0.95 a frame at 60)
        float far_memory = 1.5f;     // s: the round robin's (0.70 a lap at 60 fps; 3.3 s took half a minute to settle a tunnel walked into)
    };
    Schedule& schedule() { return sched_; }

    // `cloud_shadow_srv`: the clouds' shadow map's heap slot (Clouds::kShadowSrv),
    // which a sunlit wall's light passes through.
    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT cloud_shadow_srv, std::string& error);
    bool ready() const { return ready_; }

    // The player's edits changed inside a world box: its blocks are filled
    // again (field + edits) and the probes whose cones can reach it re-traced.
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();

    // CPU, every frame before record: which blocks to fill, which edits to
    // scatter over them, which probes to re-trace. `settle` does it all at
    // once, for a still. `dt`: this frame's seconds (< 0: the wall clock).
    void plan(const float cam[3], const BrickStore* edits, bool settle, float dt = -1.0f);
    // GPU. Needs the frame's shadow constants (the heightfield's corner)
    // and the heightfield's maps already built this frame.
    // `cloud` the clouds' shadow parameters (Clouds::shadow_params; zero extent
    // for none). The sun's shadow map and the light atlas must be readable
    // by compute.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const WorldGpu& world,
                D3D12_GPU_VIRTUAL_ADDRESS shadow_cb, const float cloud[4]);
    // ShadowConstants' l_nf: the volume's min world voxel, w ready.
    void constants(bool on, float out[4]) const;

    // Diagnostics, from the last plan.
    int blocks_planned() const { return int(blocks_.size()); }
    int probes_planned() const { return int(probe_list_.size()); }
    int edits_planned() const { return int(edit_list_.size() / 4); }
    int blocks_pending() const { return pending_; }
    // Over its life: plans, plans that filled blocks, blocks filled, probes traced.
    unsigned long long plans() const { return plans_; }
    unsigned long long fill_frames() const { return fill_frames_; }
    unsigned long long blocks_filled() const { return blocks_filled_; }
    unsigned long long probes_traced() const { return probes_traced_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    Schedule sched_;
    float box_credit_ = 0.0f;            // slices of the camera's box paid for, not yet traced
    uint32_t box_slice_ = 0;             // the next slice
    float dt_smooth_ = 1.0f / 60.0f;     // the frame's seconds, smoothed (the round robin's lap)
    unsigned long long last_tick_ = 0;   // the wall clock at the last plan
    float keep_box_ = 0.95f, keep_rr_ = 0.85f;   // this frame's blends (NfConstants)
    int origin_[3] = {0, 0, 0};          // min world voxel, a multiple of kBlock
    bool have_origin_ = false;
    int pending_ = kBlocks;
    // Per storage block: the world block it holds (or none), and whether an
    // edit has dirtied it since.
    std::vector<int> held_;              // 3 ints a block; INT32_MIN = none
    std::vector<uint8_t> dirty_;
    std::vector<uint8_t> probe_dirty_;   // per probe storage index
    std::vector<uint32_t> dirty_probes_;
    uint32_t cursor_ = 0;                // round robin over every probe
    uint32_t rng_ = 0x9E3779B9u;         // the rays' rotation, a new one each update

    std::vector<int32_t> blocks_;        // 4 ints a planned block
    std::vector<float> edit_list_;       // 4 floats an entry
    std::vector<uint32_t> probe_list_;
    bool occ_dirty_ = false;
    unsigned long long plans_ = 0, fill_frames_ = 0, blocks_filled_ = 0, probes_traced_ = 0;

    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> density_, occ_, probes_, top_;
    bool top_is_srv_ = false;
    ComPtr<ID3D12Resource> cb_[2], blocks_up_[2], edits_up_[2], probes_up_[2];
    void* cb_cpu_[2] = {};
    void* blocks_cpu_[2] = {};
    void* edits_cpu_[2] = {};
    void* probes_cpu_[2] = {};
    bool occ_is_srv_ = false, probes_is_srv_ = false;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> fill_pso_, edits_pso_, occ_pso_, mip_pso_, probe_pso_, top_pso_;
};

}  // namespace engine
