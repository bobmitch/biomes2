#include "worldgen/world.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace worldgen {

// The shaping half of an archetype. worldgen/shaders/ does not know the table
// format, so the conversion lives on this side of the line.
static BiomeProfile profile_of(const BiomeArchetype& a) {
    BiomeProfile p;
    p.base_elevation = a.base_elevation;
    p.amplitude      = a.amplitude;
    p.octaves        = static_cast<int>(a.octaves);
    p.lacunarity     = a.lacunarity;
    p.gain           = a.gain;
    p.ridge_blend    = a.ridge_blend;
    p.flatness       = a.flatness;
    return p;
}

// --- layer 2: blue-noise scatter (Bridson) ---------------------------------
std::vector<std::pair<float, float> > scatter_cells(const WorldParams& wp, uint32_t seed) {
    // N ~= 0.683 * A / rmin^2 for Bridson, so rmin^2 ~= 0.683 * target cell area.
    const float rmin = std::sqrt(0.683f * wp.cell_area_target);
    const float R    = wp.radius;
    const float cell = rmin / 1.41421356f;
    const int   gw   = static_cast<int>(std::ceil(2.0f * R / cell)) + 1;

    std::vector<int> grid(static_cast<size_t>(gw) * gw, -1);
    std::vector<std::pair<float, float> > pts;
    std::vector<int> active;
    Rng rng(seed);

    auto gx = [&](float x) { return static_cast<int>((x + R) / cell); };
    auto gz = [&](float z) { return static_cast<int>((z + R) / cell); };

    auto fits = [&](float x, float z) {
        const int cx = gx(x), cz = gz(z);
        const int x0 = std::max(cx - 2, 0), x1 = std::min(cx + 2, gw - 1);
        const int z0 = std::max(cz - 2, 0), z1 = std::min(cz + 2, gw - 1);
        for (int j = z0; j <= z1; ++j) {
            for (int i = x0; i <= x1; ++i) {
                const int p = grid[static_cast<size_t>(j) * gw + i];
                if (p < 0) continue;
                const float dx = pts[p].first - x, dz = pts[p].second - z;
                if (dx * dx + dz * dz < rmin * rmin) return false;
            }
        }
        return true;
    };

    auto push = [&](float x, float z) {
        grid[static_cast<size_t>(gz(z)) * gw + gx(x)] = static_cast<int>(pts.size());
        active.push_back(static_cast<int>(pts.size()));
        pts.push_back(std::make_pair(x, z));
    };

    // Seed point near the centre so the home landmass always has cells.
    push(0.0f, 0.0f);

    while (!active.empty()) {
        const size_t ai = rng.next() % active.size();
        const int    pi = active[ai];
        bool placed = false;
        for (int k = 0; k < 30; ++k) {
            // A direction by rejection in the unit disc, normalised with sqrt,
            // rather than cos and sin of a random angle. Those are libm and
            // not bit-reproducible across platforms, and the cell positions
            // feed every layer below this one -- an ULP here moves a Voronoi
            // boundary, which moves a height, which moves a river.
            //
            // Rejecting straight into the annulus would have been simpler but
            // samples it uniformly by AREA where the original sampled
            // uniformly by RADIUS. The packing changes, and quota went to
            // 11.3% against a 10% tolerance. Same distribution, exact
            // arithmetic.
            float dx = 0.0f, dz = 0.0f, dd = 0.0f;
            bool  ok = false;
            for (int t = 0; t < 24 && !ok; ++t) {
                dx = rng.uniform() * 2.0f - 1.0f;
                dz = rng.uniform() * 2.0f - 1.0f;
                dd = dx * dx + dz * dz;
                ok = (dd > 1.0e-8f) && (dd <= 1.0f);
            }
            if (!ok) continue;
            const float inv = 1.0f / std::sqrt(dd);
            const float rad = rmin * (1.0f + rng.uniform());
            const float x = pts[pi].first  + dx * inv * rad;
            const float z = pts[pi].second + dz * inv * rad;
            if (x * x + z * z > R * R) continue;
            if (!fits(x, z)) continue;
            push(x, z);
            placed = true;
            break;
        }
        if (!placed) { active[ai] = active.back(); active.pop_back(); }
    }
    return pts;
}

// --- layer 4 ----------------------------------------------------------------
// Ground height in metres. Which cells are near a point is an index question,
// so the lookup and the weights live here; every metre of the result comes
// from worldgen/shaders/field.h.
float terrain_base(const SeedWorld& w, const WorldParams& wp,
                            const ArchetypeTable& table, float x, float z, float r) {
    const float cont = layer0_height(x, z, r, wp, w.s_cont);

    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);

    int   idx[4];
    float dist[4];
    const int found = w.lookup.nearest4(w.pts, wx, wz, idx, dist);

    float h[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < found; ++k) {
        const int b = w.biome[idx[k]];
        if (b < 0) continue;
        const BiomeArchetype& a = table[b];
        h[k] = layer4_profile(x, z, wp.biome_freq, profile_of(a), w.s_height);
        // Smoothed distance to the boundary: the gap between this cell and the
        // nearest one, which is twice the distance to the bisector.
        const float t = 1.0f - (dist[k] - dist[0]) / wp.blend_width;
        wt[k] = t > 0.0f ? t * t : 0.0f;
    }

    const float blended = layer4_blend(h[0], wt[0], h[1], wt[1], h[2], wt[2], h[3], wt[3]);
    return layer4_height(cont, wp.continent_scale, blended, wp.sea_level);
}

// Distance to the nearest river segment, with that channel's shape. Shared by
// the carve and the relief renderer, so the water is drawn where it is.
float nearest_river(const SeedWorld& w, const WorldParams& wp,
                           float x, float z, float& out_w, float& out_d) {
    out_w = 0.0f;
    out_d = 0.0f;
    if (w.rivers.empty()) return 3.4e38f;

    const float R = wp.radius;
    const int   gx = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((x + R) / w.rgrid_cell)));
    const int   gz = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((z + R) / w.rgrid_cell)));

    float best_d = 3.4e38f;
    float best_w = 0.0f, best_depth = 0.0f;
    for (int j = gz - 1; j <= gz + 1; ++j) {
        if (j < 0 || j >= w.rgrid_n) continue;
        for (int i = gx - 1; i <= gx + 1; ++i) {
            if (i < 0 || i >= w.rgrid_n) continue;
            const std::vector<int>& bin = w.rgrid[static_cast<size_t>(j) * w.rgrid_n + i];
            for (size_t m = 0; m < bin.size(); ++m) {
                const SeedWorld::RiverSeg& s = w.rivers[bin[m]];
                const float d = river_segment_distance(x, z, s.ax, s.az, s.bx, s.bz);
                if (d < best_d) { best_d = d; best_w = s.width; best_depth = s.depth; }
            }
        }
    }
    out_w = best_w;
    out_d = best_depth;
    return best_d;
}

// Ground height with the drainage carved in. The tracing in build_rivers runs
// against terrain_base, so this is the one place the two must not be confused:
// carving the surface the rivers were traced on would feed back on itself.
float terrain_height(const SeedWorld& w, const WorldParams& wp,
                            const ArchetypeTable& table, float x, float z, float r) {
    const float h = terrain_base(w, wp, table, x, z, r);
    float rw = 0.0f, rd = 0.0f;
    const float dist = nearest_river(w, wp, x, z, rw, rd);
    if (dist >= rw) return h;
    return river_carve(h, dist, rw, rd);
}

// --- layer 5: trace the drainage -------------------------------------------
//
// Steepest descent on a raw heightfield gets stuck in every local minimum, so
// the surface is depression-filled first (priority flood). That buys more than
// convenience: the flood pops cells in order of spill height, so the cell that
// relaxes a neighbour is always downstream of it. Taking that as the flow
// direction makes "descends monotonically and terminates in ocean" true by
// construction rather than something to test for and patch up.
void build_rivers(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table) {
    const int   G = wp.river_grid;
    const int   N = G * G;
    const float R = wp.radius;
    const float step = 2.0f * R / G;

    std::vector<float> h(N, 0.0f);
    std::vector<char>  inside(N, 0);
    for (int j = 0; j < G; ++j) {
        const float z = -R + (j + 0.5f) * step;
        for (int i = 0; i < G; ++i) {
            const float x = -R + (i + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            const int   k = j * G + i;
            if (rr > 1.0f) { h[k] = -1.0e9f; continue; }
            inside[k] = 1;
            h[k] = terrain_base(w, wp, table, x, z, rr);
        }
    }

    std::vector<float> filled(h);
    std::vector<int>   parent(N, -1);
    std::vector<char>  done(N, 0);
    std::vector<int>   order;
    order.reserve(N);

    typedef std::pair<float, int> PQ;
    std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ> > pq;
    for (int k = 0; k < N; ++k) {
        // The sea and everything beyond the rim are the outlets.
        if (!inside[k] || h[k] <= 0.0f) {
            done[k] = 1;
            order.push_back(k);
            pq.push(PQ(h[k], k));
        }
    }

    // Eight-connected, not four. Four-connected flow can only run along the
    // grid axes, so every river comes out as a right-angled staircase -- which
    // is invisible on a contact sheet at 87 m per pixel and unmistakable the
    // moment you look at one seed close up.
    const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int dzs[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!pq.empty()) {
        const PQ cur = pq.top();
        pq.pop();
        const int k = cur.second, ci = k % G, cj = k / G;
        for (int d = 0; d < 8; ++d) {
            const int ni = ci + dxs[d], nj = cj + dzs[d];
            if (ni < 0 || ni >= G || nj < 0 || nj >= G) continue;
            const int nk = nj * G + ni;
            if (done[nk] || !inside[nk]) continue;
            done[nk] = 1;
            filled[nk] = std::max(h[nk], cur.first);
            parent[nk] = k;              // the cell it spills into: downstream
            order.push_back(nk);
            pq.push(PQ(filled[nk], nk));
        }
    }

    // Upstream area. Children are relaxed after their parent, so walking the
    // flood order backwards visits every cell before the one it drains into.
    std::vector<float> acc(N, 1.0f);
    for (size_t idx = order.size(); idx-- > 0;) {
        const int k = order[idx];
        if (parent[k] >= 0) acc[parent[k]] += acc[k];
    }

    w.rivers.clear();
    for (int k = 0; k < N; ++k) {
        if (!inside[k] || parent[k] < 0) continue;
        if (h[k] <= 0.0f) continue;                 // already at sea
        if (acc[k] < wp.river_threshold) continue;
        const int p = parent[k];
        const int g = parent[p];
        // Corner-cut: run the segment between edge midpoints rather than cell
        // centres. One pass of Chaikin on the flow path, which costs nothing
        // and takes the hard right angles off the grid.
        const float kx = -R + ((k % G) + 0.5f) * step, kz = -R + ((k / G) + 0.5f) * step;
        const float px = -R + ((p % G) + 0.5f) * step, pz = -R + ((p / G) + 0.5f) * step;
        SeedWorld::RiverSeg s;
        s.ax = 0.5f * (kx + px);
        s.az = 0.5f * (kz + pz);
        if (g >= 0) {
            const float gx = -R + ((g % G) + 0.5f) * step, gz = -R + ((g / G) + 0.5f) * step;
            s.bx = 0.5f * (px + gx);
            s.bz = 0.5f * (pz + gz);
        } else {
            s.bx = px;
            s.bz = pz;
        }
        const float f = std::sqrt(acc[k] / wp.river_threshold);
        s.width = std::min(38.0f + 26.0f * f, 170.0f);
        s.depth = std::min(3.0f + 2.4f * f, 17.0f);
        w.rivers.push_back(s);
        if (filled[p] > filled[k]) ++w.river_ascending;
    }

    // Every river head followed to its outlet, as the harness asserts.
    for (int k = 0; k < N; ++k) {
        if (!inside[k] || h[k] <= 0.0f || acc[k] < wp.river_threshold) continue;
        bool head = true;
        for (int d = 0; d < 8 && head; ++d) {
            const int ni = (k % G) + dxs[d], nj = (k / G) + dzs[d];
            if (ni < 0 || ni >= G || nj < 0 || nj >= G) continue;
            const int nk = nj * G + ni;
            if (parent[nk] == k && acc[nk] >= wp.river_threshold) head = false;
        }
        if (!head) continue;
        ++w.river_paths_total;
        int cur = k, steps = 0;
        while (cur >= 0 && steps < 4 * N) {
            if (h[cur] <= 0.0f || !inside[cur]) { ++w.river_paths_to_ocean; break; }
            cur = parent[cur];
            ++steps;
        }
    }

    // Bin the segments so the carve only tests the few that could be near.
    w.rgrid_n = std::max(8, G / 4);
    w.rgrid_cell = 2.0f * R / w.rgrid_n;
    w.rgrid.assign(static_cast<size_t>(w.rgrid_n) * w.rgrid_n, std::vector<int>());
    for (size_t s = 0; s < w.rivers.size(); ++s) {
        const SeedWorld::RiverSeg& seg = w.rivers[s];
        const float pad = seg.width;
        const int i0 = std::max(0, static_cast<int>((std::min(seg.ax, seg.bx) - pad + R) / w.rgrid_cell));
        const int i1 = std::min(w.rgrid_n - 1, static_cast<int>((std::max(seg.ax, seg.bx) + pad + R) / w.rgrid_cell));
        const int j0 = std::max(0, static_cast<int>((std::min(seg.az, seg.bz) - pad + R) / w.rgrid_cell));
        const int j1 = std::min(w.rgrid_n - 1, static_cast<int>((std::max(seg.az, seg.bz) + pad + R) / w.rgrid_cell));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                w.rgrid[static_cast<size_t>(j) * w.rgrid_n + i].push_back(static_cast<int>(s));
            }
        }
    }
}

SeedWorld build_world(uint64_t world_seed,
                             const WorldParams& wp,
                             const ArchetypeTable& table,
                             const AssignParams& ap) {
    SeedWorld w;

    w.s_cont = feature_seed(world_seed, FEATURE_CONTINENT);
    w.s_dang = feature_seed(world_seed, FEATURE_DANGER);
    w.s_wx   = feature_seed(world_seed, FEATURE_WARP_X);
    w.s_wz   = feature_seed(world_seed, FEATURE_WARP_Z);
    w.s_height = feature_seed(world_seed, FEATURE_HEIGHT);
    const uint32_t s_cont = w.s_cont, s_dang = w.s_dang, s_wx = w.s_wx, s_wz = w.s_wz;
    const uint32_t s_cell = feature_seed(world_seed, FEATURE_CELLS);

    w.pts = scatter_cells(wp, s_cell);
    const std::vector<std::pair<float, float> >& pts = w.pts;
    const int n = static_cast<int>(pts.size());
    if (n == 0) return w;

    const float R = wp.radius;
    w.lookup.build(pts, R);
    const CellLookup& lookup = w.lookup;
    auto nearest = [&](float x, float z) { return lookup.nearest(pts, x, z); };

    // Rasterise the disc: real cell areas, real land mask, real adjacency.
    const int   G = wp.grid;
    const float step = 2.0f * R / G;
    const float sample_area = step * step;

    std::vector<double> acc_x(n, 0.0), acc_z(n, 0.0);
    std::vector<double> acc_total(n, 0.0), acc_land(n, 0.0);
    std::vector<char> adj(static_cast<size_t>(n) * n, 0);
    std::vector<int> row_prev(G, -1), row_cur(G, -1);

    double disc_area = 0.0, land_area = 0.0;

    for (int gj = 0; gj < G; ++gj) {
        const float z = -R + (gj + 0.5f) * step;
        for (int gi = 0; gi < G; ++gi) {
            const float x = -R + (gi + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            if (rr > 1.0f) { row_cur[gi] = -1; continue; }

            const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, s_wx);
            const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, s_wz);
            const int ci = nearest(wx, wz);
            row_cur[gi] = ci;
            if (ci < 0) continue;

            disc_area += sample_area;
            acc_total[ci] += sample_area;
            acc_x[ci] += x;
            acc_z[ci] += z;

            const float h = layer0_height(x, z, rr, wp, s_cont);
            if (rr > 0.95f && h > w.max_h_outer) w.max_h_outer = h;
            if (h > wp.sea_level) {
                acc_land[ci] += sample_area;
                land_area += sample_area;
            }

            if (gi > 0 && row_cur[gi - 1] >= 0 && row_cur[gi - 1] != ci) {
                adj[static_cast<size_t>(ci) * n + row_cur[gi - 1]] = 1;
                adj[static_cast<size_t>(row_cur[gi - 1]) * n + ci] = 1;
            }
            if (row_prev[gi] >= 0 && row_prev[gi] != ci) {
                adj[static_cast<size_t>(ci) * n + row_prev[gi]] = 1;
                adj[static_cast<size_t>(row_prev[gi]) * n + ci] = 1;
            }
        }
        row_prev.swap(row_cur);
    }

    w.land_fraction = disc_area > 0.0 ? static_cast<float>(land_area / disc_area) : 0.0f;

    // Build cell records from the rasterised truth.
    w.cells.resize(n);
    std::vector<CellRecord>& cells = w.cells;
    for (int i = 0; i < n; ++i) {
        CellRecord& c = cells[i];
        c.total_area = static_cast<float>(acc_total[i]);
        c.land_area  = static_cast<float>(acc_land[i]);
        if (acc_total[i] > 0.0) {
            c.cx = static_cast<float>(acc_x[i] / (acc_total[i] / sample_area));
            c.cz = static_cast<float>(acc_z[i] / (acc_total[i] / sample_area));
        } else {
            c.cx = pts[i].first; c.cz = pts[i].second;
        }
        const float rr = std::sqrt(c.cx * c.cx + c.cz * c.cz) / R;
        c.norm_area = rr * rr;
        c.danger    = layer1_danger(c.cx, c.cz, rr, wp, s_dang);
        c.is_land   = c.total_area > 0.0f && c.land_area > 0.5f * c.total_area;
    }

    CellGraph& graph = w.graph;
    graph.offset.resize(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        graph.offset[i] = static_cast<int>(graph.list.size());
        for (int j = 0; j < n; ++j) if (adj[static_cast<size_t>(i) * n + j]) graph.list.push_back(j);
    }
    graph.offset[n] = static_cast<int>(graph.list.size());

    // --- the thing under test ---
    assign_biomes(cells, graph, table, ap, w.biome, w.stats);

    // Layer 5 runs last: it needs the finished heightfield, which needs the
    // biome assignment.
    build_rivers(w, wp, table);
    return w;
}

}  // namespace worldgen
