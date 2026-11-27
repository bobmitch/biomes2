#pragma once
// ---------------------------------------------------------------------------
// The trample map: where something has been standing in the ground cover, and
// which way it pushed it.
//
// Grass is a pure function of its cell -- nothing about a blade is stored, and
// nothing should be -- so the memory of being trodden on lives in the WORLD,
// not in the blade. One camera-centred texture, 10 cm texels over 204.8 m,
// holding a push vector per texel: the same measure the wind answers with, a
// tip displacement as a fraction of a blade's height, so `vs_blade` bends,
// shortens and leans by it without knowing which pushed it.
//
// TOROIDAL by construction. World texel w lives at w mod N, which with a
// power-of-two N and a WRAP sampler is just frac(world.xz / kMapMetres) -- no
// origin in the shader, and following the camera refills only the strips that
// scrolled in rather than moving anything. The near field's occupancy volume
// does the same thing for the same reason.
//
// TWO decay rates, not two systems. `cs_splat` writes the same push into both
// pairs of channels and `cs_decay` fades them at different speeds: the fast
// pair (xy) is gone in a fraction of a second, so a bush shoved aside springs
// back the moment the creature steps off; the slow pair (zw) lasts tens of
// seconds, so grass stays laid over behind a cart. The "push" is the "flatten"
// read at age zero -- something standing still re-stamps its texel every frame
// and both pairs stay full, which is the "pushed while on them" case with no
// special handling at all.
//
// The recovery time is bounded by the map, not by taste: anything that scrolls
// past the edge is forgotten, so a trail that outlived the window would pop.
// At 102 m from the camera to the edge that is ~20 s at a jog, which is what
// kSlowTau is set under. Longer trails would need world-space state that is
// saved, which is a different and much larger feature.
//
// Presentation only: no hash, no save, no physics. Cleared on load, respawn
// and any jump of the camera.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

class TrampleMap {
public:
    static const UINT kN = 2048;             // texels a side, a power of two: the wrap IS the torus
    static constexpr float kTexel = 0.1f;    // metres: a footprint is 3 texels, a cart rut 1
    static constexpr float kMapMetres = float(kN) * kTexel;   // 204.8 m, so 102 m to the edge
    // The most a blade may lean (foliage.hlsl clamps the wind and the push
    // together to this: past 1 / sqrt(0.35) the tip is drawn under its root).
    static constexpr float kMaxLean = 1.5f;

    // `srv_slot` and `uav_slot` are the renderer's heap; see its slot map.
    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT srv_slot, UINT uav_slot,
              std::string& error);
    bool ready() const { return ready_; }

    // A capsule pressed into the cover this frame, in world space: from (x0,
    // z0) to (x1, z1), `radius` across, `strength` as a fraction of a blade's
    // height (~1 is lying over). Long capsules -- a felled log -- are split
    // into pieces the splat's own footprint can hold. Cleared every frame.
    void add(float x0, float z0, float x1, float z1, float radius, float strength);
    // A capsule pressed again every frame until cleared. Nothing walks
    // during a still -- a trail is history and no camera can be pointed at
    // one -- so `--trample` holds a path here and the shot has something to
    // show, the way --dig-row digs before it shoots.
    void hold(float x0, float z0, float x1, float z1, float radius, float strength);
    void clear_held() { held_.clear(); }
    void clear_stamps() { stamps_.clear(); }
    size_t stamp_count() const { return stamps_.size(); }

    // Follow the camera, stamp this frame's capsules, and fade what is there.
    // The caller has the renderer's heap bound. Leaves the map readable by a
    // shader. `dt` is the frame's own; the fade is applied in batches, so a
    // still and a long run agree.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, float cam_x, float cam_z, float dt);
    // Forget everything: a load, a respawn, a camera that jumped.
    void reset() { full_clear_ = true; }

    ID3D12Resource* texture() const { return tex_.Get(); }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Stamp { float x0, z0, x1, z1, radius, strength, pad0, pad1; };

    bool ready_ = false;
    ComPtr<ID3D12Resource> tex_, cb_[2], stamps_up_[2];
    void* cb_cpu_[2] = {};
    void* stamps_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> splat_pso_, decay_pso_, clear_pso_;
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT inc_ = 0, uav_slot_ = 0;

    std::vector<Stamp> stamps_, held_;
    int  org_x_ = 0, org_z_ = 0;      // the map's corner, in world texels
    bool have_org_ = false;
    bool full_clear_ = true;
    float decay_dt_ = 0.0f;           // seconds owed to the fade
    UINT  frames_ = 0;
};

}  // namespace engine
