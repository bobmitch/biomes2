#include "engine/svdag/brick_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {
namespace {

inline int floor_div(int a, int b) {
    // Voxel coordinates go negative -- the world is centred on the origin
    // (invariant 6) -- and C's integer division truncates toward zero, which
    // would put voxel -1 and voxel 0 in the same brick.
    return a >= 0 ? a / b : -(((-a) + b - 1) / b);
}

inline int floor_mod(int a, int b) {
    const int m = a % b;
    return m < 0 ? m + b : m;
}

inline int voxel_of(float w) {
    return static_cast<int>(std::floor(w / kEditVoxel));
}

template <typename T>
bool read_pod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
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
    dirty_.push_back(k);
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
                // Voxel v's value is what the mesher reads at the lattice
                // POINT v x kEditVoxel (surface_nets.hlsl floors the corner's
                // position), so that is where its distance is measured. It
                // was measured from the voxel's middle, which put every edit
                // a quarter voxel off towards +x +y +z -- nothing at 3.5 m,
                // the whole hole at 0.5.
                const float wx = float(vx) * kEditVoxel;
                const float wy = float(vy) * kEditVoxel;
                const float wz = float(vz) * kEditVoxel;
                const float dx = wx - cx, dy = wy - cy, dz = wz - cz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                // Strictly inside: a point ON the rim would be written as
                // exactly zero, which is neither air nor rock.
                if (d >= radius) continue;

                BrickKey key;
                key.x = floor_div(vx, kBrickSide);
                key.y = floor_div(vy, kBrickSide);
                key.z = floor_div(vz, kBrickSide);
                bool created = false;
                Brick& b = touch(key, created);
                if (!created) {
                    // Re-edited: the pool's copy is stale either way, and the
                    // list is deduplicated when it is drained.
                    dirty_.push_back(key);
                }

                const int lx = floor_mod(vx, kBrickSide);
                const int ly = floor_mod(vy, kBrickSide);
                const int lz = floor_mod(vz, kBrickSide);
                const int li = (lz * kBrickSide + ly) * kBrickSide + lx;

                // Absolute, and feathered to the rim so the carve does not end
                // in a step one voxel wide. The sign is what matters: negative
                // is air, positive is rock.
                //
                // Two guards make overlapping edits add up. The value never
                // reaches zero at the rim (kept a hair on the edit's side),
                // so a rim point that was air stays air instead of becoming
                // a film of "surface". And a point an earlier edit already
                // owns keeps whichever value goes further the same way:
                // digging the rim of one hole into the middle of another
                // must not shallow it.
                const float t = d / radius;
                float v = strength * (1.0f - t * t);
                const float kRim = 0.05f;
                if (strength < 0.0f) v = std::min(v, -kRim);
                else                 v = std::max(v, kRim);
                const bool owned = (b.edited[li >> 5] & (1u << (li & 31))) != 0;
                if (owned) v = strength < 0.0f ? std::min(v, b.density[li]) : std::max(v, b.density[li]);
                b.density[li] = v;
                b.material[li] = material;
                b.edited[li >> 5] |= (1u << (li & 31));
                mark_column(vx, vy, vz);
                any = true;
            }
        }
    }
    if (!any) return false;
    ++generation_;

    // One voxel past the sphere: an edited point on a chunk's border is also
    // its neighbour's ghost corner, and that neighbour must re-mesh too.
    const float pad = radius + kEditVoxel;
    out_min[0] = cx - pad; out_min[1] = cy - pad; out_min[2] = cz - pad;
    out_max[0] = cx + pad; out_max[1] = cy + pad; out_max[2] = cz + pad;
    return true;
}

bool BrickStore::edited_at(int vx, int vy, int vz, float& out_d) const {
    if (bricks_.empty()) return false;
    BrickKey key;
    key.x = floor_div(vx, kBrickSide);
    key.y = floor_div(vy, kBrickSide);
    key.z = floor_div(vz, kBrickSide);
    std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.find(key);
    if (it == bricks_.end()) return false;
    const int li = (floor_mod(vz, kBrickSide) * kBrickSide + floor_mod(vy, kBrickSide)) * kBrickSide +
                   floor_mod(vx, kBrickSide);
    if ((it->second.edited[li >> 5] & (1u << (li & 31))) == 0) return false;
    out_d = it->second.density[li];
    return true;
}

void BrickStore::bricks_in_box(const float lo[3], const float hi[3],
                               std::vector<BrickKey>& out) const {
    out.clear();
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
                if (bricks_.find(k) == bricks_.end()) continue;
                out.push_back(k);
            }
        }
    }
}

void BrickStore::take_dirty(std::vector<BrickKey>& out) {
    out.clear();
    out.swap(dirty_);
    // The same brick is pushed once per edited voxel, so the list is mostly
    // duplicates. Sorting 500 entries to collapse them is cheaper than
    // uploading one brick 500 times.
    std::sort(out.begin(), out.end(), [](const BrickKey& a, const BrickKey& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

const Brick* BrickStore::find(const BrickKey& k) const {
    std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.find(k);
    return it == bricks_.end() ? nullptr : &it->second;
}

size_t BrickStore::edited_voxels() const {
    size_t n = 0;
    for (std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.begin();
         it != bricks_.end(); ++it) {
        for (int w = 0; w < kBrickMaskWords; ++w) {
            uint32_t m = it->second.edited[w];
            while (m) { m &= m - 1; ++n; }
        }
    }
    return n;
}

bool BrickStore::save(const std::string& path, uint32_t field_id,
                      std::string& error) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "could not open " + path + " for writing"; return false; }

    const uint32_t magic = kSaveMagic, fmt = kSaveFormat;
    const uint32_t count = static_cast<uint32_t>(bricks_.size());
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&fmt, 4, 1, f);
    std::fwrite(&field_id, 4, 1, f);
    std::fwrite(&count, 4, 1, f);

    for (std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.begin();
         it != bricks_.end(); ++it) {
        const int32_t kx = it->first.x, ky = it->first.y, kz = it->first.z;
        std::fwrite(&kx, 4, 1, f);
        std::fwrite(&ky, 4, 1, f);
        std::fwrite(&kz, 4, 1, f);
        std::fwrite(it->second.edited, 4, kBrickMaskWords, f);

        // Only the voxels this brick owns. A dug hole touches a few hundred
        // voxels in bricks of four thousand, so writing all of them would make
        // the file around twenty times larger than the edits in it.
        for (int i = 0; i < kBrickVoxels; ++i) {
            if ((it->second.edited[i >> 5] & (1u << (i & 31))) == 0) continue;
            std::fwrite(&it->second.density[i], 4, 1, f);
            std::fwrite(&it->second.material[i], 1, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

bool BrickStore::load(const std::string& path, uint32_t expect_field_id,
                      std::string& error, bool& field_mismatch) {
    field_mismatch = false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "could not open " + path; return false; }

    uint32_t magic = 0, fmt = 0, field_id = 0, count = 0;
    if (!read_pod(f, magic) || !read_pod(f, fmt) || !read_pod(f, field_id) ||
        !read_pod(f, count)) {
        std::fclose(f);
        error = "truncated header in " + path;
        return false;
    }
    if (magic != kSaveMagic) { std::fclose(f); error = path + " is not an edit file"; return false; }
    if (fmt != kSaveFormat) {
        std::fclose(f);
        error = "edit file format " + std::to_string(fmt) + ", expected " +
                std::to_string(kSaveFormat);
        return false;
    }
    // Loaded anyway, but the caller is told. The edits are absolute, so they
    // are still exactly what the player made -- it is the terrain AROUND them
    // that a different field would have moved.
    if (field_id != expect_field_id) field_mismatch = true;

    bricks_.clear();
    dirty_.clear();
    column_top_.clear();
    ++generation_;
    for (uint32_t b = 0; b < count; ++b) {
        int32_t kx = 0, ky = 0, kz = 0;
        if (!read_pod(f, kx) || !read_pod(f, ky) || !read_pod(f, kz)) {
            std::fclose(f);
            error = "truncated brick in " + path;
            return false;
        }
        BrickKey key;
        key.x = kx; key.y = ky; key.z = kz;
        bool created = false;
        Brick& brick = touch(key, created);
        if (std::fread(brick.edited, 4, kBrickMaskWords, f) != kBrickMaskWords) {
            std::fclose(f);
            error = "truncated mask in " + path;
            return false;
        }
        for (int i = 0; i < kBrickVoxels; ++i) {
            if ((brick.edited[i >> 5] & (1u << (i & 31))) == 0) continue;
            if (!read_pod(f, brick.density[i]) || !read_pod(f, brick.material[i])) {
                std::fclose(f);
                error = "truncated voxels in " + path;
                return false;
            }
        }
    }
    std::fclose(f);
    // The column map is derived, never saved: rebuilt from what was read.
    column_top_.clear();
    for (std::unordered_map<BrickKey, Brick, BrickKeyHash>::const_iterator it = bricks_.begin();
         it != bricks_.end(); ++it) {
        for (int i = 0; i < kBrickVoxels; ++i) {
            if ((it->second.edited[i >> 5] & (1u << (i & 31))) == 0) continue;
            const int lx = i % kBrickSide, ly = (i / kBrickSide) % kBrickSide, lz = i / (kBrickSide * kBrickSide);
            mark_column(it->first.x * kBrickSide + lx, it->first.y * kBrickSide + ly, it->first.z * kBrickSide + lz);
        }
    }
    ++generation_;
    return true;
}

namespace {
inline uint64_t column_key(int sx, int sz) {
    return (uint64_t(uint32_t(sx)) << 32) | uint64_t(uint32_t(sz));
}
}  // namespace

void BrickStore::mark_column(int vx, int vy, int vz) {
    // Lattice point v reaches the surface over [v - 1, v + 1] voxels each
    // way (trilinear), which is half-voxel columns v - 1 and v.
    const float y = float(vy) * kEditVoxel;
    for (int dz = -1; dz <= 0; ++dz) {
        for (int dx = -1; dx <= 0; ++dx) {
            std::unordered_map<uint64_t, float>::iterator it = column_top_.find(column_key(vx + dx, vz + dz));
            if (it == column_top_.end()) column_top_[column_key(vx + dx, vz + dz)] = y;
            else if (y > it->second) it->second = y;
        }
    }
}

bool BrickStore::column_top(int sx, int sz, float& out_top) const {
    if (column_top_.empty()) return false;
    std::unordered_map<uint64_t, float>::const_iterator it = column_top_.find(column_key(sx, sz));
    if (it == column_top_.end()) return false;
    out_top = it->second;
    return true;
}

}  // namespace engine
