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
// Each probe marches its rays through the occupancy until they rise above
// the heightfield's top surface, and carries back the heightfield's own
// sky visibility and bounce for that direction: so a pit's floor sees the
// sky its walls leave, a tunnel darkens metre by metre from its mouth, and
// a sealed chamber gets nothing.
// hfgi.hlsli reads the probes for any point under the top surface.
//
// A ray that meets rock carries back what that wall sends: the sun on it,
// the local lights on it, and the probes round it (every further bounce).
// Deep and unlit stays dark: nothing there to send anything.
//
// TWO LEVELS, after Split Radiance Cascades (docs/2607.20384v2.pdf; its
// idea only -- no hash map, no screen-space split, no hardware rays). The
// 2 m probes trace their 64 rays 0-6 m. A coarse level, one probe per 8 m
// block placed in the block's covered air, traces 256 rays from 6 m to
// 48 m into 64 octahedral bins (four jittered rays a bin, the cascades'
// branching of four), so a fine ray that gets 6 m without meeting rock
// takes the bin in its direction from the coarse probes round it. A far
// sunlit sliver is then an average over a bin's solid angle, not a rare
// fine ray carrying 100 klx.
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include "engine/render/sky_access.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class BrickStore;
class GpuProfiler;
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
    // The probe visibility volume (cs_nf_vis): read by every table that
    // reads the probes (t30), written here.
    static const UINT kSrvVis = kUavTop + 1, kUavVis = kUavTop + 2;
    static const UINT kUavSky = kUavTop + 3;   // the sky access's distances (u22)
    static const UINT kHeapEnd = kUavTop + 4;
    // Per-frame budgets: blocks filled from the field, probes listed.
    static const int kBlocksPerFrame = 48;
    static const int kProbesPerFrame = 2048;   // the round robin: placing and copying
    static const int kMaxEdits = 65536;
    // The coarse level: a probe a block (its storage index is the block's),
    // 64 octahedral bins of two float4 -- (a R, a G, a B, a vis), (a) with a
    // the share of the bin's rays that got 6 m clear -- after a meta float4
    // per probe (placed, its offset from the block's centre).
    static const int kCoarse = kBlocks;
    static const int kCoarseBins = 64;
    static const int kCoarseFloat4s = kCoarse + kCoarse * kCoarseBins * 2;
    static const int kCoarsePerFrame = 128;
    // Local lights a ray's hit within 6 m is lit by: the frame's first.
    static const int kHitLights = 32;
    // A still runs both levels this many times a frame: each bounces off the
    // other, and a still's ~17 frames left that chain unconverged.
    static const int kSettleIters = 4;
    // The live lists (near_field.hlsl u_aux): a mode per probe, the fine and
    // coarse lists the live pass traces every frame, their two counts.
    static const int kAuxUints = 2 * kProbes + kCoarse + 4;   // the counts in two slots (by the plan's parity)
    static const int kLiveMax = 8192;   // fine probes a frame (kNfLiveMax)
    // The light jumping (set_light_change): `c` is the frame's change --
    // |ln| of the key light's and the sky's ratios to last frame's, plus the
    // key's turn over 0.05 rad. Past kRelightReset (a time skip of more than
    // ~10 minutes, the key light passing from sun to moon) the near field
    // starts over and settles in the frame, as a still does. Below it
    // nothing changes: with every live probe traced every frame the clock
    // run fast (H) is followed anyway, and shortening the blends by `c` --
    // tried, exp(-c / 0.1) -- made the meter jump 4x frame to frame through
    // a fast sunset.
    static constexpr float kRelightReset = 1.0f;
    // How long probes remember, in seconds, so the look does not depend on
    // the frame rate. A probe's blend over its history is exp(-dt / memory);
    // every probe under cover is traced every frame (the live pass).
    struct Schedule {
        float near_memory = 0.35f;   // s: the fine probes' history (0.95 a frame at 60)
        float coarse_memory = 1.0f;  // s: the coarse level's
        float light_memory = 0.1f;   // s: probes a moving or new light reaches
        float light_reach = 10.0f;   // m: how far from such a light
        // The live pass in turns: each fine probe traced every live_stride
        // frames, each coarse one every coarse_stride (blends cover it). The
        // coarse pass is latency, not work: a stride saves it nothing.
        int live_stride = 2;
        int coarse_stride = 1;
        // Sky access (sky_access.h): metres of path through air over which
        // the open sky's light falls by e -- how fast daylight dies down a
        // tunnel. archetypes.json `underground` sky_falloff_m; live.
        float sky_falloff = 12.0f;
    };
    Schedule& schedule() { return sched_; }

    // `cloud_shadow_srv`: the clouds' shadow map's heap slot (Clouds::kShadowSrv),
    // which a sunlit wall's light passes through.
    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT cloud_shadow_srv, std::string& error);
    bool ready() const { return ready_; }
    // The world whose underground air the sky-access flood labels
    // (sky_access.h). Without it no air is ever marked sealed.
    void set_world(const WorldGpu* world, const BrickStore* edits) { sky_.init(world, edits, sky_reach_); }
    // archetypes.json's `underground` block: sky_falloff_m, sky_reach_m.
    // Before set_world (the reach is the flood's).
    bool load_config(const char* path, std::string& error);
    // Whoever writes the edit store holds this for the write (the flood's
    // worker reads it).
    std::unique_lock<std::shared_mutex> sky_edit_lock() { return sky_.edit_lock(); }
    // Stops the flood's worker; before the world it reads goes away.
    void shutdown() { sky_.shutdown(); }
    const SkyAccess& sky_access() const { return sky_; }
    // The camera's cell: metres to the sky by path (255 none), and its
    // darkness, 1 - exp(-path / L) (constants: l_nf.w 1 + darkness).
    int camera_sky_dist() const { return cam_dist_; }
    float camera_dark() const { return cam_dark_; }
    // Held blocks still marked "not answered" (waiting on the flood).
    int sky_unlabelled() const { int n = 0; for (uint8_t u : held_unknown_) n += u; return n; }

    // The player's edits changed inside a world box: its blocks are filled
    // again (field + edits) and the probes whose cones can reach it re-traced.
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();

    // Lights whose bounce must keep up this frame -- the carried torch, a
    // campfire just placed: x, y, z, range per light. The live pass gives
    // their probes light_memory (up to 8 lights). Before plan.
    void set_fast_lights(const std::vector<float>& xyzr) { fast_ = xyzr; }
    // Before plan: how much the sun and sky changed since last frame.
    void set_light_change(float c) { relight_c_ = c; }
    static bool is_reset(float c) { return c > kRelightReset; }

    // CPU, every frame before record: which blocks to fill, which edits to
    // scatter over them, which probes to re-trace. `settle` does it all at
    // once, for a still. `dt`: this frame's seconds (< 0: the wall clock).
    void plan(const float cam[3], const BrickStore* edits, bool settle, float dt = -1.0f);
    // GPU. Needs the frame's shadow constants (the heightfield's corner)
    // and the heightfield's maps already built this frame.
    // `cloud` the clouds' shadow parameters (Clouds::shadow_params; zero extent
    // for none). The sun's shadow map and the light atlas must be readable
    // by compute.
    // `prof`: marks between its passes, for --gpu-profile (may be null).
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const WorldGpu& world,
                D3D12_GPU_VIRTUAL_ADDRESS shadow_cb, const float cloud[4], GpuProfiler* prof = nullptr);
    // ShadowConstants' l_nf: the volume's min world voxel, w ready.
    void constants(bool on, float out[4]) const;

    // Diagnostics, from the last plan.
    int blocks_planned() const { return int(blocks_.size()); }
    int probes_planned() const { return int(probe_list_.size()); }
    int edits_planned() const { return int(edit_list_.size() / 4); }
    int blocks_pending() const { return pending_; }
    int marks_planned() const { return int(marks_.size() / kMarkWords); }
    // Over its life: plans, plans that filled blocks, blocks filled, probes traced.
    unsigned long long plans() const { return plans_; }
    unsigned long long fill_frames() const { return fill_frames_; }
    unsigned long long blocks_filled() const { return blocks_filled_; }
    unsigned long long probes_traced() const { return probes_traced_; }
    unsigned long long coarse_traced() const { return coarse_traced_; }
    // The live lists' lengths, read back (two frames late); resets so far.
    unsigned live_fine() const { return live_fine_; }
    unsigned live_coarse() const { return live_coarse_; }
    unsigned long long resets() const { return resets_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    Schedule sched_;
    float dt_smooth_ = 1.0f / 60.0f;     // the frame's seconds, smoothed (the round robin's lap)
    unsigned long long last_tick_ = 0;   // the wall clock at the last plan
    float keep_box_ = 0.95f, keep_rr_ = 0.85f;   // this frame's blends (NfConstants)
    float keep_coarse_ = 0.9f, keep_light_ = 0.85f;
    std::vector<float> fast_;            // x, y, z, range: lights traced on the timer
    std::vector<uint32_t> stamp_;        // per probe: the plan that last listed it on the timer
    uint32_t coarse_cursor_ = 0;
    uint32_t settle_frames_ = 0;         // plans in a row that settled (a still's frames so far)
    bool settling_ = false;              // this plan settled: record iterates (kSettleIters)
    float relight_c_ = 0.0f;             // set_light_change
    bool reset_ = false;                 // this frame starts over (record clears the probes first)
    unsigned long long resets_ = 0;
    float keep_live_ = 0.95f, keep_live_light_ = 0.85f, keep_live_coarse_ = 0.98f;
    unsigned live_fine_ = 0, live_coarse_ = 0;
    std::vector<uint32_t> coarse_list_;
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
    // Sky access: the flood, the mask each storage block holds on the GPU
    // (16 words), the flood's generation they were made at, and this frame's
    // marks (20 words an entry: block xyz, pad, mask) for cs_nf_mark.
    SkyAccess sky_;
    static const int kMarkWords = 4 + SkyAccess::kCells / 4;   // an entry: block, flag, 512 bytes (cs_nf_mark)
    float sky_reach_ = 96.0f;
    std::vector<uint8_t> held_dist_;     // per storage block: the distances the GPU holds (512)
    std::vector<uint8_t> sky_pending_;   // per storage block: asked, not answered
    std::vector<float> sky_boxes_;       // edit boxes since the last plan (6 floats each)
    bool sky_all_ = false;               // an invalidate_all since the last plan
    uint8_t cam_dist_ = 0;
    float cam_dark_ = 0.0f;
    std::vector<uint8_t> held_unknown_;  // per storage block: its GPU marks are the "not labelled" rule
    std::vector<uint32_t> mark_stamp_;   // per storage block: the plan that last listed it for cs_nf_mark
    uint32_t mark_plan_ = 0;
    uint64_t sky_gen_ = 0;
    // Masks the flood has answered under the current generation, by world block.
    std::unordered_map<int64_t, std::vector<uint8_t> > sky_cache_;
    std::vector<SkyAccess::Result> sky_results_;
    std::vector<uint32_t> marks_;
    // The visibility volume's blocks this frame (4 ints each): the filled
    // ones and their held neighbours.
    std::vector<int32_t> vis_blocks_;
    std::vector<uint32_t> vis_stamp_;
    uint32_t vis_plan_ = 0;

    std::vector<float> edit_list_;       // 4 floats an entry
    std::vector<uint32_t> probe_list_;
    bool occ_dirty_ = false;
    unsigned long long plans_ = 0, fill_frames_ = 0, blocks_filled_ = 0, probes_traced_ = 0, coarse_traced_ = 0;

    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> density_, occ_, probes_, top_, coarse_, aux_, args_, live_rb_[2], vis_, sky_vol_;
    bool vis_is_srv_ = false;
    ComPtr<ID3D12CommandSignature> dispatch_sig_;
    bool live_rb_pending_[2] = {false, false};
    bool top_is_srv_ = false;
    ComPtr<ID3D12Resource> cb_[2], blocks_up_[2], edits_up_[2], probes_up_[2], coarse_up_[2], marks_up_[2], vis_up_[2];
    void* marks_cpu_[2] = {};
    void* vis_cpu_[2] = {};
    void* cb_cpu_[2] = {};
    void* blocks_cpu_[2] = {};
    void* edits_cpu_[2] = {};
    void* probes_cpu_[2] = {};
    void* coarse_cpu_[2] = {};
    bool occ_is_srv_ = false, probes_is_srv_ = false;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> fill_pso_, edits_pso_, occ_pso_, mip_pso_, probe_pso_, top_pso_, coarse_pso_,
        probe_live_pso_, coarse_live_pso_, classify_pso_, live_pso_, live_args_pso_, clear_pso_, mark_pso_, vis_pso_;
};

}  // namespace engine
