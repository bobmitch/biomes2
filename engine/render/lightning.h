#pragma once
// ---------------------------------------------------------------------------
// Lightning (docs/adr-005-clouds-and-rain.md, phase C5).
//
// A strike is a PURE FUNCTION of the seed, the clock and where the camera
// is (to a 2 km cell), like the rest of the weather: the clock is cut into
// 2.5 s slots, and each slot's hash says whether it holds a strike, when in
// the slot it starts, where, whether it reaches the ground, and how many
// return strokes it has. The rate rises from nothing at storm 0.78.
//
//   flash    the light of it: a point source -- the channel's middle for a
//            cloud-to-ground stroke, a point inside the cloud for an
//            in-cloud flash -- whose luminous intensity (cd) pulses with the
//            return strokes, each a 30 ms decay, over a dimmer continuing
//            current. Lit shaders add it to the AMBIENT target (lighting.hlsli
//            flash_light), the clouds glow round it (clouds.hlsl), and the
//            exposure meter holds while it lasts, so it reads as a flash, not
//            a change of light the eye adapts to.
//   bolt     the channel, made on the CPU when a strike begins: a jagged
//            midpoint-displaced line from the cloud base to the ground with a
//            few branches, drawn additively (shaders/lightning.hlsl) with a
//            glow, depth tested against the scene.
//
// Presentation only: nothing here reaches the golden hash or a save.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine {

struct LightningSettings {
    float storm_lo = 0.78f, storm_hi = 1.0f;   // the storm dial where strikes begin, and where the rate is full
    float per_minute = 14.0f;                  // strikes a minute at the full rate
    float candela = 5e10f;                     // a return stroke's peak luminous intensity
    float range_m = 6000.0f;                   // strikes fall within this of the camera's 2 km cell
    float ground_share = 0.4f;                 // the share that reach the ground (the rest stay in the cloud)
};

struct Strike {
    bool     active = false;
    bool     ground = false;          // cloud-to-ground (a bolt), or in-cloud (a flash only)
    uint32_t id = 0;                  // the slot, for regenerating the bolt
    float    ground_pt[3] = {0, 0, 0};
    float    cloud_pt[3] = {0, 0, 0};
    float    flash_pt[3] = {0, 0, 0};  // where the light comes from
    float    candela = 0.0f;          // now
    float    channel = 0.0f;          // 0..1, how bright the channel is now (the bolt)
    float    age = 0.0f;              // s since it began
    float    distance = 0.0f;         // m from the camera to the strike point
};

struct BoltSegment {
    float a[3];  float bright;        // 1 the main channel, less for branches
    float b[3];  float width;         // m
};

class Lightning {
public:
    static const unsigned kMaxSegments = 1024;

    bool load(const char* archetypes_path, std::string& error);
    void set_seed(uint32_t seed) { seed_ = seed; }
    uint32_t seed() const { return seed_; }
    const LightningSettings& settings() const { return settings_; }

    // The strike active at time t, if any. `storm_at` gives the storm dial at
    // a time; `ground_h` the terrain height at x, z. A forced strike (stills)
    // takes the place of the schedule.
    Strike at(double t, const float cam[3], float base_alt, const std::function<float(double)>& storm_at,
              const std::function<float(float, float)>& ground_h) const;
    // A strike for a still: `dist` m away, `bearing` radians from the camera's
    // forward (positive to the right), at the peak of its first stroke.
    void force(float dist, float bearing, bool in_cloud = false) {
        forced_ = true; forced_dist_ = dist; forced_bearing_ = bearing; forced_cloud_ = in_cloud;
    }
    bool forced() const { return forced_; }
    void set_view_basis(const float fwd[3], const float right[3]);

    // The channel for a strike (regenerated when its id changes).
    const std::vector<BoltSegment>& bolt(const Strike& s, float base_alt);

    // --- drawing ---------------------------------------------------------------
    struct Frame {
        float view_proj[16];   // this frame, jittered
        float cam_pos[3];
        float near_plane, far_plane;
        float pixel_angle;     // radians a pixel
        float channel;         // 0..1 the channel's brightness now
        unsigned width, height;
    };
    bool init(ID3D12Device* device, DXGI_FORMAT target, std::string& error);
    // Additively onto `rtv` (RENDER_TARGET); `depth_table` is the heap handle
    // of the scene depth's SRV (t0).
    void record(ID3D12GraphicsCommandList* cmd, unsigned frame, const Frame& f,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_GPU_DESCRIPTOR_HANDLE depth_table);
    bool ready() const { return ready_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    LightningSettings settings_;
    uint32_t seed_ = 0;
    bool forced_ = false;
    float forced_dist_ = 0.0f, forced_bearing_ = 0.0f;
    bool forced_cloud_ = false;
    float fwd_[3] = {0, 0, 1}, right_[3] = {1, 0, 0};
    uint32_t bolt_id_ = 0xFFFFFFFFu;
    std::vector<BoltSegment> segs_;
    unsigned uploaded_[2] = {0, 0};
    uint32_t uploaded_id_[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

    bool ready_ = false;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12Resource> cb_[2], segbuf_[2];
    void* cb_cpu_[2] = {};
    void* seg_cpu_[2] = {};
};

}  // namespace engine
