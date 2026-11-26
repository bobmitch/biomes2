#include "engine/svdag/brick_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
                             const std::function<void(const int*, size_t, float*)>& base,
                             float out_min[3], float out_max[3]) {
    if (radius <= 0.0f) return false;

    // The band past the rim: the rock points beside the new surface carry
    // their distance to it. Without it the mesher interpolated between an
    // air point and the field's rock value -- metres, under ground -- and
    // every vertex landed beside the air point: dug walls in half-metre
    // steps, a jagged waterline wherever one crossed the sea.
    const float reach = radius + kEditBand;
    const int v0x = voxel_of(cx - reach), v1x = voxel_of(cx + reach);
    const int v0y = voxel_of(cy - reach), v1y = voxel_of(cy + reach);
    const int v0z = voxel_of(cz - reach), v1z = voxel_of(cz + reach);

    // Gather first: the field is asked only where it can matter, all at
    // once (`base` may spread a big batch over threads). A point more than
    // kEditBand inside the sphere needs no answer: every lattice neighbour
    // is inside too (the distance is 1-Lipschitz), so no surface crosses an
    // edge it ends, and the sphere's distance alone stands there. Asked
    // everywhere, an 8 m dig took 24 thousand field calls, 30 ms.
    struct Pt { int x, y, z; float d, old; bool owned; };
    std::vector<Pt> pts;
    std::vector<int> need_xyz;
    std::vector<size_t> need_at;
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
                if (d >= reach) continue;
                Pt p = {vx, vy, vz, d, 0.0f, false};
                p.owned = edited_at(vx, vy, vz, p.old);
                if (!p.owned && d - radius > -kEditBand) {
                    need_at.push_back(pts.size());
                    need_xyz.push_back(vx);
                    need_xyz.push_back(vy);
                    need_xyz.push_back(vz);
                }
                pts.push_back(p);
            }
        }
    }
    if (!need_at.empty()) {
        std::vector<float> got(need_at.size());
        base(need_xyz.data(), need_at.size(), got.data());
        for (size_t k = 0; k < need_at.size(); ++k) pts[need_at[k]].old = got[k];
    }

    // Every gathered point by its lattice position, for the roof test below.
    const int nx = v1x - v0x + 1, ny = v1y - v0y + 1, nz = v1z - v0z + 1;
    std::vector<int> at(size_t(nx) * ny * nz, -1);
    for (size_t k = 0; k < pts.size(); ++k)
        at[(size_t(pts[k].z - v0z) * ny + size_t(pts[k].y - v0y)) * nx + size_t(pts[k].x - v0x)] = int(k);
    auto find_pt = [&](int x, int y, int z) -> const Pt* {
        if (x < v0x || x > v1x || y < v0y || y > v1y || z < v0z || z > v1z) return nullptr;
        const int k = at[(size_t(z - v0z) * ny + size_t(y - v0y)) * nx + size_t(x - v0x)];
        return k < 0 ? nullptr : &pts[size_t(k)];
    };
    // A dig that stops just short of the surface leaves a roof thinner than
    // the lattice: rock carved on one side, air on the other, and no point
    // left rock between them -- a hole in the ground the dig never reached.
    // So before a rock point is carved, each neighbour that is air and
    // OUTSIDE the sphere is asked: along the edge between them, does the
    // sphere's boundary come before the surface's? Then rock remains there,
    // and the point stays rock: the roof is drawn a voxel thick where it is
    // thinner, never as a hole. A dig that does break through (the surface
    // first) carves as before.
    auto keeps_roof = [&](const Pt& p, float sd_p) {
        static const int kN[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int n = 0; n < 6; ++n) {
            const Pt* a = find_pt(p.x + kN[n][0], p.y + kN[n][1], p.z + kN[n][2]);
            if (!a) continue;
            const float sd_a = a->d - radius;
            if (sd_a <= 0.0f || a->old >= 0.0f) continue;      // carved too, or rock
            const float t_ground = p.old / (p.old - a->old);    // where the surface crosses, P to A
            const float t_sphere = -sd_p / (sd_a - sd_p);       // where the sphere's boundary does
            if (t_sphere < t_ground) return true;
        }
        return false;
    };

    bool any = false;
    for (const Pt& p : pts) {
        const int vx = p.x, vy = p.y, vz = p.z;
        const float d = p.d;
        // What is there now: an earlier edit's value, or the field (an
        // unowned point deep inside has none, and needs none).
        const bool known = p.owned || d - radius > -kEditBand;
        const float old = p.old;

        // The sphere's signed distance, positive outside: a dig is the rock
        // that was there AND outside the sphere, min(old, d - r); a fill the
        // reverse. Overlapping edits add up by the same rule, and nothing is
        // written where the sphere changes nothing -- air a dig passes
        // through, rock past its band. Never zero inside (neither air nor
        // rock): a point on the rim is kept a hair on the edit's side.
        const float sd = d - radius;
        const float kRim = 1.0e-3f;
        float v;
        if (strength < 0.0f) {
            if (known && old <= sd) continue;
            v = sd;
            if (d < radius) v = std::min(v, -kRim);
            if (known && old > 0.0f && v < 0.0f && keeps_roof(p, sd)) continue;
        } else {
            if (known && old >= -sd) continue;
            v = -sd;
            if (d < radius) v = std::max(v, kRim);
        }

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
        b.density[li] = v;
        b.material[li] = material;
        b.edited[li >> 5] |= (1u << (li & 31));
        // The column map is for ground that MOVED (grass, settling trees):
        // the band's rock stays rock, and marking it bared grass a metre
        // round every hole and over tunnels a metre deeper than before.
        if (v < 0.0f) mark_column(vx, vy, vz);
        any = true;
    }
    if (!any) return false;
    ++generation_;

    // One voxel past the sphere: an edited point on a chunk's border is also
    // its neighbour's ghost corner, and that neighbour must re-mesh too.
    const float pad = reach + kEditVoxel;
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
            if (it->second.density[i] >= 0.0f) continue;   // as edit_sphere marks
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
