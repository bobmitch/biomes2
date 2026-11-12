#include "engine/svdag/brick_store.h"

#include <cmath>
#include <cstring>

namespace engine {
namespace {

inline int floor_div(int a, int b) {
    // Voxel coordinates go negative -- the world is centred on the origin
    // (invariant 6) -- and C's integer division truncates toward zero, which
    // puts voxel -1 and voxel 0 in the same brick.
    return a >= 0 ? a / b : -(((-a) + b - 1) / b);
}

inline int floor_mod(int a, int b) {
    const int m = a % b;
    return m < 0 ? m + b : m;
}

inline int voxel_of(float w) {
    return static_cast<int>(std::floor(w / kEditVoxel));
}

}  // namespace

Brick& BrickStore::touch(const BrickKey& k, bool& created) {
    std::unordered_map<BrickKey, Brick, BrickKeyHash>::iterator it = bricks_.find(k);
    if (it != bricks_.end()) {
        created = false;
        return it->second;
    }
    created = true;
    Brick& b = bricks_[k];
    std::memset(b.edited, 0, sizeof(b.edited));
    std::memset(b.material, 0, sizeof(b.material));
    // density is left alone on purpose: a voxel whose edited bit is clear is
    // never read, and zeroing 4096 floats per brick would be work for nothing.
    b.slot = next_slot_++;
    b.dirty = true;
    return b;
}

bool BrickStore::edit_sphere(float cx, float cy, float cz, float radius,
                             float strength, uint8_t material,
                             float out_min[3], float out_max[3]) {
    if (radius <= 0.0f) return false;

    const int v0x = voxel_of(cx - radius), v1x = voxel_of(cx + radius);
    const int v0y = voxel_of(cy - radius), v1y = voxel_of(cy + radius);
    const int v0z = voxel_of(cz - radius), v1z = voxel_of(cz + radius);

    bool any = false;
    for (int vz = v0z; vz <= v1z; ++vz) {
        for (int vy = v0y; vy <= v1y; ++vy) {
            for (int vx = v0x; vx <= v1x; ++vx) {
                const float wx = (float(vx) + 0.5f) * kEditVoxel;
                const float wy = (float(vy) + 0.5f) * kEditVoxel;
                const float wz = (float(vz) + 0.5f) * kEditVoxel;
                const float dx = wx - cx, dy = wy - cy, dz = wz - cz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > radius) continue;

                BrickKey key;
                key.x = floor_div(vx, kBrickSide);
                key.y = floor_div(vy, kBrickSide);
                key.z = floor_div(vz, kBrickSide);
                bool created = false;
                Brick& b = touch(key, created);

                const int lx = floor_mod(vx, kBrickSide);
                const int ly = floor_mod(vy, kBrickSide);
                const int lz = floor_mod(vz, kBrickSide);
                const int li = (lz * kBrickSide + ly) * kBrickSide + lx;

                // Absolute, and feathered to the rim so the carve does not end
                // in a step one voxel wide. The sign is what matters: negative
                // is air, positive is rock.
                const float t = radius > 0.0f ? d / radius : 1.0f;
                b.density[li] = strength * (1.0f - t * t);
                b.material[li] = material;
                b.edited[li >> 5] |= (1u << (li & 31));
                if (!b.dirty) {
                    b.dirty = true;
                    dirty_.push_back(key);
                } else if (created) {
                    dirty_.push_back(key);
                }
                any = true;
            }
        }
    }
    if (!any) return false;

    out_min[0] = cx - radius; out_min[1] = cy - radius; out_min[2] = cz - radius;
    out_max[0] = cx + radius; out_max[1] = cy + radius; out_max[2] = cz + radius;
    return true;
}

void BrickStore::bricks_in_box(const float lo[3], const float hi[3],
                               std::vector<uint32_t>& out_slots,
                               std::vector<int>& out_origins) const {
    out_slots.clear();
    out_origins.clear();
    if (bricks_.empty()) return;

    const int b0x = floor_div(voxel_of(lo[0]), kBrickSide);
    const int b1x = floor_div(voxel_of(hi[0]), kBrickSide);
    const int b0y = floor_div(voxel_of(lo[1]), kBrickSide);
    const int b1y = floor_div(voxel_of(hi[1]), kBrickSide);
    const int b0z = floor_div(voxel_of(lo[2]), kBrickSide);
    const int b1z = floor_div(voxel_of(hi[2]), kBrickSide);

    for (int bz = b0z; bz <= b1z; ++bz) {
        for (int by = b0y; by <= b1y; ++by) {
            for (int bx = b0x; bx <= b1x; ++bx) {
                BrickKey k;
                k.x = bx; k.y = by; k.z = bz;
                std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it =
                    bricks_.find(k);
                if (it == bricks_.end()) continue;
                out_slots.push_back(it->second.slot);
                out_origins.push_back(bx);
                out_origins.push_back(by);
                out_origins.push_back(bz);
            }
        }
    }
}

void BrickStore::take_dirty(std::vector<BrickKey>& out) {
    out.clear();
    out.swap(dirty_);
    for (size_t i = 0; i < out.size(); ++i) {
        std::unordered_map<BrickKey, Brick, BrickKeyHash>::iterator it = bricks_.find(out[i]);
        if (it != bricks_.end()) it->second.dirty = false;
    }
}

const Brick* BrickStore::find(const BrickKey& k) const {
    std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.find(k);
    return it == bricks_.end() ? nullptr : &it->second;
}

}  // namespace engine
