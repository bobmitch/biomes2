// ---------------------------------------------------------------------------
// Drawing the player's character (character_model.h poses it).
//
// The mesh and its textures go up once; each frame the caller hands over the
// bone matrices (model -> world) and the renderer copies them into this
// frame's upload buffer. GPU skinning in the vertex shader (character.hlsl),
// on the TERRAIN's root signature like the rocks: the main pass straight
// after the rocks, every cascade, and every local light's shadow faces
// (record_light_shadow) -- for the carried torch with the body round the
// flame cleared, or the hand would black out its whole light. Those faces
// are cached, so the renderer redraws the ones the body is in (or was) when
// pose_serial() changes. Not the heightfield GI's top-down map: a body is
// not terrain.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CHARACTER_RENDERER_H
#define ENGINE_CHARACTER_RENDERER_H

#include "engine/character/character_model.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace engine {

class CharacterRenderer {
public:
    static const UINT kFrames = 2;
    static const UINT kMaxJoints = 255;
    // Heap slots of the two texture arrays: t49..t50, a range of the
    // terrain's table (Renderer::create_mesh_pipeline).
    static const UINT kSrvA = 110, kSrvB = 111;

    // Loads the pack and uploads it. A missing pack is not an error for the
    // engine -- ready() stays false, note() says why and what to run.
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
              ID3D12DescriptorHeap* heap, const std::string& pack, std::string& error);
    bool ready() const { return ready_; }
    const std::string& note() const { return note_; }
    const CharacterModel& model() const { return model_; }

    // This frame's pose: 12 floats a joint. visible false draws nothing.
    void set_pose(const std::vector<float>& bones, bool visible);
    bool visible() const { return visible_; }
    // The body's bounding sphere this frame; false when nothing is drawn.
    bool bounds(float c[3], float& r) const;
    // Changes whenever a joint has moved 3 mm since the last change.
    unsigned long long pose_serial() const { return pose_serial_; }

    // Copy the pose into `frame`'s buffer. Once a frame, before recording.
    void prepare(UINT frame);
    // The terrain's root signature, constants, shadow CBV and table bound.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);
    void record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame);
    // A local light's face, its matrix in the bound constants. Nothing of
    // the body within `clear` metres of the light casts. Sets root
    // constants 16..19 (cam_pos, far_plane).
    void record_light_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, const float light_pos[3], float clear);

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    void bind(ID3D12GraphicsCommandList* cmd, UINT frame);

    bool ready_ = false, visible_ = false, drawn_[kFrames] = {false, false};
    std::string note_;
    CharacterModel model_;
    std::vector<float> pose_;
    std::vector<float> joints_, committed_;   // joint positions, now and at the last serial
    float bound_c_[3] = {0, 0, 0}, bound_r_ = 0.0f;
    unsigned long long pose_serial_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, tex_a_, tex_b_, bones_[kFrames];
    float* bones_cpu_[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
    D3D12_INDEX_BUFFER_VIEW ibv_ = {};
    ComPtr<ID3D12PipelineState> pso_, shadow_pso_, light_shadow_pso_;
};

}  // namespace engine

#endif  // ENGINE_CHARACTER_RENDERER_H
