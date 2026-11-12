#include "engine/physics/physics_world.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>

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
}

void PhysicsWorld::launch_character(float vy) {
    pending_launch_ = vy;
}

void PhysicsWorld::update(float dt, float vx, float vz, bool jump) {
    if (!impl_ || !impl_->character) return;
    if (dt > 0.05f) dt = 0.05f;

    JPH::CharacterVirtual* c = impl_->character;
    const JPH::Vec3 gravity = impl_->system.GetGravity();

    JPH::Vec3 v = c->GetLinearVelocity();
    const bool on_ground = c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    if (on_ground) {
        v.SetY(0.0f);
        if (jump) v.SetY(7.5f);
    } else {
        v += gravity * dt;
    }
    v.SetX(vx);
    v.SetZ(vz);
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
