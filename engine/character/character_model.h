// ---------------------------------------------------------------------------
// A rigged character: the packed model (tools/models/pack_character.py) and
// the procedural animation that poses it.
//
// The model is Quaternius's Universal Base Character (CC0): a UE5-mannequin
// skeleton of 65 joints, in T-pose, metres, Y up, facing +Z, its left on +X.
//
// Its clips are Quaternius's Universal Animation Library (CC0, the same joint
// names), packed by tools/models/pack_character.py and RETARGETED at load
// (AnimLibrary::load): the library's rest pose differs from each character's
// by up to 17 degrees at a joint and 8 cm in a limb, so a clip's local
// rotations are not copied; each joint's GLOBAL rotation relative to the
// library's rest is applied to the character's rest. CharacterAnimator picks
// and blends clips from what physics did -- horizontal speed, on the ground
// or not, in the water or not -- with walk, jog and sprint on one shared
// phase advanced by DISTANCE, so the feet do not skate at any speed.
//
// With no library, the animator falls back to a procedural gait: every joint
// delta a rotation about a MODEL-SPACE axis at the rest pose
// (CharacterAnimator::apply), the legs from gait-lab tables.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CHARACTER_MODEL_H
#define ENGINE_CHARACTER_MODEL_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {

struct CharacterVertex {
    float    pos[3];
    float    nrm[3];
    float    uv[2];
    uint8_t  joints[4];
    uint8_t  weights[4];   // sum 255
    uint32_t material;
};
static_assert(sizeof(CharacterVertex) == 44, "the packer's vertex record");

struct CharacterJoint {
    std::string name;
    int   parent = -1;      // always earlier in the list
    float t[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};   // x y z w
    float s[3] = {1, 1, 1};
    float inv_bind[12] = {};     // 3 x 4 rows, p' = M p
    // Derived at load: the rest pose in model space.
    float rest_q[4] = {0, 0, 0, 1};
    float rest_p[3] = {0, 0, 0};
};

struct CharacterMaterial {
    std::string name;
    float roughness = 1.0f;
    bool  double_sided = false;
};

struct CharacterModel {
    std::vector<CharacterJoint> joints;
    std::vector<CharacterVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<CharacterMaterial> materials;
    float root[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    uint32_t tex_size = 0, tex_mips = 0;
    // Per material, every mip from the largest, RGBA8: albedo (sRGB) and
    // (normal x, normal y down the texture, roughness, 1).
    std::vector<std::vector<uint8_t> > tex_a, tex_b;
    float height = 1.8f;   // the rest pose's top, metres

    bool load(const std::string& path, std::string& error);
    int  find(const char* name) const;
    // Drop the texture data once it is on the GPU.
    void release_textures() { tex_a.clear(); tex_b.clear(); tex_a.shrink_to_fit(); tex_b.shrink_to_fit(); }
};

// A clip retargeted onto one character: per frame, per CHARACTER joint, the
// local rotation (x y z w); and the pelvis's local translation, scaled by
// hip height. Joints the library lacks keep the character's rest.
struct AnimClip {
    std::string name;
    uint32_t frames = 0;
    float duration = 0.0f;
    float speed = 0.0f;     // m/s the clip covers with root motion
    float phase0 = 0.0f;    // the cycle fraction where the left foot leads
    bool  loop = false;
    std::vector<float> q;       // frames * joints * 4
    std::vector<float> pelvis;  // frames * 3
};

struct AnimLibrary {
    std::vector<AnimClip> clips;
    std::string note;
    bool load(const std::string& path, const CharacterModel& target, std::string& error);
    int  find(const char* name) const;
};

// A pose under construction: every joint's local rotation, the pelvis's
// local translation.
struct AnimPose {
    std::vector<float> q;
    float pelvis[3] = {0, 0, 0};
    void resize(size_t joints) { q.assign(joints * 4, 0.0f); }
};

// What physics did this frame: the animator's whole input.
struct LocomotionInput {
    float dt = 0.0f;
    float speed = 0.0f;       // horizontal, m/s, measured from the feet
    bool  grounded = true;
    bool  swimming = false;
    float vertical = 0.0f;    // m/s, + up
    float time = 0.0f;
    bool  torch = false;      // carrying one: the right arm holds it up
    bool  crouch = false;     // crouched: Crouch_Idle_Loop to Crouch_Fwd_Loop by speed
};

class CharacterAnimator {
public:
    void bind(const CharacterModel* m, const AnimLibrary* lib = nullptr);
    bool ready() const { return model_ != nullptr; }
    bool clips() const { return lib_ != nullptr; }
    // A swing of the tool in hand: the upper body plays a clip over the
    // legs. `clip` names it (the tool's own, items.json `tool.anim`;
    // nullptr for the default axe swing) and `len` how much of it to play
    // (0 for all of it) -- UAL's Sword_Attack holds a second strike after
    // the first, which an axe should not show.
    void trigger_action(const char* clip = nullptr, float len = 0.0f);
    // Instruments: how far into the swing, and the landing; -1 when idle.
    void set_action_time(float t) { action_t_ = t; }

    // Advance the gait and pose the skeleton. Then skin() gives the bone
    // matrices for a character standing at feet (x, y, z) facing `yaw`
    // (forward (sin yaw, 0, cos yaw), as Camera's).
    void update(const LocomotionInput& in);
    // 3 x 4 rows per joint, model -> world, ready for the shader.
    void skin(const float feet[3], float yaw, std::vector<float>& out) const;
    // Where joint j is in the world, for the same placement. False for -1.
    bool joint_world(int j, const float feet[3], float yaw, float out[3]) const;

    // Instruments, for a report.
    float phase() const { return phase_; }
    void  set_phase(float p) { phase_ = p - std::floor(p); }
    float run_blend() const { return run_; }
    float air_blend() const { return air_; }
    float crouch_blend() const { return crouch_; }
    float foot_drop() const { return drop_; }

private:
    void apply(int j, const float q_model[4]);
    void finish_globals();
    void pose_procedural(const LocomotionInput& in);
    void pose_clips(const LocomotionInput& in);
    void sample(int clip, float time, AnimPose& out) const;
    void sample_phase(int clip, float phase, AnimPose& out) const;

    const CharacterModel* model_ = nullptr;
    const AnimLibrary* lib_ = nullptr;
    // Clips by role, -1 if the library lacks one.
    int c_idle_ = -1, c_walk_ = -1, c_jog_ = -1, c_sprint_ = -1, c_air_ = -1, c_land_ = -1;
    int c_swim_idle_ = -1, c_swim_fwd_ = -1, c_torch_ = -1, c_action_ = -1;
    int action_clip_ = -1;         // this swing's clip, -1 for c_action_
    float action_len_ = 0.0f;      // how much of it to play, 0 for all
    int c_crouch_idle_ = -1, c_crouch_fwd_ = -1;
    std::vector<uint8_t> upper_mask_, right_arm_mask_;
    AnimPose pose_a_, pose_b_, pose_c_;
    float idle_t_ = 0.0f, air_t_ = 0.0f, land_t_ = -1.0f, swim_t_ = 0.0f, swim_phase_ = 0.0f;
    float action_t_ = -1.0f, torch_ = 0.0f;
    float crouch_ = 0.0f, crouch_phase_ = 0.0f;
    bool  was_air_ = false;
    bool  pelvis_t_on_ = false;
    float pelvis_t_[3] = {0, 0, 0};
    // Per joint: this frame's local rotation, and the posed globals.
    std::vector<float> local_q_, global_m_;   // 4 per joint, 12 per joint
    float pelvis_offset_[3] = {0, 0, 0};       // model space
    float phase_ = 0.0f, move_ = 0.0f, run_ = 0.0f, air_ = 0.0f, swim_ = 0.0f;
    float drop_ = 0.0f;
    // Joints the animator touches, by name, -1 if the skeleton lacks one.
    int pelvis_ = -1, spine_[3] = {-1, -1, -1}, neck_ = -1, head_ = -1;
    int thigh_[2] = {-1, -1}, calf_[2] = {-1, -1}, foot_[2] = {-1, -1}, ball_[2] = {-1, -1};
    int clav_[2] = {-1, -1}, upper_[2] = {-1, -1}, lower_[2] = {-1, -1}, hand_[2] = {-1, -1};
    std::vector<int> fingers_[2], thumbs_[2];
    // The rotation that lowers each arm from the T-pose to hanging, model space.
    float arm_down_[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    float arm_half_[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};   // raised half way, for a jump
};

}  // namespace engine

#endif  // ENGINE_CHARACTER_MODEL_H
