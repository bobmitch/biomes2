#include "engine/physics/physics_world.h"
#include "engine/render/water_wave.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
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
    JPH::RefConst<JPH::Shape> character_shape;
    JPH::Ref<JPH::CharacterVirtualSettings> character_settings;
    std::unordered_map<int, JPH::Ref<JPH::CharacterVirtual> > walkers;
    std::unordered_map<uint32_t, JPH::Ref<JPH::TwoBodyConstraint> > hinges;
    // The player and every creature collide with each other.
    JPH::CharacterVsCharacterCollisionSimple cvc;
    std::vector<JPH::RefConst<JPH::Shape>> prop_shapes;   // add_prop_mesh
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
    impl_->character_settings = cs;
    impl_->cvc.Add(impl_->character);
    impl_->character->SetCharacterVsCharacterCollision(&impl_->cvc);
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    // Constraints first: a body cannot be destroyed out from under one.
    for (std::unordered_map<uint32_t, JPH::Ref<JPH::TwoBodyConstraint> >::iterator it = impl_->hinges.begin();
         it != impl_->hinges.end(); ++it) {
        impl_->system.RemoveConstraint(it->second);
    }
    impl_->hinges.clear();
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    for (std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash>::iterator it = bodies_.begin();
         it != bodies_.end(); ++it) {
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
    }
    bodies_.clear();
    for (std::unordered_map<uint32_t, JPH::BodyID>::iterator it = props_.begin(); it != props_.end(); ++it) {
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
    }
    props_.clear();
    for (auto& w : impl_->walkers) impl_->cvc.Remove(w.second);
    impl_->walkers.clear();
    walker_state_.clear();
    if (impl_->character) impl_->cvc.Remove(impl_->character);
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

int PhysicsWorld::add_prop_mesh(const float* verts, unsigned nverts, const unsigned* idx, unsigned nidx) {
    if (!impl_) { prop_error_ = "physics not initialised"; return -1; }
    JPH::VertexList vl;
    vl.resize(nverts);
    for (unsigned i = 0; i < nverts; ++i)
        vl[i] = JPH::Float3(verts[size_t(i) * 3 + 0], verts[size_t(i) * 3 + 1], verts[size_t(i) * 3 + 2]);
    JPH::IndexedTriangleList tl;
    tl.reserve(nidx / 3);
    for (unsigned t = 0; t + 2 < nidx; t += 3) {
        const unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a == b || b == c || a == c || a >= nverts || b >= nverts || c >= nverts) continue;
        tl.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    if (tl.empty()) { prop_error_ = "no triangles"; return -1; }
    JPH::MeshShapeSettings ms(vl, tl);
    JPH::ShapeSettings::ShapeResult r = ms.Create();
    if (r.HasError()) { prop_error_ = r.GetError().c_str(); return -1; }
    impl_->prop_shapes.push_back(r.Get());
    return int(impl_->prop_shapes.size()) - 1;
}

uint32_t PhysicsWorld::add_static_prop(int mesh, const float pos[3], const float rot[4], float scale) {
    if (!impl_ || mesh < 0 || mesh >= int(impl_->prop_shapes.size()) || scale <= 0.0f) return 0;
    JPH::RefConst<JPH::Shape> shape = new JPH::ScaledShape(impl_->prop_shapes[size_t(mesh)], JPH::Vec3::sReplicate(scale));
    JPH::BodyCreationSettings bcs(shape, JPH::RVec3(pos[0], pos[1], pos[2]),
                                  JPH::Quat(rot[0], rot[1], rot[2], rot[3]).Normalized(),
                                  JPH::EMotionType::Static, Layers::NON_MOVING);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return 0;
    const uint32_t h = id.GetIndexAndSequenceNumber();
    props_[h] = id;
    return h;
}

uint32_t PhysicsWorld::add_static_capsule(const float a[3], const float b[3], float radius) {
    if (!impl_) return 0;
    const JPH::Vec3 pa(a[0], a[1], a[2]), pb(b[0], b[1], b[2]);
    JPH::Vec3 axis = pb - pa;
    const float len = axis.Length();
    if (len < 1e-3f) return 0;
    axis /= len;
    // Jolt's capsule runs along Y; turn Y onto the axis.
    const JPH::Quat q = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis);
    const float half = len > 2.0f * radius ? 0.5f * len - radius : 0.01f;
    JPH::BodyCreationSettings bcs(new JPH::CapsuleShape(half, radius),
                                  JPH::RVec3(0.5f * (pa + pb)), q,
                                  JPH::EMotionType::Static, Layers::NON_MOVING);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return 0;
    const uint32_t h = id.GetIndexAndSequenceNumber();
    props_[h] = id;
    return h;
}

uint32_t PhysicsWorld::add_log(const float pos[3], const float rot[4], float trunk_r, float y0,
                               float y1, float crown_y, float crown_r, const float spin[3],
                               bool asleep) {
    if (!impl_) return 0;
    JPH::StaticCompoundShapeSettings cs;
    {
        const float len = y1 - y0;
        const float half = len > 2.0f * trunk_r ? 0.5f * len - trunk_r : 0.01f;
        JPH::CapsuleShapeSettings* trunk = new JPH::CapsuleShapeSettings(half, trunk_r);
        trunk->SetDensity(650.0f);
        cs.AddShape(JPH::Vec3(0.0f, 0.5f * (y0 + y1), 0.0f), JPH::Quat::sIdentity(), trunk);
    }
    if (crown_r > 0.05f) {
        JPH::SphereShapeSettings* crown = new JPH::SphereShapeSettings(crown_r);
        crown->SetDensity(6.0f);
        cs.AddShape(JPH::Vec3(0.0f, crown_y, 0.0f), JPH::Quat::sIdentity(), crown);
    }
    JPH::ShapeSettings::ShapeResult r = cs.Create();
    if (r.HasError()) return 0;
    JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3(pos[0], pos[1], pos[2]),
                                  JPH::Quat(rot[0], rot[1], rot[2], rot[3]).Normalized(),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    bcs.mFriction = 0.8f;
    bcs.mRestitution = 0.05f;
    bcs.mLinearDamping = 0.1f;
    bcs.mAngularDamping = 0.6f;    // a crown rolls on its sphere for ever at 0.35
    bcs.mAngularVelocity = JPH::Vec3(spin[0], spin[1], spin[2]);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        bcs, asleep ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) return 0;
    const uint32_t h = id.GetIndexAndSequenceNumber();
    props_[h] = id;
    ++dynamic_props_;
    return h;
}

uint32_t PhysicsWorld::add_piece(const float pos[3], const float rot[4], float radius,
                                 const float a[3], const float b[3], const float vel[3],
                                 const float spin[3], bool asleep) {
    if (!impl_) return 0;
    const JPH::Vec3 pa(a[0], a[1], a[2]), pb(b[0], b[1], b[2]);
    JPH::Vec3 axis = pb - pa;
    const float len = axis.Length();
    if (len < 1e-3f || radius <= 0.0f) return 0;
    axis /= len;
    // An EIGHT-SIDED PRISM, not a capsule. A capsule is a perfect roller:
    // on any slope gravity beats the damping and the log creeps downhill
    // for ever, never sleeping (--fell: still turning at 3.7 rad/s twelve
    // seconds after it fell). A real log stops because it is not round, and
    // so does this one -- rolling onto the next facet has to lift it, which
    // on gentle ground it cannot do. The faces are circumscribed, so the
    // drawn trunk is inscribed in the shape.
    const int sides = 8;
    const float circum = radius / std::cos(3.14159265f / float(sides));
    JPH::Vec3 u = JPH::Vec3::sAxisX().Cross(axis);
    if (u.LengthSq() < 1e-6f) u = JPH::Vec3::sAxisZ().Cross(axis);
    u = u.Normalized();
    const JPH::Vec3 v = axis.Cross(u);
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(size_t(sides) * 2);
    for (int i = 0; i < sides; ++i) {
        const float ang = 6.28318531f * (float(i) + 0.5f) / float(sides);
        const JPH::Vec3 off = u * (std::cos(ang) * circum) + v * (std::sin(ang) * circum);
        pts.push_back(pa + off);
        pts.push_back(pb + off);
    }
    // Points are already in the body's frame -- which is the MODEL's frame,
    // so the draw and physics agree without a second transform.
    JPH::ConvexHullShapeSettings hull(pts, (std::min)(0.02f, radius * 0.1f));
    hull.SetDensity(650.0f);
    JPH::ShapeSettings::ShapeResult r = hull.Create();
    if (r.HasError()) return 0;
    JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3(pos[0], pos[1], pos[2]),
                                  JPH::Quat(rot[0], rot[1], rot[2], rot[3]).Normalized(),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    // 0.7, not 0.9: wood on grassy soil is 0.6-0.8, and the difference is
    // what decides whether a person can shove a round of firewood at all.
    // At 0.9 a 167 kg round needs 1.5 kN to break away and nothing short of
    // a winch moved; the log's own prism is what stops it rolling, not this.
    bcs.mFriction = 0.7f;
    bcs.mRestitution = 0.02f;
    bcs.mLinearDamping = 0.25f;
    // Low, because the PRISM is what stops a log now, not drag: an eight-
    // sided one has to lift its own weight over an edge to keep rolling and
    // loses the normal component of the impact at every facet. 1.6 was
    // picked while fighting a capsule, which damping cannot stop at all,
    // and it left a log on steep ground stopping limply.
    bcs.mAngularDamping = 0.5f;
    bcs.mLinearVelocity = JPH::Vec3(vel[0], vel[1], vel[2]);
    bcs.mAngularVelocity = JPH::Vec3(spin[0], spin[1], spin[2]);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        bcs, asleep ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) return 0;
    const uint32_t h = id.GetIndexAndSequenceNumber();
    props_[h] = id;
    ++dynamic_props_;
    return h;
}

uint32_t PhysicsWorld::add_debris(const float pos[3], const float rot[4], float radius,
                                  float density, const float vel[3], const float spin[3]) {
    if (!impl_ || radius <= 0.01f) return 0;
    // An ICOSAHEDRON, for the same reason a log is a prism: a sphere is the
    // perfect roller and no damping stops one on a slope. A tangle of
    // branches sprawls where it lands, and twenty faces is what that costs
    // -- a regular icosahedron sits still on any slope under 21 degrees
    // (the face's inradius over the solid's, atan(0.382)), which is the
    // octagonal trunk's 22.5 within a degree and a half. The vertices are
    // ON the nominal sphere, so the hull is strictly inside it and clumps
    // that did not overlap before still do not.
    static const float kPhi = 0.8506508f, kOne = 0.5257311f;   // (1, phi) / |(1, phi)|
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(12);
    for (int s1 = -1; s1 <= 1; s1 += 2)
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            const float a = float(s1) * kOne * radius, b = float(s2) * kPhi * radius;
            pts.push_back(JPH::Vec3(0.0f, a, b));
            pts.push_back(JPH::Vec3(a, b, 0.0f));
            pts.push_back(JPH::Vec3(b, 0.0f, a));
        }
    JPH::ConvexHullShapeSettings ss(pts, (std::min)(0.05f, radius * 0.1f));
    ss.SetDensity(density > 0.1f ? density : 0.1f);
    JPH::ShapeSettings::ShapeResult r = ss.Create();
    if (r.HasError()) return 0;
    JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3(pos[0], pos[1], pos[2]),
                                  JPH::Quat(rot[0], rot[1], rot[2], rot[3]).Normalized(),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    bcs.mFriction = 0.95f;
    bcs.mRestitution = 0.0f;
    bcs.mLinearDamping = 0.5f;     // a crown is a parachute, not a cannonball
    bcs.mAngularDamping = 0.9f;    // ... and it must stop rolling, or it never fades
    bcs.mLinearVelocity = JPH::Vec3(vel[0], vel[1], vel[2]);
    bcs.mAngularVelocity = JPH::Vec3(spin[0], spin[1], spin[2]);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
    if (id.IsInvalid()) return 0;
    const uint32_t h = id.GetIndexAndSequenceNumber();
    props_[h] = id;
    ++dynamic_props_;
    return h;
}

void PhysicsWorld::remove_body(uint32_t handle) {
    if (!impl_) return;
    std::unordered_map<uint32_t, JPH::BodyID>::iterator it = props_.find(handle);
    if (it == props_.end()) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    if (bi.GetMotionType(it->second) == JPH::EMotionType::Dynamic && dynamic_props_ > 0) --dynamic_props_;
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);
    props_.erase(it);
}

bool PhysicsWorld::body_pose(uint32_t handle, float pos[3], float rot[4], bool& active) const {
    if (!impl_) return false;
    std::unordered_map<uint32_t, JPH::BodyID>::const_iterator it = props_.find(handle);
    if (it == props_.end()) return false;
    const JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    JPH::RVec3 p;
    JPH::Quat q;
    bi.GetPositionAndRotation(it->second, p, q);
    pos[0] = float(p.GetX()); pos[1] = float(p.GetY()); pos[2] = float(p.GetZ());
    rot[0] = q.GetX(); rot[1] = q.GetY(); rot[2] = q.GetZ(); rot[3] = q.GetW();
    active = bi.IsActive(it->second);
    return true;
}

bool PhysicsWorld::body_velocity(uint32_t handle, float lin[3], float ang[3]) const {
    if (!impl_) return false;
    std::unordered_map<uint32_t, JPH::BodyID>::const_iterator it = props_.find(handle);
    if (it == props_.end()) return false;
    const JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::Vec3 v = bi.GetLinearVelocity(it->second), w = bi.GetAngularVelocity(it->second);
    lin[0] = v.GetX(); lin[1] = v.GetY(); lin[2] = v.GetZ();
    ang[0] = w.GetX(); ang[1] = w.GetY(); ang[2] = w.GetZ();
    return true;
}

uint32_t PhysicsWorld::add_hinge(uint32_t body, uint32_t anchor, const float point[3],
                                 const float axis[3]) {
    if (!impl_) return 0;
    std::unordered_map<uint32_t, JPH::BodyID>::const_iterator a = props_.find(body);
    std::unordered_map<uint32_t, JPH::BodyID>::const_iterator b = props_.find(anchor);
    if (a == props_.end() || b == props_.end()) return 0;
    JPH::Vec3 ax(axis[0], axis[1], axis[2]);
    if (ax.LengthSq() < 1e-8f) return 0;
    ax = ax.Normalized();
    // Any direction square to the axis will do for the hinge's zero angle.
    JPH::Vec3 nrm = JPH::Vec3::sAxisY().Cross(ax);
    if (nrm.LengthSq() < 1e-8f) nrm = JPH::Vec3::sAxisX().Cross(ax);
    nrm = nrm.Normalized();
    JPH::HingeConstraintSettings cs;
    cs.mSpace = JPH::EConstraintSpace::WorldSpace;
    cs.mPoint1 = cs.mPoint2 = JPH::RVec3(point[0], point[1], point[2]);
    cs.mHingeAxis1 = cs.mHingeAxis2 = ax;
    cs.mNormalAxis1 = cs.mNormalAxis2 = nrm;
    JPH::TwoBodyConstraint* c = impl_->system.GetBodyInterface().CreateConstraint(&cs, a->second, b->second);
    if (!c) return 0;
    impl_->system.AddConstraint(c);
    const uint32_t h = ++next_hinge_;
    impl_->hinges[h] = c;
    return h;
}

void PhysicsWorld::remove_hinge(uint32_t handle) {
    if (!impl_ || !handle) return;
    std::unordered_map<uint32_t, JPH::Ref<JPH::TwoBodyConstraint> >::iterator it = impl_->hinges.find(handle);
    if (it == impl_->hinges.end()) return;
    impl_->system.RemoveConstraint(it->second);
    impl_->hinges.erase(it);
}

void PhysicsWorld::set_body_damping(uint32_t handle, float linear, float angular) {
    if (!impl_) return;
    std::unordered_map<uint32_t, JPH::BodyID>::iterator it = props_.find(handle);
    if (it == props_.end()) return;
    JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), it->second);
    if (!lock.Succeeded()) return;
    JPH::MotionProperties* mp = lock.GetBody().GetMotionProperties();
    if (!mp) return;
    mp->SetLinearDamping(linear < 0.0f ? 0.0f : linear);
    mp->SetAngularDamping(angular < 0.0f ? 0.0f : angular);
}

void PhysicsWorld::rest_body(uint32_t handle) {
    if (!impl_) return;
    std::unordered_map<uint32_t, JPH::BodyID>::iterator it = props_.find(handle);
    if (it == props_.end()) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    bi.SetLinearAndAngularVelocity(it->second, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    bi.DeactivateBody(it->second);
}

bool PhysicsWorld::body_point_velocity(uint32_t handle, const float p[3], float v[3]) const {
    if (!impl_) return false;
    std::unordered_map<uint32_t, JPH::BodyID>::const_iterator it = props_.find(handle);
    if (it == props_.end()) return false;
    const JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::Vec3 w = bi.GetPointVelocity(it->second, JPH::RVec3(p[0], p[1], p[2]));
    v[0] = w.GetX(); v[1] = w.GetY(); v[2] = w.GetZ();
    return true;
}

void PhysicsWorld::set_body_velocity(uint32_t handle, const float lin[3], const float ang[3]) {
    if (!impl_) return;
    std::unordered_map<uint32_t, JPH::BodyID>::iterator it = props_.find(handle);
    if (it == props_.end()) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    bi.SetLinearAndAngularVelocity(it->second, JPH::Vec3(lin[0], lin[1], lin[2]),
                                   JPH::Vec3(ang[0], ang[1], ang[2]));
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

float PhysicsWorld::probe_ray(const float from[3], const float dir[3], float len, int& prop) const {
    prop = 0;
    if (!impl_) return -1.0f;
    const JPH::RRayCast ray(JPH::RVec3(from[0], from[1], from[2]),
                            JPH::Vec3(dir[0], dir[1], dir[2]) * len);
    JPH::RayCastSettings rs;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> col;
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, rs, col);
    if (!col.HadHit()) return -1.0f;
    prop = props_.count(col.mHit.mBodyID.GetIndexAndSequenceNumber()) ? 1 : 0;
    return col.mHit.mFraction * len;
}

void PhysicsWorld::character_contacts(int& total, int& props) const {
    total = props = 0;
    if (!impl_ || !impl_->character) return;
    const JPH::CharacterVirtual::ContactList& cl = impl_->character->GetActiveContacts();
    for (size_t i = 0; i < cl.size(); ++i) {
        ++total;
        if (props_.count(cl[i].mBodyB.GetIndexAndSequenceNumber())) ++props;
    }
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
    player_.sea_here = false;
    player_.medium = Medium::Dry;
}

void PhysicsWorld::launch_character(float vy) {
    player_.pending_launch = vy;
}

void PhysicsWorld::set_push_strength(float newtons) {
    if (!impl_ || !impl_->character) return;
    impl_->character->SetMaxStrength(newtons > 0.0f ? newtons : 0.0f);
}

void PhysicsWorld::update(float dt, float vx, float vz, bool jump, float swim) {
    if (!impl_ || !impl_->character) return;
    if (dt > 0.05f) dt = 0.05f;
    step_body(impl_->character.GetPtr(), player_, dt, vx, vz, jump, swim);
    // What it ended up leaning on, and anything it walked into. Jolt's contact normal points towards the
    // character, so walking INTO something is a negative dot with where the
    // feet were asked to go. The character's own step has just run, so the
    // contact list is this frame's.
    push_ = Push();
    bumps_.clear();
    for (std::unordered_map<uint32_t, float>::iterator it = bump_cd_.begin(); it != bump_cd_.end();) {
        it->second -= dt;
        if (it->second <= 0.0f) it = bump_cd_.erase(it); else ++it;
    }
    {
        const float want_len = std::sqrt(vx * vx + vz * vz);
        const bool want_move = want_len > 1e-2f;
        const JPH::Vec3 want = want_move ? JPH::Vec3(vx / want_len, 0.0f, vz / want_len)
                                         : JPH::Vec3::sZero();
        const JPH::CharacterVirtual::ContactList& cl = impl_->character->GetActiveContacts();
        JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
        const float char_mass = impl_->character->GetMass();
        std::vector<uint32_t> touching;
        for (size_t i = 0; i < cl.size(); ++i) {
            if (cl[i].mMotionTypeB != JPH::EMotionType::Dynamic) continue;
            const uint32_t h = cl[i].mBodyB.GetIndexAndSequenceNumber();
            if (!props_.count(h)) continue;
            touching.push_back(h);
            // Jolt's contact normal points AT the character, so walking
            // into it is a negative dot with where the feet were asked to go.
            const float into = want_move ? -cl[i].mContactNormal.Dot(want) : 0.0f;
            JPH::RefConst<JPH::Shape> sh = bi.GetShape(cl[i].mBodyB);
            const float m = sh ? sh->GetMassProperties().mMass : 0.0f;
            if (into > 0.25f && m > push_.mass) {
                push_.on = true;
                push_.body = h;
                push_.mass = m;
                push_.speed = bi.GetLinearVelocity(cl[i].mBodyB).Length();
            }
            // The bump: a NEW contact only, off cooldown, coming in fast
            // enough to be a knock rather than a lean, and on a normal
            // within 45 degrees of level -- standing on a log must not
            // punt it into the ground.
            const bool fresh = std::find(touched_.begin(), touched_.end(), h) == touched_.end();
            if (!fresh || into <= 0.0f || bump_cd_.count(h)) continue;
            const float closing = into * want_len;
            if (closing < kBumpMinSpeed || std::fabs(cl[i].mContactNormal.GetY()) > 0.707f) continue;
            JPH::Vec3 dir = -cl[i].mContactNormal;
            dir.SetY(0.0f);
            const float dl = dir.Length();
            if (dl < 1e-3f) continue;
            dir /= dl;
            const float j = char_mass * closing * bump_transfer_;
            bi.AddImpulse(cl[i].mBodyB, dir * j, cl[i].mPosition);
            bump_cd_[h] = kBumpCooldown;
            Bump b;
            b.body = h; b.speed = closing; b.impulse = j; b.mass = m;
            bumps_.push_back(b);
        }
        touched_.swap(touching);
    }
    // The static bodies still need the broadphase kept current as chunks come
    // and go. A falling log is the one dynamic body; two collision steps at
    // the 50 ms clamp keep a toppling trunk from tunnelling a heightfield.
    impl_->system.Update(dt, dt > 0.02f ? 2 : 1, &impl_->temp, impl_->jobs.get());
}

int PhysicsWorld::add_walker(float x, float y, float z) {
    if (!impl_ || !impl_->character_settings) return 0;
    const int id = next_walker_++;
    JPH::Ref<JPH::CharacterVirtual> c = new JPH::CharacterVirtual(
        impl_->character_settings, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(), 0, &impl_->system);
    impl_->cvc.Add(c);
    c->SetCharacterVsCharacterCollision(&impl_->cvc);
    impl_->walkers[id] = c;
    walker_state_[id] = BodyState();
    return id;
}

void PhysicsWorld::remove_walker(int id) {
    if (!impl_) return;
    auto it = impl_->walkers.find(id);
    if (it == impl_->walkers.end()) return;
    impl_->cvc.Remove(it->second);
    impl_->walkers.erase(it);
    walker_state_.erase(id);
}

void PhysicsWorld::place_walker(int id, float x, float y, float z) {
    if (!impl_) return;
    auto it = impl_->walkers.find(id);
    if (it == impl_->walkers.end()) return;
    it->second->SetPosition(JPH::RVec3(x, y, z));
    it->second->SetLinearVelocity(JPH::Vec3::sZero());
    walker_state_[id].sea_here = false;
    walker_state_[id].medium = Medium::Dry;
}

void PhysicsWorld::step_walker(int id, float dt, float vx, float vz, bool jump, float swim) {
    if (!impl_) return;
    auto it = impl_->walkers.find(id);
    if (it == impl_->walkers.end()) return;
    if (dt > 0.05f) dt = 0.05f;
    if (dt <= 0.0f) return;
    step_body(it->second.GetPtr(), walker_state_[id], dt, vx, vz, jump, swim);
}

bool PhysicsWorld::walker_position(int id, float out[3]) const {
    if (!impl_) return false;
    auto it = impl_->walkers.find(id);
    if (it == impl_->walkers.end()) return false;
    const JPH::RVec3 p = it->second->GetPosition();
    out[0] = float(p.GetX());
    out[1] = float(p.GetY());
    out[2] = float(p.GetZ());
    return true;
}

bool PhysicsWorld::walker_supported(int id) const {
    if (!impl_) return false;
    auto it = impl_->walkers.find(id);
    return it != impl_->walkers.end() && it->second->IsSupported();
}

Medium PhysicsWorld::walker_medium(int id) const {
    auto it = walker_state_.find(id);
    return it == walker_state_.end() ? Medium::Dry : it->second.medium;
}

// One character's step: the water over it, buoyancy or gravity, the asked-for
// horizontal velocity, and Jolt's ExtendedUpdate. The player's and every
// creature's, the same rules.
void PhysicsWorld::step_body(void* character, BodyState& b, float dt, float vx, float vz, bool jump,
                             float swim) {
    JPH::CharacterVirtual* c = static_cast<JPH::CharacterVirtual*>(character);
    const JPH::Vec3 gravity = impl_->system.GetGravity();
    float& pending_launch_ = b.pending_launch;
    Medium& medium_ = b.medium;
    bool& sea_here_ = b.sea_here;
    float& water_y_ = b.water_y;
    float& submerged_ = b.submerged;
    float& water_rate_ = b.water_rate;

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
            // One rule everywhere (water_wave.h ww_sea_at): the water's
            // surface over the feet is sea level plus the deep sea's waves
            // weighted by the generated column's depth (none on land, so an
            // inland pit or a cave is still), plus the swash on the low beach
            // unless the feet are well under the generated ground (dug).
            const float ground = column_height_(fx, fz);
            const float depth = sea_.level - ground;
            water_y_ = sea_.level + ww_sea_at(fx, fz, sea_.time, sea_.sea, sea_.swash, depth, ground - fy);
            // "Here" while the surface is within a body's height under the
            // feet too: a swimmer a crest throws clear is still over water,
            // and the surface's rate (below) must not restart.
            const float body = 2.0f * (half_height_ + radius_);
            const float sub = water_y_ - fy;
            sea_here_ = sub > -body;
            submerged_ = sub < 0.0f ? 0.0f : (sub > body ? body : sub);
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
