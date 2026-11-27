#include "engine/nav/nav_tile.h"

#include "worldgen/world.h"

#include "DetourAlloc.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace engine {
namespace nav {

namespace {

const float kCs = kEditVoxel;   // 0.5 m: the edit lattice
const float kCh = 0.25f;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }

// The tile's copy of the edits, looked up as BrickStore::edited_at does.
struct Edits {
    std::unordered_map<BrickKey, const Brick*, BrickKeyHash> map;
    bool at(int vx, int vy, int vz, float& d) const {
        if (map.empty()) return false;
        BrickKey k;
        k.x = floor_div(vx, kBrickSide);
        k.y = floor_div(vy, kBrickSide);
        k.z = floor_div(vz, kBrickSide);
        const auto it = map.find(k);
        if (it == map.end()) return false;
        const int li = (floor_mod(vz, kBrickSide) * kBrickSide + floor_mod(vy, kBrickSide)) * kBrickSide +
                       floor_mod(vx, kBrickSide);
        if ((it->second->edited[li >> 5] & (1u << (li & 31))) == 0) return false;
        d = it->second->density[li];
        return true;
    }
};

// Frees Recast's intermediates however the build ends.
struct Scratch {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    ~Scratch() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    }
};

}  // namespace

bool build_tile(const worldgen::SeedWorld& w, const worldgen::WorldParams& wp, const worldgen::ArchetypeTable& t,
                const TileInput& in, TileOutput& out, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    out = TileOutput();
    const Agent& ag = in.agent;
    const int walk_h = int(std::ceil(ag.height / kCh));
    const int climb = int(std::floor(ag.climb / kCh));
    const int rad = int(std::ceil(ag.radius / kCs));
    const int cells = int(std::lround(in.size / kCs));
    const int border = rad + 3;
    const int W = cells + 2 * border;

    // Cells CENTRED on the edit lattice: Recast's cell x covers
    // [bmin + x cs, bmin + (x + 1) cs], and every sample and stamp below is
    // taken at bmin + (x + 0.5) cs, a lattice point. Sampled at the corner,
    // the whole tile sat a quarter cell off the world.
    float bmin[3], bmax[3];
    bmin[0] = float(in.tx) * in.size - float(border) * kCs - 0.5f * kCs;
    bmin[2] = float(in.tz) * in.size - float(border) * kCs - 0.5f * kCs;
    bmax[0] = bmin[0] + float(W) * kCs;
    bmax[2] = bmin[2] + float(W) * kCs;

    Edits edits;
    for (const auto& b : in.bricks) edits.map[b.first] = b.second.get();

    // --- each column's band -------------------------------------------------
    std::vector<float> lo(size_t(W) * W), hi(size_t(W) * W);
    const float brick_m = float(kBrickSide) * kEditVoxel;
    for (int bz = 0; bz < W; bz += 4)
        for (int bx = 0; bx < W; bx += 4) {
            // Layer 6's overhang reach (real_ground's), per 2 m block.
            const float cx = bmin[0] + (float(bx) + 2.5f) * kCs, cz = bmin[2] + (float(bz) + 2.5f) * kCs;
            int bio[4];
            float bw[4];
            const int nb = worldgen::biome_mix(w, wp, cx, cz, bio, bw);
            float ov = 0.0f;
            for (int k = 0; k < nb; ++k) ov += bw[k] * t[size_t(bio[k])].overhang_strength;
            ov = ov * wp.overhang_scale + 1.0f;
            for (int z = bz; z < (std::min)(bz + 4, W); ++z)
                for (int x = bx; x < (std::min)(bx + 4, W); ++x) {
                    const float px = bmin[0] + (float(x) + 0.5f) * kCs, pz = bmin[2] + (float(z) + 0.5f) * kCs;
                    const float r = std::sqrt(px * px + pz * pz) / wp.radius;
                    const float h = worldgen::terrain_height(w, wp, t, px, pz, r);
                    lo[size_t(z) * W + x] = h - ov - 1.5f;
                    hi[size_t(z) * W + x] = h + ov + 1.0f;
                }
        }
    // Down to any placed cave under the column, per 8 m block.
    for (int bz = 0; bz < W; bz += 16)
        for (int bx = 0; bx < W; bx += 16) {
            const float x0 = bmin[0] + float(bx) * kCs, z0 = bmin[2] + float(bz) * kCs;
            const float cb = worldgen::cave_bottom_in_box(w, x0, z0, x0 + 16.0f * kCs, z0 + 16.0f * kCs, 1.0f);
            if (cb > 1e30f) continue;
            for (int z = bz; z < (std::min)(bz + 16, W); ++z)
                for (int x = bx; x < (std::min)(bx + 16, W); ++x)
                    lo[size_t(z) * W + x] = (std::min)(lo[size_t(z) * W + x], cb);
        }
    // Over every edited brick.
    for (const auto& b : in.bricks) {
        const float x0 = float(b.first.x) * brick_m, z0 = float(b.first.z) * brick_m, y0 = float(b.first.y) * brick_m;
        const int cx0 = (std::max)(0, int(std::floor((x0 - bmin[0]) / kCs)));
        const int cz0 = (std::max)(0, int(std::floor((z0 - bmin[2]) / kCs)));
        const int cx1 = (std::min)(W, int(std::ceil((x0 + brick_m - bmin[0]) / kCs)) + 1);
        const int cz1 = (std::min)(W, int(std::ceil((z0 + brick_m - bmin[2]) / kCs)) + 1);
        for (int z = cz0; z < cz1; ++z)
            for (int x = cx0; x < cx1; ++x) {
                lo[size_t(z) * W + x] = (std::min)(lo[size_t(z) * W + x], y0 - 1.0f);
                hi[size_t(z) * W + x] = (std::max)(hi[size_t(z) * W + x], y0 + brick_m + 1.0f);
            }
    }
    float ylo = 1e30f, yhi = -1e30f;
    for (size_t i = 0; i < lo.size(); ++i) { ylo = (std::min)(ylo, lo[i]); yhi = (std::max)(yhi, hi[i]); }
    for (const Obstacle& o : in.obstacles) yhi = (std::max)(yhi, (std::max)(o.a[1], o.b[1]) + o.r);
    yhi = (std::max)(yhi, in.sea_level + 1.0f);
    bmin[1] = std::floor(ylo) - 2.0f;
    bmax[1] = std::ceil(yhi) + ag.height + 2.0f;
    const float max_h = float(RC_SPAN_MAX_HEIGHT - 4) * kCh;
    if (bmax[1] - bmin[1] > max_h) bmax[1] = bmin[1] + max_h;

    rcContext ctx(false);
    Scratch s;
    s.hf = rcAllocHeightfield();
    if (!s.hf || !rcCreateHeightfield(&ctx, *s.hf, W, W, bmin, bmax, kCs, kCh)) {
        error = "rcCreateHeightfield";
        return false;
    }
    auto unit = [&](float y) {
        int u = int(std::floor((y - bmin[1]) / kCh + 0.5f));
        return (std::max)(0, (std::min)(u, RC_SPAN_MAX_HEIGHT - 1));
    };
    auto add = [&](int x, int z, float y0, float y1, unsigned char area) {
        int a = unit(y0), b = unit(y1);
        if (b <= a) b = a + 1;
        rcAddSpan(&ctx, *s.hf, x, z, (unsigned short)a, (unsigned short)b, area, climb);
    };
    auto floor_area = [&](float top) -> unsigned char {
        if (top >= in.sea_level) return kAreaGround;
        if (top >= in.sea_level - in.wade_depth) return kAreaWade;
        return RC_NULL_AREA;
    };

    // --- the field's spans, column by column ---------------------------------
    for (int z = 0; z < W; ++z)
        for (int x = 0; x < W; ++x) {
            const float px = bmin[0] + (float(x) + 0.5f) * kCs, pz = bmin[2] + (float(z) + 0.5f) * kCs;
            const int vx = int(std::lround(px / kEditVoxel)), vz = int(std::lround(pz / kEditVoxel));
            const int v_hi = int(std::ceil(hi[size_t(z) * W + x] / kEditVoxel));
            const int v_lo = int(std::floor(lo[size_t(z) * W + x] / kEditVoxel));
            auto dens = [&](int vy) {
                float d;
                if (edits.at(vx, vy, vz, d)) return d;
                ++out.samples;
                return worldgen::terrain_density(w, wp, t, px, float(vy) * kEditVoxel, pz, kEditVoxel);
            };
            ++out.columns;
            float top_y = 0.0f, highest = -1e30f;
            float y_prev = float(v_hi) * kEditVoxel;
            float d_prev = dens(v_hi);
            bool rock = d_prev > 0.0f;
            if (rock) top_y = y_prev;
            for (int vy = v_hi - 1; vy >= v_lo; --vy) {
                const float y = float(vy) * kEditVoxel;
                const float d = dens(vy);
                if (!rock && d > 0.0f) {
                    // Air above, rock here: a floor between.
                    top_y = y + (y_prev - y) * d / (d - d_prev);
                    rock = true;
                } else if (rock && d <= 0.0f) {
                    // Rock above, air here: a ceiling's underside.
                    const float bot = y + (y_prev - y) * d / (d - d_prev);
                    add(x, z, bot, top_y, floor_area(top_y));
                    highest = (std::max)(highest, top_y);
                    rock = false;
                }
                y_prev = y;
                d_prev = d;
            }
            if (rock) {
                add(x, z, bmin[1], top_y, floor_area(top_y));
                highest = (std::max)(highest, top_y);
            }
            // Deep water over the column's top floor: the surface to swim.
            if (highest > -1e29f && highest < in.sea_level - in.wade_depth)
                add(x, z, in.sea_level - 0.7f, in.sea_level - 0.5f, kAreaSwim);
        }

    // --- obstacles: solid, never walkable ------------------------------------
    // Inflated by a whole cell. The cells (0.5 m) are coarser than the
    // agent (0.4 m), so eroding by one cell clears a blocked cell's four
    // sides and not its corners: a thin trunk's one blocked cell left the
    // mesh touching it diagonally, 0.1-0.2 m from the axis, and a quarter
    // cell still left 0.09 m past the bark (--nav-report, C).
    const float kInflate = kCs;
    for (const Obstacle& o : in.obstacles) {
        const float r = o.r + kInflate;
        const float xa = (std::min)(o.a[0], o.b[0]) - r, xb = (std::max)(o.a[0], o.b[0]) + r;
        const float za = (std::min)(o.a[2], o.b[2]) - r, zb = (std::max)(o.a[2], o.b[2]) + r;
        const int x0 = (std::max)(0, int(std::floor((xa - bmin[0]) / kCs)));
        const int x1 = (std::min)(W - 1, int(std::ceil((xb - bmin[0]) / kCs)));
        const int z0 = (std::max)(0, int(std::floor((za - bmin[2]) / kCs)));
        const int z1 = (std::min)(W - 1, int(std::ceil((zb - bmin[2]) / kCs)));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const float px = bmin[0] + (float(x) + 0.5f) * kCs, pz = bmin[2] + (float(z) + 0.5f) * kCs;
                if (!o.capsule) {
                    const float dx = px - o.a[0], dz = pz - o.a[2];
                    // The column nearest the axis always: a trunk thinner
                    // than a cell still blocks.
                    const bool axis = std::fabs(dx) <= 0.5f * kCs && std::fabs(dz) <= 0.5f * kCs;
                    if (dx * dx + dz * dz > r * r && !axis) continue;
                    add(x, z, o.a[1], o.b[1], RC_NULL_AREA);
                } else {
                    // The nearest point of the axis in plan, and the capsule's
                    // vertical extent over this column there.
                    const float sx = o.b[0] - o.a[0], sz = o.b[2] - o.a[2];
                    const float l2 = sx * sx + sz * sz;
                    float u = l2 > 1e-6f ? ((px - o.a[0]) * sx + (pz - o.a[2]) * sz) / l2 : 0.0f;
                    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
                    const float qx = o.a[0] + sx * u, qz = o.a[2] + sz * u;
                    const float dh2 = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
                    if (dh2 > r * r) continue;
                    const float yc = o.a[1] + (o.b[1] - o.a[1]) * u;
                    const float half = std::sqrt(r * r - dh2);
                    add(x, z, yc - half, yc + half, RC_NULL_AREA);
                }
            }
    }

    // --- Recast's pipeline ----------------------------------------------------
    rcFilterLowHangingWalkableObstacles(&ctx, climb, *s.hf);
    rcFilterLedgeSpans(&ctx, walk_h, climb, *s.hf);
    rcFilterWalkableLowHeightSpans(&ctx, walk_h, *s.hf);
    s.chf = rcAllocCompactHeightfield();
    if (!s.chf || !rcBuildCompactHeightfield(&ctx, walk_h, climb, *s.hf, *s.chf)) { error = "compact"; return false; }
    if (!rcErodeWalkableArea(&ctx, rad, *s.chf)) { error = "erode"; return false; }
    if (!rcBuildDistanceField(&ctx, *s.chf)) { error = "distance field"; return false; }
    if (!rcBuildRegions(&ctx, *s.chf, border, 16, 400)) { error = "regions"; return false; }
    s.cset = rcAllocContourSet();
    // Contours simplified to within 0.35 m: Recast's demo allows 1.3 m, and
    // a metre of slack cut polygon edges through trunks.
    if (!s.cset || !rcBuildContours(&ctx, *s.chf, 0.35f, int(12.0f / kCs), *s.cset)) { error = "contours"; return false; }
    s.pmesh = rcAllocPolyMesh();
    if (!s.pmesh || !rcBuildPolyMesh(&ctx, *s.cset, 6, *s.pmesh)) { error = "poly mesh"; return false; }
    s.dmesh = rcAllocPolyMeshDetail();
    if (!s.dmesh || !rcBuildPolyMeshDetail(&ctx, *s.pmesh, *s.chf, kCs * 6.0f, kCh, *s.dmesh)) {
        error = "detail mesh";
        return false;
    }
    out.polys = s.pmesh->npolys;
    if (s.pmesh->npolys > 0) {
        for (int i = 0; i < s.pmesh->npolys; ++i) {
            const unsigned char a = s.pmesh->areas[i];
            s.pmesh->flags[i] = a == kAreaSwim ? kFlagSwim : (a == kAreaWade ? kFlagWade : kFlagWalk);
        }
        dtNavMeshCreateParams p = {};
        p.verts = s.pmesh->verts;
        p.vertCount = s.pmesh->nverts;
        p.polys = s.pmesh->polys;
        p.polyAreas = s.pmesh->areas;
        p.polyFlags = s.pmesh->flags;
        p.polyCount = s.pmesh->npolys;
        p.nvp = s.pmesh->nvp;
        p.detailMeshes = s.dmesh->meshes;
        p.detailVerts = s.dmesh->verts;
        p.detailVertsCount = s.dmesh->nverts;
        p.detailTris = s.dmesh->tris;
        p.detailTriCount = s.dmesh->ntris;
        p.walkableHeight = ag.height;
        p.walkableRadius = ag.radius;
        p.walkableClimb = ag.climb;
        p.tileX = in.tile_x;
        p.tileY = in.tile_y;
        p.tileLayer = 0;
        rcVcopy(p.bmin, s.pmesh->bmin);
        rcVcopy(p.bmax, s.pmesh->bmax);
        p.cs = kCs;
        p.ch = kCh;
        p.buildBvTree = true;
        if (!dtCreateNavMeshData(&p, &out.data, &out.size)) { error = "dtCreateNavMeshData"; return false; }
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace nav
}  // namespace engine
