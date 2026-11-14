#include "engine/physics/physics_world.h"
#include "engine/render/water_wave.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace engine {
namespace {

// Jolt talks to the host through these. Both are no-ops here: the engine has
// no log window yet, and a trace that goes nowhere is better than one that
// opens a message box in the middle of a frame-rate measurement.
void trace_impl(const char*, ...) {}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        map_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        map_[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return map_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l == BroadPhaseLayers::NON_MOVING ? "terrain" : "moving";
    }
#endif
private:
    JPH::BroadPhaseLayer map_[Layers::NUM_LAYERS] = {
        BroadPhaseLayers::NON_MOVING, BroadPhaseLayers::MOVING};
};

class ObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        // Terrain against terrain is the pair worth excluding: with hundreds of
        // static chunk bodies it is every pair in the world.
        if (a == Layers::NON_MOVING) return b == BroadPhaseLayers::MOVING;
        return true;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true;
    }
};

}  // namespace

struct PhysicsWorld::Impl {
    JPH::PhysicsSystem        system;
    JPH::TempAllocatorImpl    temp{16 * 1024 * 1024};
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BPLayerInterfaceImpl      bp_layers;
    ObjectVsBPFilter          obj_vs_bp;
    ObjectPairFilter          obj_pair;
    JPH::Ref<JPH::CharacterVirtual> character;
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;

bool PhysicsWorld::init(std::string& error) {
    // BEFORE Impl is constructed, not after. Impl holds a TempAllocatorImpl
    // that takes 16 MB in its constructor through Jolt's allocator hooks, and
    // those hooks are null until RegisterDefaultAllocator installs them --
    // constructing Impl first segfaults on the first allocation.
    JPH::RegisterDefaultAllocator();
    JPH::Trace = trace_impl;
    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    impl_.reset(new Impl());

    impl_->jobs.reset(new JPH::JobSystemThreadPool(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2));

    // Sized for the resident set: one static body per chunk, 768 slots, plus
    // room for what B6 and B7 add.
    impl_->system.Init(4096, 0, 8192, 2048,
                       impl_->bp_layers, impl_->obj_vs_bp, impl_->obj_pair);
    impl_->system.SetGravity(JPH::Vec3(0.0f, -22.0f, 0.0f));

    // The capsule is offset so the character's POSITION is its feet. Jolt's
    // own shape origin is the capsule centre, and a character controller whose
    // position is halfway up itself makes every ground comparison off by a
    // constant that is easy to get wrong in only some places.
    JPH::RotatedTranslatedShapeSettings offset(
        JPH::Vec3(0.0f, half_height_ + radius_, 0.0f), JPH::Quat::sIdentity(),
        new JPH::CapsuleShape(half_height_, radius_));
    JPH::ShapeSettings::ShapeResult shape = offset.Create();
    if (shape.HasError()) {
        error = std::string("character shape: ") + shape.GetError().c_str();
        return false;
    }

    JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
    cs->mShape = shape.Get();
    cs->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius_);
    impl_->character = new JPH::CharacterVirtual(cs, JPH::RVec3(0, 0, 0),
                                                 JPH::Quat::sIdentity(), 0,
                                                 &impl_->system);
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    for (std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash>::iterator it = bodies_.begin();
         it != bodies_.end(); ++it) {
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
    }
    bodies_.clear();
    impl_->character = nullptr;
    impl_.reset();
    // Factory and the registered types are process-wide and deliberately left
    // standing: tearing them down while another PhysicsWorld could exist is a
    // worse failure than leaking them at exit.
}

void PhysicsWorld::set_chunk(const ChunkKey& key, const float origin[3], float voxel,
                             const float* heights) {
    if (!impl_) return;

    // The kernel's sentinel becomes Jolt's. Translating here rather than in
    // the shader keeps the kernel ignorant of which physics engine is behind
    // it, which is the only reason the kernel did not have to change when this
    // replaced the hand-written height query.
    std::vector<float> samples(kHeightSamples);
    bool any = false;
    for (UINT i = 0; i < kHeightSamples; ++i) {
        if (heights[i] >= 1.0e29f) {
            samples[i] = JPH::HeightFieldShapeConstants::cNoCollisionValue;
        } else {
            samples[i] = heights[i];
            any = true;
        }
    }
    remove_chunk(key);
    if (!any) return;   // a chunk of air carries no shape

    // Offset and scale put the samples in world space: the shape is at the
    // origin and carries its own placement, so the body never has to.
    // kHeightGrid is 34, so 34 / mBlockSize = 17 blocks -- allowed (Jolt asks
    // only for at least 2), though a power of two would pack better.
    JPH::HeightFieldShapeSettings hs(samples.data(),
                                     JPH::Vec3(origin[0], 0.0f, origin[2]),
                                     JPH::Vec3(voxel, 1.0f, voxel),
                                     kHeightGrid);
    hs.mBlockSize = 2;
    JPH::ShapeSettings::ShapeResult r = hs.Create();
    if (r.HasError()) return;

    JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Static, Layers::NON_MOVING);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
    if (!id.IsInvalid()) {
        bodies_[key] = id;
        body_lod_[id.GetIndexAndSequenceNumber()] = key.lod;
    }
}

void PhysicsWorld::set_chunk_mesh(const ChunkKey& key, const float* verts,
                                  unsigned nverts, const unsigned* idx,
                                  unsigned nidx) {
    if (!impl_ || nverts == 0 || nidx < 3) return;

    JPH::VertexList vl;
    vl.resize(nverts);
    for (unsigned i = 0; i < nverts; ++i) {
        vl[i] = JPH::Float3(verts[size_t(i) * 3 + 0], verts[size_t(i) * 3 + 1],
                            verts[size_t(i) * 3 + 2]);
    }

    JPH::IndexedTriangleList tl;
    tl.reserve(nidx / 3);
    for (unsigned t = 0; t + 2 < nidx; t += 3) {
        const unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
        // Surface nets emits degenerate triangles wherever two of a quad's
        // four cell vertices land on the same point. Jolt would reject or
        // silently drop them; dropping them here keeps the shape's triangle
        // count meaningful.
        if (a == b || b == c || a == c) continue;
        if (a >= nverts || b >= nverts || c >= nverts) continue;
        tl.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    if (tl.empty()) return;

    JPH::MeshShapeSettings ms(vl, tl);
    ms.mMaxTrianglesPerLeaf = 8;
    JPH::ShapeSettings::ShapeResult r = ms.Create();
    if (r.HasError()) return;

    remove_chunk(key);
    JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Static, Layers::NON_MOVING);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
    if (!id.IsInvalid()) {
        bodies_[key] = id;
        body_lod_[id.GetIndexAndSequenceNumber()] = key.lod;
        body_is_mesh_[id.GetIndexAndSequenceNumber()] = 1;
        ++mesh_bodies_;
    }
}

void PhysicsWorld::remove_chunk(const ChunkKey& key) {
    if (!impl_) return;
    std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash>::iterator it = bodies_.find(key);
    if (it == bodies_.end()) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::uint32 bid = it->second.GetIndexAndSequenceNumber();
    if (body_is_mesh_.erase(bid)) --mesh_bodies_;
    body_lod_.erase(bid);
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);
    bodies_.erase(it);
}

PhysicsWorld::UpProbe PhysicsWorld::probe_up(float x, float y, float z, float len) const {
    UpProbe p;
    if (!impl_) return p;
    const JPH::RRayCast ray(JPH::RVec3(x, y, z), JPH::Vec3(0.0f, len, 0.0f));
    for (int pass = 0; pass < 2; ++pass) {
        JPH::RayCastSettings rs;
        rs.SetBackFaceMode(pass == 0 ? JPH::EBackFaceMode::IgnoreBackFaces
                                     : JPH::EBackFaceMode::CollideWithBackFaces);
        JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> col;
        impl_->system.GetNarrowPhaseQuery().CastRay(ray, rs, col);
        if (!col.HadHit()) continue;
        const float d = col.mHit.mFraction * len;
        if (pass == 0) { p.hit_front = true; p.dist_front = d; }
        else {
            p.hit_any = true; p.dist_any = d;
            const JPH::uint32 bid = col.mHit.mBodyID.GetIndexAndSequenceNumber();
            std::unordered_map<JPH::uint32, int>::const_iterator l = body_lod_.find(bid);
            p.lod = l == body_lod_.end() ? -1 : l->second;
            p.is_mesh = body_is_mesh_.count(bid) ? 1 : 0;
        }
    }
    return p;
}

int PhysicsWorld::chunk_shape(const ChunkKey& k) const {
    std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash>::const_iterator it = bodies_.find(k);
    if (it == bodies_.end()) return 0;
    return body_is_mesh_.count(it->second.GetIndexAndSequenceNumber()) ? 2 : 1;
}

int PhysicsWorld::character_ground_lod() const {
    if (!impl_ || !impl_->character) return -1;
    const JPH::BodyID id = impl_->character->GetGroundBodyID();
    if (id.IsInvalid()) return -1;
    std::unordered_map<JPH::uint32, int>::const_iterator it =
        body_lod_.find(id.GetIndexAndSequenceNumber());
    return it == body_lod_.end() ? -1 : it->second;
}

void PhysicsWorld::place_character(float x, float y, float z) {
    if (!impl_ || !impl_->character) return;
    impl_->character->SetPosition(JPH::RVec3(x, y, z));
    impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
    // A teleport is not motion: the next step must not read the jump in
    // water height between the old place and the new as the sea rising.
    sea_here_ = false;
    medium_ = Medium::Dry;
}

void PhysicsWorld::launch_character(float vy) {
    pending_launch_ = vy;
}

void PhysicsWorld::update(float dt, float vx, float vz, bool jump, float swim) {
    if (!impl_ || !impl_->character) return;
    if (dt > 0.05f) dt = 0.05f;

    JPH::CharacterVirtual* c = impl_->character;
    const JPH::Vec3 gravity = impl_->system.GetGravity();

    // Where the water is, from the same function the shader draws. Asked at
    // the start of the step, so a swimmer is judged against the surface it is
    // in now rather than one it will be in after moving -- a frame's lag at
    // most, which the buoyancy spring cannot tell from the wave itself.
    const bool on_ground = c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    {
        const JPH::RVec3 at = c->GetPosition();
        const float fx = float(at.GetX()), fy = float(at.GetY()), fz = float(at.GetZ());
        const bool was_here = sea_here_;
        const float was_y = water_y_;
        sea_here_ = false;
        submerged_ = 0.0f;
        if (sea_.on && column_height_) {
            // water.hlsl's clip(depth + 0.02): drawn wherever the base column
            // is below sea level, give or take its two centimetres.
            sea_here_ = column_height_(fx, fz) < sea_.level + 0.02f;
            if (sea_here_) {
                water_y_ = sea_.level + ww_height_at(fx, fz, sea_.time, sea_.state);
                float sub = water_y_ - fy;
                const float body = 2.0f * (half_height_ + radius_);
                submerged_ = sub < 0.0f ? 0.0f : (sub > body ? body : sub);
            }
        }
        // How fast the surface under the character is rising, ALONG ITS PATH
        // -- the wave's own motion and the swimmer crossing its slope, both of
        // which the body has to follow. Clamped because a step that streams
        // in after a stall has a meaningless dt.
        water_rate_ = 0.0f;
        if (sea_here_ && was_here && dt > 1e-4f) {
            const float r = (water_y_ - was_y) / dt;
            water_rate_ = r > 10.0f ? 10.0f : (r < -10.0f ? -10.0f : r);
        }

        // In the water with nothing underfoot is swimming, however little of
        // the body is wet: a swimmer lifted clear by a crest that then drops
        // away is still a swimmer, and calling that wading turned off the
        // buoyancy exactly when it was about to be needed. Standing on the
        // bottom is wading until the water is deep enough to float in.
        if (submerged_ <= 0.0f) {
            medium_ = Medium::Dry;
        } else if (!on_ground) {
            medium_ = Medium::Swimming;
        } else if (medium_ == Medium::Swimming) {
            if (submerged_ < kSwimLeave) medium_ = Medium::Wading;
        } else {
            medium_ = submerged_ > kSwimEnter ? Medium::Swimming : Medium::Wading;
        }
    }

    JPH::Vec3 v = c->GetLinearVelocity();
    float hscale = 1.0f;
    if (medium_ != Medium::Dry) {
        // Water drags at the legs: full speed at the waterline down to
        // swimming speed at the depth where swimming takes over. One curve
        // for both modes, so a frame that flickers between them in the
        // shallows does not jerk the speed.
        const float f = submerged_ / kSwimEnter;
        hscale = 1.0f - (1.0f - kSwimSpeed) * (f > 1.0f ? 1.0f : f);
    }
    if (medium_ == Medium::Swimming) {
        // Archimedes, linearised: lift in proportion to how much of the body
        // is under, balancing gravity exactly at kFloatDepth. That is a spring
        // of stiffness g / kFloatDepth -- omega 4.0 rad/s at this gravity --
        // so the drag below is about 0.74 of critical: one small overshoot
        // when you drop in, then it rides the swell. Gravity here is 22, not
        // 9.81, so these are tuned to the world, not to the textbook.
        const float g = gravity.GetY();
        const float wet = submerged_ / kFloatDepth;
        float ay = g * (1.0f - wet);
        float intent = swim + (jump ? 1.0f : 0.0f);
        if (intent > 1.0f) intent = 1.0f;
        if (intent < -1.0f) intent = -1.0f;
        // Thrust pushes against water, so it stops working once the body is
        // nearly out of it: a swimmer can lunge onto a ledge about a metre up
        // and cannot fly.
        if (intent < 0.0f || submerged_ > 0.3f) ay += 30.0f * intent;
        // Drag acts on velocity RELATIVE TO THE WATER. The first version
        // damped absolute velocity, which in a storm is a force holding the
        // swimmer down while a crest rises past at 3 m/s: --swim at sea state
        // 1 measured the feet 1.9 m under on average against a balance of
        // 1.35, and the eye under for 60% of frames. Scaled by how much of the
        // body is in the water, so a swimmer thrown clear falls like one.
        const float drag = 6.0f * (wet > 1.0f ? 1.0f : wet);
        v.SetY((v.GetY() + (ay + drag * water_rate_) * dt) / (1.0f + drag * dt));
    } else {
        if (on_ground) {
            v.SetY(0.0f);
            if (jump) v.SetY(7.5f);
        } else {
            v += gravity * dt;
        }
    }
    v.SetX(vx * hscale);
    v.SetZ(vz * hscale);
    if (pending_launch_ != 0.0f) {
        v.SetY(pending_launch_);
        pending_launch_ = 0.0f;
    }
    c->SetLinearVelocity(v);

    // ExtendedUpdate is what makes this a character rather than a falling
    // capsule: mStickToFloorStepDown (0.5 m by default) keeps it on the ground
    // over a crest instead of launching, and mWalkStairsStepUp lets it climb.
    // The hand-written version of B4 reached for the same step-down at 0.4 m
    // after measuring 483 airborne frames in 897 without it.
    JPH::CharacterVirtual::ExtendedUpdateSettings s;
    // ...and it is exactly wrong for a swimmer: anything within half a metre
    // below gets snapped onto, so a float over shallow water would be dragged
    // down to walk the seabed -- the bug this whole mode exists to remove.
    if (medium_ == Medium::Swimming && submerged_ >= kSwimLeave) {
        s.mStickToFloorStepDown = JPH::Vec3::sZero();
    }
    c->ExtendedUpdate(dt, gravity, s,
                      impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                      impl_->system.GetDefaultLayerFilter(Layers::MOVING),
                      {}, {}, impl_->temp);

    // The static bodies still need the broadphase kept current as chunks come
    // and go, which is what this does with no dynamic bodies in the world.
    impl_->system.Update(dt, 1, &impl_->temp, impl_->jobs.get());
}

void PhysicsWorld::character_position(float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (!impl_ || !impl_->character) return;
    const JPH::RVec3 p = impl_->character->GetPosition();
    out[0] = float(p.GetX());
    out[1] = float(p.GetY());
    out[2] = float(p.GetZ());
}

bool PhysicsWorld::character_grounded() const {
    if (!impl_ || !impl_->character) return false;
    return impl_->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

bool PhysicsWorld::character_supported() const {
    if (!impl_ || !impl_->character) return false;
    return impl_->character->IsSupported();
}

}  // namespace engine
