#include "worldgen/world.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
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
    p.detail_amp     = a.detail_amp;
    p.overhang_strength = a.overhang_strength;
    p.cave_density      = a.cave_density;
    p.cave_depth_min    = a.cave_depth_min;
    p.cave_depth_max    = a.cave_depth_max;
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
                   const ArchetypeTable& table, float x, float z, float r, float voxel) {
    const Layer0Noise l0n = layer0_noise(x, z, w.l0, voxel);
    const float cont = layer0_shape(l0n.n + w.l0.land_offset, r, w.l0.centre_floor, w.l0.edge_drop);

    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);

    int   idx[4];
    float dist[4];
    float d5 = 3.4e38f;
    const int found = w.lookup.nearest4(w.pts, wx, wz, idx, dist, &d5);

    // The micro-relief once, for every biome: each scales it by its own
    // detail_amp, so blending the heights blends the amplitudes exactly.
    const float dn = layer4_detail(x, z, table.detail.period, table.detail.octaves,
                                   table.detail.gain, w.s_detail, voxel);

    float h[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wall = 0.0f;
    for (int k = 0; k < found; ++k) {
        // Smoothed distance to the boundary: the gap between this cell and the
        // nearest one, which is twice the distance to the bisector -- less
        // what the fifth cell would carry, or the blend steps where the
        // fourth and fifth trade places (blend_weight in field.h). Ocean
        // cells count toward the total, which blend_divisor needs.
        const float wk = blend_weight(dist[k], dist[0], d5, wp.blend_width);
        wall = wall + wk;
        const int b = w.biome[idx[k]];
        if (b < 0) continue;
        const BiomeArchetype& a = table[b];
        h[k] = layer4_profile(x, z, wp.biome_freq, profile_of(a), w.s_height, voxel)
             + a.detail_amp * dn;
        wt[k] = wk;
    }

    const float blended = layer4_blend(h[0], wt[0], h[1], wt[1], h[2], wt[2], h[3], wt[3], wall);
    return layer4_height_shelf(cont, wp.continent_scale, blended, wp.sea_level,
                               w.l0.shelf_depth, layer0_shelf_width(w.l0, l0n.regime));
}

float still_water_depth(const SeedWorld& w, const WorldParams& wp, float x, float z, float r) {
    const Layer0Noise l0n = layer0_noise(x, z, w.l0, 0.0f);
    const float cont = layer0_shape(l0n.n + w.l0.land_offset, r, w.l0.centre_floor, w.l0.edge_drop);
    if (cont > wp.sea_level) return 0.0f;
    const float bed = layer4_height_shelf(cont, wp.continent_scale, 0.0f, wp.sea_level,
                                          w.l0.shelf_depth, layer0_shelf_width(w.l0, l0n.regime));
    return wp.sea_level * wp.continent_scale - bed;
}

int apply_param_spec(WorldParams& p, const char* spec) {
    int applied = 0;
    const std::string all = spec ? spec : "";
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find(',', at);
        if (end == std::string::npos) end = all.size();
        const std::string kv = all.substr(at, end - at);
        at = end + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq);
        while (!k.empty() && k[0] == ' ') k.erase(0, 1);
        const float v = static_cast<float>(std::atof(kv.c_str() + eq + 1));
        const float size = v > 1.0f ? v : 1.0f;
        if (k == "centre_floor") p.centre_floor = v;
        else if (k == "land_offset") p.land_offset = v;
        else if (k == "fine_feat") p.fine_freq = 1.0f / size;
        else if (k == "fine_weight") p.fine_weight = v;
        else if (k == "fine_regime") p.fine_regime = v;
        else if (k == "regime_feat") p.regime_freq = 1.0f / size;
        else if (k == "regime_mean") p.regime_mean = v;
        else if (k == "regime_amp") p.regime_amp = v;
        else if (k == "regime_draw") p.regime_draw = v != 0.0f;
        else if (k == "regime_size_lo") p.regime_size_lo = v;
        else if (k == "regime_size_hi") p.regime_size_hi = v;
        else if (k == "regime_mean_lo") p.regime_mean_lo = v;
        else if (k == "regime_mean_hi") p.regime_mean_hi = v;
        else if (k == "regime_amp_lo") p.regime_amp_lo = v;
        else if (k == "regime_amp_hi") p.regime_amp_hi = v;
        else if (k == "shelf_depth") p.shelf_depth = v;
        else if (k == "shelf_width") p.shelf_width = v;
        else if (k == "shelf_regime") p.shelf_regime = v;
        else if (k == "knee") p.knee = v != 0.0f;
        else if (k == "knee_k") p.knee_k = v;
        else if (k == "knee_c") p.knee_c = v;
        else if (k == "danger_w") p.danger_w = v;
        else if (k == "danger_feat") p.danger_freq = 1.0f / size;
        else continue;
        ++applied;
    }
    return applied;
}

// --- layer 0 prototype: per-seed parameters and the land knee ----------------
static float unit_draw(uint64_t world_seed, uint32_t feature) {
    return static_cast<float>(feature_seed(world_seed, feature) >> 8) * (1.0f / 16777216.0f);
}

Layer0Params make_layer0(const WorldParams& wp, uint64_t world_seed) {
    Layer0Params p;
    p.freq = wp.continent_freq;
    p.octaves = wp.continent_octaves;
    p.centre_floor = wp.centre_floor;
    p.edge_drop = wp.edge_drop;
    p.land_offset = wp.land_offset;
    p.fine_freq = wp.fine_freq;
    p.fine_octaves = wp.fine_octaves;
    p.fine_weight = wp.fine_weight;
    p.fine_regime = wp.fine_regime;
    p.regime_freq = wp.regime_freq;
    p.regime_octaves = wp.regime_octaves;
    p.regime_mean = wp.regime_mean;
    p.regime_amp = wp.regime_amp;
    if (wp.regime_draw) {
        const float us = unit_draw(world_seed, FEATURE_REGIME_SCALE);
        const float um = unit_draw(world_seed, FEATURE_REGIME_MEAN);
        const float ua = unit_draw(world_seed, FEATURE_REGIME_AMP);
        const float size = wp.regime_size_lo + (wp.regime_size_hi - wp.regime_size_lo) * us;
        p.regime_freq = 1.0f / (size > 1.0f ? size : 1.0f);
        p.regime_mean = wp.regime_mean_lo + (wp.regime_mean_hi - wp.regime_mean_lo) * um;
        p.regime_amp = wp.regime_amp_lo + (wp.regime_amp_hi - wp.regime_amp_lo) * ua;
    }
    p.shelf_depth = wp.shelf_depth;
    p.shelf_width = wp.shelf_width;
    p.shelf_regime = wp.shelf_regime;
    // The regime is evaluated only if something reads it; off, the mask is
    // today's bit for bit.
    p.regime_on = (p.regime_mean != 0.0f || p.regime_amp != 0.0f || p.fine_regime != 0.0f ||
                   (p.shelf_width > 0.0f && p.shelf_regime != 0.0f)) ? 1 : 0;
    p.seed = feature_seed(world_seed, FEATURE_CONTINENT);
    p.seed_fine = feature_seed(world_seed, FEATURE_CONTINENT_FINE);
    p.seed_regime = feature_seed(world_seed, FEATURE_REGIME);
    return p;
}

void apply_land_knee(Layer0Params& l0, const WorldParams& wp,
                     float& raw_land, float& knee_land, float& ms) {
    const std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    // The noise once per sample; the offset only enters layer0_shape, so each
    // pass of the bisection is the radial shape and a comparison.
    const int   G = wp.knee_grid > 8 ? wp.knee_grid : 8;
    const float R = wp.radius;
    const float step = 2.0f * R / G;
    std::vector<float> nn, rr;
    nn.reserve(static_cast<size_t>(G) * G);
    rr.reserve(static_cast<size_t>(G) * G);
    for (int j = 0; j < G; ++j) {
        const float z = -R + (j + 0.5f) * step;
        for (int i = 0; i < G; ++i) {
            const float x = -R + (i + 0.5f) * step;
            const float r = std::sqrt(x * x + z * z) / R;
            if (r > 1.0f) continue;
            nn.push_back(layer0_noise(x, z, l0, 0.0f).n);
            rr.push_back(r);
        }
    }
    const long long disc = static_cast<long long>(nn.size());
    auto land_at = [&](float off) {
        long long land = 0;
        for (size_t k = 0; k < nn.size(); ++k) {
            if (layer0_shape(nn[k] + off, rr[k], l0.centre_floor, l0.edge_drop) > wp.sea_level) ++land;
        }
        return land;
    };
    const long long raw = land_at(l0.land_offset);
    raw_land = disc > 0 ? static_cast<float>(static_cast<double>(raw) / disc) : 0.0f;
    knee_land = raw_land;
    const float k = wp.knee_k, c = wp.knee_c;
    if (disc > 0 && raw_land > k && c > k) {
        const float e = raw_land - k;
        const float target = k + (c - k) * e / (e + (c - k));
        const long long want = static_cast<long long>(target * static_cast<float>(disc) + 0.5f);
        // Land is monotone in the offset (smax is), so bisect: hi keeps
        // land >= want, lo has less. 16 passes is 1e-5 of mask.
        float hi = l0.land_offset, lo = l0.land_offset - 0.6f;
        for (int it = 0; it < 16; ++it) {
            const float mid = 0.5f * (lo + hi);
            if (land_at(mid) >= want) hi = mid; else lo = mid;
        }
        l0.land_offset = hi;
        knee_land = static_cast<float>(static_cast<double>(land_at(hi)) / disc);
    }
    const std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    ms = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
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
                     const ArchetypeTable& table, float x, float z, float r, float voxel) {
    float h = terrain_base(w, wp, table, x, z, r, voxel);
    float rw = 0.0f, rd = 0.0f;
    const float dist = nearest_river(w, wp, x, z, rw, rd);
    if (dist < rw) h = river_carve(h, dist, rw, rd);
    // Layer 5b after the carve. A site keeps clear of every channel, so the
    // order only matters to say which wins: the site.
    const int fs = flat_site_at(w, x, z, 0.0f);
    if (fs >= 0) {
        const SeedWorld::FlatSite& f = w.flats[static_cast<size_t>(fs)];
        h = flat_apply(h, x, z, f.x, f.z, f.radius, f.shoulder, f.level);
    }
    return h;
}

int flat_site_at(const SeedWorld& w, float x, float z, float margin) {
    if (w.flats.empty() || w.fgrid.empty()) return -1;
    // The disc's radius from the river grid's own geometry, as build_flats
    // binned with: the same arithmetic both sides, so a point finds its bin.
    const float R = 0.5f * w.rgrid_cell * static_cast<float>(w.rgrid_n);
    const int   gx = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((x + R) / w.rgrid_cell)));
    const int   gz = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((z + R) / w.rgrid_cell)));
    const std::vector<int>& bin = w.fgrid[static_cast<size_t>(gz) * w.rgrid_n + gx];
    for (size_t m = 0; m < bin.size(); ++m) {
        const SeedWorld::FlatSite& f = w.flats[static_cast<size_t>(bin[m])];
        const float dx = x - f.x, dz = z - f.z;
        const float o = f.radius + f.shoulder + margin;
        if (dx * dx + dz * dz < o * o) return bin[m];
    }
    return -1;
}

// Layer 6. The per-biome cave and overhang parameters are blended across the
// nearest cells, the same weights layer 4 uses for heights.
//
// Blending these is safe where blending octave counts would not be: they are
// strength multipliers on a shared noise field, not parameters that change
// what the noise looks like. A biome boundary with a step in cave density
// would read as tunnels stopping dead at an invisible line.
float terrain_density(const SeedWorld& w, const WorldParams& wp,
                      const ArchetypeTable& table, float x, float y, float z, float voxel) {
    const float r = std::sqrt(x * x + z * z) / wp.radius;
    const float h = terrain_height(w, wp, table, x, z, r, voxel);

    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
    int   idx[4];
    float dist[4];
    float d5 = 3.4e38f;
    const int found = w.lookup.nearest4(w.pts, wx, wz, idx, dist, &d5);

    BiomeProfile p;
    p.base_elevation = 0.0f;
    p.amplitude = 0.0f;
    p.octaves = 0;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.ridge_blend = 0.0f;
    p.flatness = 1.0f;
    p.detail_amp = 0.0f;
    p.overhang_strength = 0.0f;
    p.cave_density = 0.0f;
    p.cave_depth_min = 0.0f;
    p.cave_depth_max = 0.0f;

    float wsum = 0.0f, wall = 0.0f;
    for (int k = 0; k < found; ++k) {
        const float wt = blend_weight(dist[k], dist[0], d5, wp.blend_width);
        wall = wall + wt;
        const int b = w.biome[idx[k]];
        if (b < 0) continue;
        if (wt <= 0.0f) continue;
        const BiomeArchetype& a = table[b];
        p.overhang_strength = p.overhang_strength + wt * a.overhang_strength;
        p.cave_density      = p.cave_density      + wt * a.cave_density;
        p.cave_depth_min    = p.cave_depth_min    + wt * a.cave_depth_min;
        p.cave_depth_max    = p.cave_depth_max    + wt * a.cave_depth_max;
        wsum = wsum + wt;
    }
    const float div = blend_divisor(wsum, wall);
    if (div > 0.0f) {
        const float inv = 1.0f / div;
        p.overhang_strength = p.overhang_strength * inv;
        p.cave_density      = p.cave_density * inv;
        p.cave_depth_min    = p.cave_depth_min * inv;
        p.cave_depth_max    = p.cave_depth_max * inv;
    }

    float d = layer6_density(x, y, z, h, p, wp.overhang_freq, wp.overhang_scale,
                             wp.cave_freq, wp.cave_carve, w.s_overhang, w.s_cave, voxel);

    // Layer 7 last, and outside cave_depth_mask entirely. Every other carve in
    // this function is forbidden to reach the surface; this one exists to.
    if (!w.entrances.empty()) {
        const float sd = nearest_entrance(w, wp, x, y, z);
        if (sd < 0.0f) {
            d = entrance_carve(d, sd, wp.entrance_falloff, wp.cave_carve);
        }
    }
    return d;
}

int biome_mix(const SeedWorld& w, const WorldParams& wp, float x, float z,
              int* biome, float* weight) {
    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
    int   idx[4];
    float dist[4];
    float d5 = 3.4e38f;
    const int found = w.lookup.nearest4(w.pts, wx, wz, idx, dist, &d5);
    float wsum = 0.0f, wall = 0.0f;
    float wt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int   bb[4] = {-1, -1, -1, -1};
    for (int k = 0; k < found; ++k) {
        const float wk = blend_weight(dist[k], dist[0], d5, wp.blend_width);
        wall = wall + wk;
        const int b = w.biome[idx[k]];
        if (b < 0 || wk <= 0.0f) continue;
        bb[k] = b;
        wt[k] = wk;
        wsum = wsum + wk;
    }
    const float div = blend_divisor(wsum, wall);
    int n = 0;
    for (int k = 0; k < found; ++k) {
        if (bb[k] < 0) continue;
        biome[n] = bb[k];
        weight[n] = div > 0.0f ? wt[k] / div : 0.0f;
        ++n;
    }
    return n;
}

// Where the REAL surface is at (x, z), near the 2.5D height h. Layer 6's
// overhang term moves the ground off h by up to strength * overhang_scale, a
// few metres in a forest. Testing for air and rock 0.75 m either side of h
// rejected 80% of forest candidates that had passed everything else
// (validate --tree-reasons). So bracket the crossing within that
// displacement, walking down from above it in 1 m steps, and bisect.
// False when there is rock at the top of the bracket or none inside it.
// Layers 8 and 9 both stand on it; the arithmetic is layer 8's, moved.
static bool real_ground(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                        float x, float z, float h, int nb, const int* bio, const float* bw, float& ground) {
    float ov = 0.0f;
    for (int k = 0; k < nb; ++k) ov = ov + bw[k] * table[bio[k]].overhang_strength;
    ov = ov * wp.overhang_scale + 1.0f;
    float y_air = h + ov;
    if (terrain_density(w, wp, table, x, y_air, z) > 0.0f) return false;
    float y_rock = y_air;
    bool found = false;
    while (y_rock > h - ov) {
        y_rock = y_rock - 1.0f;
        if (terrain_density(w, wp, table, x, y_rock, z) > 0.0f) { found = true; break; }
        y_air = y_rock;
    }
    if (!found) return false;
    for (int it = 0; it < 8; ++it) {
        const float ym = 0.5f * (y_air + y_rock);
        if (terrain_density(w, wp, table, x, ym, z) > 0.0f) y_rock = ym; else y_air = ym;
    }
    ground = y_rock;
    return true;
}

// --- layer 8: trees -----------------------------------------------------------
// Cheapest test first. Most candidate cells are in a clearing or a treeless
// biome and are rejected on the blend and the patch noise alone; the height,
// slope and surface checks only run for the cells that survive those.
bool tree_at_cell(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                  int i, int j, TreeInstance& out, int* reason) {
#define TREE_NO(r) do { if (reason) *reason = (r); return false; } while (0)
    const TreeTable& tt = table.trees;
    const size_t ns = tt.species.size();
    if (ns == 0) TREE_NO(TREE_NO_LAND);

    const float u_x = tree_unit(tree_hash(i, j, w.s_trees, 0u));
    const float u_z = tree_unit(tree_hash(i, j, w.s_trees, 1u));
    // Jitter inside the middle 80% of the cell: trunks never closer than a
    // fifth of a cell (0.8 m at 4 m), so two never stand in one another.
    const float x = (static_cast<float>(i) + 0.1f + 0.8f * u_x) * tt.cell;
    const float z = (static_cast<float>(j) + 0.1f + 0.8f * u_z) * tt.cell;
    if (x * x + z * z > wp.radius * wp.radius) TREE_NO(TREE_OUTSIDE);

    int   bio[4];
    float bw[4];
    const int nb = biome_mix(w, wp, x, z, bio, bw);
    if (nb == 0) TREE_NO(TREE_NO_LAND);
    // Sharpen the blend for trees. Heights need the full 500 m blend; trees
    // do not, and with it a forest neighbour still carried enough weight at
    // the CENTRE of a rift cell to grow 14 trees a hectare there. Each weight
    // to an integer power, renormalised: still continuous at a border, where
    // equal weights stay equal, but a biome's trees keep to its own ground.
    // Kept to the land share: a coastal fade stays a coastal fade.
    {
        float land = 0.0f, sharp = 0.0f;
        float sw[4];
        for (int k = 0; k < nb; ++k) {
            land = land + bw[k];
            float p = 1.0f;
            for (int e = 0; e < tt.blend_sharpen; ++e) p = p * bw[k];
            sw[k] = p;
            sharp = sharp + p;
        }
        if (sharp > 0.0f) {
            for (int k = 0; k < nb; ++k) bw[k] = sw[k] / sharp * land;
        }
    }

    // Density and cover blend across a border like every other biome property.
    // The patch is evaluated per biome and mixed, rather than with mixed cover,
    // so each biome's own patches carry on to its edge instead of reshaping.
    float density = 0.0f;
    for (int k = 0; k < nb; ++k) {
        const BiomeArchetype& a = table[bio[k]];
        if (a.tree_density <= 0.0f) continue;
        const float patch = tree_patch(x, z, tt.patch_freq, a.tree_cover, tt.patch_edge, w.s_tree_patch);
        density = density + bw[k] * a.tree_density * patch;
    }
    if (density <= 0.0f) TREE_NO(TREE_CLEARING);
    const float u_keep = tree_unit(tree_hash(i, j, w.s_trees, 2u));
    if (u_keep >= density) TREE_NO(TREE_ROLL);
    // A flat site is a glade: kept clear, with room past its shoulder.
    if (flat_site_at(w, x, z, table.flats.tree_clear) >= 0) TREE_NO(TREE_FLAT);

    // The ground. Shore, slope and river first: they need only heights.
    const float r = std::sqrt(x * x + z * z) / wp.radius;
    const float h = terrain_height(w, wp, table, x, z, r);
    if (h < table.sea_level + tt.shore) TREE_NO(TREE_SHORE);
    const float e = 0.5f * tt.cell;
    const float hx = terrain_height(w, wp, table, x + e, z, r) - terrain_height(w, wp, table, x - e, z, r);
    const float hz = terrain_height(w, wp, table, x, z + e, r) - terrain_height(w, wp, table, x, z - e, r);
    const float slope2 = (hx * hx + hz * hz) / (4.0f * e * e);
    if (slope2 > tt.max_slope * tt.max_slope) TREE_NO(TREE_SLOPE);
    float rw = 0.0f, rd = 0.0f;
    // Two statements, not one. `nearest_river(...) < rw + 1.0f` reads rw in
    // the same expression as the call that writes it, and C++ leaves that
    // order unspecified: MSVC read rw first (still 0, so almost nothing was
    // ever rejected) and GCC called first (the real width). v12 grew 95
    // trees in a tile on Windows and 42 on Linux from this line alone.
    const float river_d = nearest_river(w, wp, x, z, rw, rd);
    if (river_d < rw + 1.0f) TREE_NO(TREE_RIVER);

    // Which species: biome weight times altitude fit. The fit is what moves
    // a forest from oak to fir uphill, and what makes the treeline.
    float total = 0.0f;
    float fit_of[64];
    float wsp[64];
    const size_t nsp = ns < 64 ? ns : 64;
    for (size_t s = 0; s < nsp; ++s) {
        float ws = 0.0f;
        for (int k = 0; k < nb; ++k) {
            const BiomeArchetype& a = table[bio[k]];
            if (a.tree_density <= 0.0f) continue;
            ws = ws + bw[k] * a.tree_species[s];
        }
        const TreeSpecies& sp = tt.species[s];
        fit_of[s] = tree_altitude_fit(h - table.sea_level, sp.alt_lo, sp.alt_hi, tt.altitude_ramp);
        wsp[s] = ws * fit_of[s];
        total = total + wsp[s];
    }
    if (total <= 0.0f) TREE_NO(TREE_ALTITUDE);
    // The fit also thins: where every candidate species fits badly the cell
    // is usually rejected, so a treeline fades rather than stopping at a wall.
    float best_fit = 0.0f;
    for (size_t s = 0; s < nsp; ++s) {
        if (wsp[s] > 0.0f && fit_of[s] > best_fit) best_fit = fit_of[s];
    }
    if (tree_unit(tree_hash(i, j, w.s_trees, 3u)) >= best_fit) TREE_NO(TREE_FIT_ROLL);

    const float u_sp = tree_unit(tree_hash(i, j, w.s_trees, 4u)) * total;
    size_t pick = nsp;
    float acc = 0.0f;
    for (size_t s = 0; s < nsp; ++s) {
        if (wsp[s] <= 0.0f) continue;
        acc = acc + wsp[s];
        pick = s;
        if (u_sp < acc) break;
    }
    if (pick >= nsp) TREE_NO(TREE_ALTITUDE);

    // Stand the trunk on the REAL surface (real_ground, above).
    float ground = 0.0f;
    if (!real_ground(w, wp, table, x, z, h, nb, bio, bw, ground)) TREE_NO(TREE_SURFACE);
    // No roof: a tree under an overhang would grow through it.
    if (terrain_density(w, wp, table, x, ground + 3.0f, z) > 0.0f) TREE_NO(TREE_SURFACE);
    if (terrain_density(w, wp, table, x, ground + 8.0f, z) > 0.0f) TREE_NO(TREE_SURFACE);

    const TreeSpecies& sp = tt.species[pick];
    const float u_h = tree_unit(tree_hash(i, j, w.s_trees, 5u));
    // Stunted towards the ends of its band: 55% of full size at the edge.
    const float stunt = 0.55f + 0.45f * fit_of[pick];
    out.x = x;
    out.y = ground;
    out.z = z;
    out.height = (sp.height_min + (sp.height_max - sp.height_min) * u_h) * stunt;
    out.yaw01 = tree_unit(tree_hash(i, j, w.s_trees, 6u));
    out.lean01 = tree_unit(tree_hash(i, j, w.s_trees, 7u));
    out.id = tree_id(i, j);
    out.species = static_cast<uint16_t>(pick);
    out.variant = static_cast<uint16_t>(tree_hash(i, j, w.s_trees, 8u) & 0xFFu);
    if (reason) *reason = TREE_KEPT;
    return true;
#undef TREE_NO
}

void trees_in_cells(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                    int i0, int j0, int i1, int j1, std::vector<TreeInstance>& out) {
    TreeInstance t;
    for (int j = j0; j < j1; ++j) {
        for (int i = i0; i < i1; ++i) {
            if (tree_at_cell(w, wp, table, i, j, t)) out.push_back(t);
        }
    }
}

// --- layer 9: rocks -------------------------------------------------------------
// Layer 8's shape: hashed rolls per cell, biome properties blended across a
// border, the real surface. Then two exclusions the trees do not need: a
// rock keeps clear of every trunk (trees are pinned, so rocks give way), and
// a boulder keeps clear of every outcrop.
bool rock_at_cell(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                  int kind, int i, int j, RockInstance& out, int* reason) {
#define ROCK_NO(r) do { if (reason) *reason = (r); return false; } while (0)
    const RockTable& rt = table.rocks;
    if (!rt.present) ROCK_NO(ROCK_NO_LAND);
    const RockClass& rc = kind == 0 ? rt.boulder : rt.outcrop;
    const uint32_t k0 = kind == 0 ? 0u : 16u;   // each class its own rolls

    const float u_x = tree_unit(tree_hash(i, j, w.s_rocks, k0 + 0u));
    const float u_z = tree_unit(tree_hash(i, j, w.s_rocks, k0 + 1u));
    const float x = (static_cast<float>(i) + 0.1f + 0.8f * u_x) * rc.cell;
    const float z = (static_cast<float>(j) + 0.1f + 0.8f * u_z) * rc.cell;
    if (x * x + z * z > wp.radius * wp.radius) ROCK_NO(ROCK_OUTSIDE);

    int   bio[4];
    float bw[4];
    const int nb = biome_mix(w, wp, x, z, bio, bw);
    if (nb == 0) ROCK_NO(ROCK_NO_LAND);
    {
        float land = 0.0f, sharp = 0.0f;
        float sw[4];
        for (int k = 0; k < nb; ++k) {
            land = land + bw[k];
            float p = 1.0f;
            for (int e = 0; e < rt.blend_sharpen; ++e) p = p * bw[k];
            sw[k] = p;
            sharp = sharp + p;
        }
        if (sharp > 0.0f) {
            for (int k = 0; k < nb; ++k) bw[k] = sw[k] / sharp * land;
        }
    }
    float density = 0.0f;
    int dom = 0;
    for (int k = 0; k < nb; ++k) {
        if (bw[k] > bw[dom]) dom = k;
        const BiomeArchetype& a = table[bio[k]];
        const float d = kind == 0 ? a.rock_boulders : a.rock_outcrops;
        if (d <= 0.0f) continue;
        const float patch = tree_patch(x, z, rt.patch_freq, a.rock_cover, rt.patch_edge, w.s_rock_patch);
        density = density + bw[k] * d * patch;
    }
    if (density <= 0.0f) ROCK_NO(ROCK_CLEARING);
    if (tree_unit(tree_hash(i, j, w.s_rocks, k0 + 2u)) >= density) ROCK_NO(ROCK_ROLL);

    // Sizes skewed to the small end: a cube of a uniform.
    const float u_s = tree_unit(tree_hash(i, j, w.s_rocks, k0 + 3u));
    const float size = rc.size_min + (rc.size_max - rc.size_min) * (u_s * u_s * u_s);
    const float rad = 0.45f * size;

    if (flat_site_at(w, x, z, table.flats.tree_clear + rad) >= 0) ROCK_NO(ROCK_FLAT);
    const float r = std::sqrt(x * x + z * z) / wp.radius;
    const float h = terrain_height(w, wp, table, x, z, r);
    if (h < table.sea_level + rt.shore) ROCK_NO(ROCK_SHORE);
    const float e = rad > 1.0f ? rad : 1.0f;
    const float h_xp = terrain_height(w, wp, table, x + e, z, r), h_xm = terrain_height(w, wp, table, x - e, z, r);
    const float h_zp = terrain_height(w, wp, table, x, z + e, r), h_zm = terrain_height(w, wp, table, x, z - e, r);
    const float hx = h_xp - h_xm;
    const float hz = h_zp - h_zm;
    const float slope = std::sqrt((hx * hx + hz * hz) / (4.0f * e * e));
    // How far the ground falls below the centre at the footprint's edge: the
    // lowest of the four, which a slope estimate misses on a crest, where
    // opposite sides cancel and the ground still falls away all round.
    float lowest = h_xp;
    if (h_xm < lowest) lowest = h_xm;
    if (h_zp < lowest) lowest = h_zp;
    if (h_zm < lowest) lowest = h_zm;
    const float drop = h > lowest ? h - lowest : 0.0f;
    if (slope > rc.max_slope) ROCK_NO(ROCK_SLOPE);
    if (rc.slope_hi > rc.slope_lo) {
        float want = (slope - rc.slope_lo) / (rc.slope_hi - rc.slope_lo);
        want = want < 0.0f ? 0.0f : (want > 1.0f ? 1.0f : want);
        if (tree_unit(tree_hash(i, j, w.s_rocks, k0 + 4u)) >= want) ROCK_NO(ROCK_SLOPE);
    }
    float rw = 0.0f, rd = 0.0f;
    // Two statements: see the river exclusion in tree_at_cell.
    const float river_d = nearest_river(w, wp, x, z, rw, rd);
    if (river_d < rw + rad + 1.0f) ROCK_NO(ROCK_RIVER);

    float ground = 0.0f;
    if (!real_ground(w, wp, table, x, z, h, nb, bio, bw, ground)) ROCK_NO(ROCK_SURFACE);
    // No roof over it.
    if (terrain_density(w, wp, table, x, ground + size + 1.0f, z) > 0.0f) ROCK_NO(ROCK_SURFACE);

    // Clear of every trunk: the tree cells whose jitter box can reach.
    {
        const float tc = table.trees.cell;
        const float reach = rad + 0.6f;
        const int ti0 = static_cast<int>(std::floor((x - reach) / tc));
        const int ti1 = static_cast<int>(std::floor((x + reach) / tc));
        const int tj0 = static_cast<int>(std::floor((z - reach) / tc));
        const int tj1 = static_cast<int>(std::floor((z + reach) / tc));
        for (int tj = tj0; tj <= tj1; ++tj) {
            for (int ti = ti0; ti <= ti1; ++ti) {
                TreeInstance t;
                if (!tree_at_cell(w, wp, table, ti, tj, t)) continue;
                const float dx = t.x - x, dz = t.z - z;
                if (dx * dx + dz * dz < reach * reach) ROCK_NO(ROCK_TREE);
            }
        }
    }
    // A boulder clear of every outcrop.
    if (kind == 0) {
        const RockClass& oc = rt.outcrop;
        const float reach = rad + 0.45f * oc.size_max;
        const int oi0 = static_cast<int>(std::floor((x - reach) / oc.cell));
        const int oi1 = static_cast<int>(std::floor((x + reach) / oc.cell));
        const int oj0 = static_cast<int>(std::floor((z - reach) / oc.cell));
        const int oj1 = static_cast<int>(std::floor((z + reach) / oc.cell));
        for (int oj = oj0; oj <= oj1; ++oj) {
            for (int oi = oi0; oi <= oi1; ++oi) {
                RockInstance o;
                if (!rock_at_cell(w, wp, table, 1, oi, oj, o)) continue;
                const float dx = o.x - x, dz = o.z - z;
                const float lim = rad + 0.45f * o.size;
                if (dx * dx + dz * dz < lim * lim) ROCK_NO(ROCK_OUTCROP);
            }
        }
    }

    // Set below the LOWEST ground under the footprint, not the centre's: on
    // a slope or a crest the ground falls away at the edge, and a rock set
    // from its centre showed its whole face there -- an outcrop read as a
    // block standing on the hill, not bedrock coming out of it.
    out.x = x;
    out.y = ground - drop - rc.sink * size;
    out.z = z;
    out.ground = ground;
    out.size = size;
    out.yaw01 = tree_unit(tree_hash(i, j, w.s_rocks, k0 + 5u));
    out.tilt01 = tree_unit(tree_hash(i, j, w.s_rocks, k0 + 6u));
    out.id = rock_id(kind, i, j);
    out.kind = static_cast<uint8_t>(kind);
    out.variant = static_cast<uint8_t>(tree_hash(i, j, w.s_rocks, k0 + 7u) & 0xFFu);
    out.biome = static_cast<uint16_t>(bio[dom]);
    if (reason) *reason = ROCK_KEPT;
    return true;
#undef ROCK_NO
}

void rocks_in_cells(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                    int kind, int i0, int j0, int i1, int j1, std::vector<RockInstance>& out) {
    RockInstance r;
    for (int j = j0; j < j1; ++j) {
        for (int i = i0; i < i1; ++i) {
            if (rock_at_cell(w, wp, table, kind, i, j, r)) out.push_back(r);
        }
    }
}

// Signed distance to the nearest shaft wall, through the XZ bin grid. Shafts
// are near-vertical, so binning them in the plane the way the rivers are
// leaves very few candidates per sample.
float nearest_entrance(const SeedWorld& w, const WorldParams& wp,
                       float x, float y, float z) {
    if (w.entrances.empty()) return 3.4e38f;
    const float R = wp.radius;
    const int gx = std::min(w.egrid_n - 1, std::max(0, static_cast<int>((x + R) / w.egrid_cell)));
    const int gz = std::min(w.egrid_n - 1, std::max(0, static_cast<int>((z + R) / w.egrid_cell)));
    float best = 3.4e38f;
    for (int j = gz - 1; j <= gz + 1; ++j) {
        if (j < 0 || j >= w.egrid_n) continue;
        for (int i = gx - 1; i <= gx + 1; ++i) {
            if (i < 0 || i >= w.egrid_n) continue;
            const std::vector<int>& bin = w.egrid[static_cast<size_t>(j) * w.egrid_n + i];
            for (size_t m = 0; m < bin.size(); ++m) {
                const SeedWorld::EntranceSeg& e = w.entrances[bin[m]];
                const float sd = entrance_tube_sd(x, y, z,
                                                  e.ax, e.ay, e.az, e.ra,
                                                  e.bx, e.by, e.bz, e.rb);
                if (sd < best) best = sd;
            }
        }
    }
    return best;
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

// --- layer 7: trace the way in ---------------------------------------------
//
// A candidate mouth is a point on land. The walk descends from it, asking the
// finished layer-6 density at every step whether it has broken into air yet.
// If it has, the polyline is kept; if it runs out of steps, the candidate is
// thrown away. Nothing is stored that did not reach a cave, which is where the
// connectivity guarantee comes from -- the same shape of argument as priority
// flood making rivers reach the sea, rather than tracing something plausible
// and asserting afterwards that it worked.

// A heading in the XZ plane, uniform in angle, without trig. Rejection
// sampling normalised with sqrt: cos/sin here would move the stored vertices
// by an ULP, and invariant 9 exists because exactly that moved a river.
static bool unit_heading(Rng& rng, float& hx, float& hz) {
    for (int t = 0; t < 24; ++t) {
        const float ax = rng.uniform() * 2.0f - 1.0f;
        const float az = rng.uniform() * 2.0f - 1.0f;
        const float d2 = ax * ax + az * az;
        if (d2 > 1.0e-8f && d2 <= 1.0f) {
            const float inv = 1.0f / std::sqrt(d2);
            hx = ax * inv;
            hz = az * inv;
            return true;
        }
    }
    return false;
}

// Descend from a point, wandering, until the rock opens. tilt is lateral
// metres per metre of drop, so 0 is a plumb shaft and 1 is a 45-degree ramp.
static bool walk_shaft(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                       Rng& rng, float sx, float sy, float sz, float surf,
                       int max_steps, float tilt,
                       std::vector<float>& px, std::vector<float>& py, std::vector<float>& pz) {
    float hx = 0.0f, hz = 0.0f;
    if (!unit_heading(rng, hx, hz)) return false;

    float cx = sx, cy = sy, cz = sz;
    px.clear(); py.clear(); pz.clear();
    px.push_back(cx); py.push_back(cy); pz.push_back(cz);

    for (int st = 0; st < max_steps; ++st) {
        // Wander the heading and renormalise. A straight shaft reads as a
        // mineshaft; nudging the vector and dividing by its length keeps the
        // wander trig-free.
        hx += (rng.uniform() * 2.0f - 1.0f) * 0.45f;
        hz += (rng.uniform() * 2.0f - 1.0f) * 0.45f;
        const float hd = hx * hx + hz * hz;
        if (hd > 1.0e-8f) {
            const float hinv = 1.0f / std::sqrt(hd);
            hx *= hinv;
            hz *= hinv;
        }
        const float ex = hx * tilt, ey = -1.0f, ez = hz * tilt;
        const float el = std::sqrt(ex * ex + ey * ey + ez * ez);
        const float k = wp.entrance_step / el;
        cx += ex * k;
        cy += ey * k;
        cz += ez * k;
        px.push_back(cx); py.push_back(cy); pz.push_back(cz);

        // Air within entrance_min_depth of the surface is an overhang, not a
        // cave. Without this the walk terminates one step in, under the lip it
        // started on, and the shaft connects the surface to itself.
        if (surf - cy < wp.entrance_min_depth) continue;
        if (terrain_density(w, wp, table, cx, cy, cz) <= 0.0f) return true;
    }
    return false;
}

// Turn a polyline into segments carrying the radius profile.
static void emit_shaft(std::vector<SeedWorld::EntranceSeg>& out, const WorldParams& wp,
                       const std::vector<float>& px, const std::vector<float>& py,
                       const std::vector<float>& pz, float mouth) {
    const int n = static_cast<int>(px.size());
    if (n < 2) return;
    const float inv = 1.0f / static_cast<float>(n - 1);
    for (int i = 0; i + 1 < n; ++i) {
        SeedWorld::EntranceSeg e;
        e.ax = px[i];     e.ay = py[i];     e.az = pz[i];
        e.bx = px[i + 1]; e.by = py[i + 1]; e.bz = pz[i + 1];
        e.ra = entrance_radius(static_cast<float>(i) * inv, mouth,
                               wp.entrance_throat, wp.entrance_bowl, wp.entrance_bowls);
        e.rb = entrance_radius(static_cast<float>(i + 1) * inv, mouth,
                               wp.entrance_throat, wp.entrance_bowl, wp.entrance_bowls);
        out.push_back(e);
    }
}

bool caves_enabled(const ArchetypeTable& table) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].cave_density > 0.0f) return true;
    }
    return false;
}

void build_entrances(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table) {
    // No caves, no shafts. The bin grid stays empty (egrid_n 0), which both
    // nearest_entrance and the engine's real_nearest_entrance read as "none".
    if (!caves_enabled(table)) return;
    const float R = wp.radius;
    Rng rng(w.s_entrance);

    // Accumulated locally and installed at the end. Appending to w.entrances as
    // we go would let one shaft's walk terminate in a previous shaft's air,
    // which is the same feedback trap terrain_base exists to avoid in layer 5.
    std::vector<SeedWorld::EntranceSeg> segs;
    std::vector<SeedWorld::EntranceMouth> mouths;
    std::vector<float> px, py, pz;
    int found = 0, tries = 0;

    while (found < wp.entrance_target && tries < wp.entrance_tries) {
        ++tries;

        // A mouth somewhere on land. Rejection inside the unit disc is already
        // uniform in AREA, which is the distribution wanted here; sampling a
        // radius uniformly would crowd every entrance into the middle.
        float ux = 0.0f, uz = 0.0f, ud = 0.0f;
        bool ok = false;
        for (int t = 0; t < 24 && !ok; ++t) {
            ux = rng.uniform() * 2.0f - 1.0f;
            uz = rng.uniform() * 2.0f - 1.0f;
            ud = ux * ux + uz * uz;
            ok = (ud > 1.0e-8f) && (ud <= 0.92f * 0.92f);   // clear of the edge taper
        }
        if (!ok) continue;

        const float mx = ux * R, mz = uz * R;
        const float rr = std::sqrt(ud);
        const float surf = terrain_height(w, wp, table, mx, mz, rr);
        if (surf <= wp.sea_level + 6.0f) continue;

        // Lateral metres per metre of drop. The range is wide on purpose:
        // at 0.3 the shaft is a pit you fall into, at 1.8 it is a ramp you can
        // walk down. A narrow range gives every entrance in the world the same
        // gradient, which reads as a template.
        const float tilt = 0.30f + 1.50f * rng.uniform();
        // Start above ground so the bowl actually breaks the surface.
        if (!walk_shaft(w, wp, table, rng, mx, surf + 1.5f, mz, surf,
                        wp.entrance_max_steps, tilt, px, py, pz)) {
            continue;
        }
        ++found;
        SeedWorld::EntranceMouth em;
        em.x = mx; em.y = surf; em.z = mz;
        em.reach = surf - py.back();
        em.seg_begin = static_cast<int>(segs.size());
        emit_shaft(segs, wp, px, py, pz, wp.entrance_mouth);
        em.seg_count = static_cast<int>(segs.size()) - em.seg_begin;
        mouths.push_back(em);

        // One dead-end side passage, sometimes. It is not required to reach
        // anything: a passage that goes nowhere is a feature, and the
        // connectivity guarantee is carried by the trunk.
        if (rng.uniform() < wp.entrance_branch_p && px.size() >= 6) {
            const size_t third = px.size() / 3;
            const size_t at = third + static_cast<size_t>(rng.next()) % third;
            std::vector<float> bx, by, bz;
            walk_shaft(w, wp, table, rng, px[at], py[at], pz[at], surf,
                       wp.entrance_max_steps / 3, 1.1f + rng.uniform(), bx, by, bz);
            if (bx.size() >= 2) emit_shaft(segs, wp, bx, by, bz, wp.entrance_throat);
        }
    }

    w.entrances.swap(segs);
    w.entrance_mouths.swap(mouths);
    w.entrance_count = found;
    w.entrance_tries_used = tries;

    // Bin in XZ, padded by the widest radius the segment reaches so the 3x3
    // lookup cannot miss a chamber that bulges across a cell line.
    w.egrid_n = 128;
    w.egrid_cell = 2.0f * R / w.egrid_n;
    w.egrid.assign(static_cast<size_t>(w.egrid_n) * w.egrid_n, std::vector<int>());
    for (size_t sg = 0; sg < w.entrances.size(); ++sg) {
        const SeedWorld::EntranceSeg& e = w.entrances[sg];
        const float pad = std::max(e.ra, e.rb) + wp.entrance_falloff;
        const int i0 = std::max(0, static_cast<int>((std::min(e.ax, e.bx) - pad + R) / w.egrid_cell));
        const int i1 = std::min(w.egrid_n - 1, static_cast<int>((std::max(e.ax, e.bx) + pad + R) / w.egrid_cell));
        const int j0 = std::max(0, static_cast<int>((std::min(e.az, e.bz) - pad + R) / w.egrid_cell));
        const int j1 = std::min(w.egrid_n - 1, static_cast<int>((std::max(e.az, e.bz) + pad + R) / w.egrid_cell));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                w.egrid[static_cast<size_t>(j) * w.egrid_n + i].push_back(static_cast<int>(sg));
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
    w.s_overhang = feature_seed(world_seed, FEATURE_OVERHANG);
    w.s_cave = feature_seed(world_seed, FEATURE_CAVE);
    w.s_entrance = feature_seed(world_seed, FEATURE_ENTRANCE);
    w.s_trees = feature_seed(world_seed, FEATURE_TREES);
    w.s_tree_patch = feature_seed(world_seed, FEATURE_TREE_PATCH);
    w.s_rocks = feature_seed(world_seed, FEATURE_ROCKS);
    w.s_rock_patch = feature_seed(world_seed, FEATURE_ROCK_PATCH);
    w.s_detail = feature_seed(world_seed, FEATURE_DETAIL);
    w.s_flats = feature_seed(world_seed, FEATURE_FLATS);
    const uint32_t s_dang = w.s_dang, s_wx = w.s_wx, s_wz = w.s_wz;

    // Layer 0 first: the knee moves the offset, and everything from the
    // raster (layers 2 and 3) down reads the result.
    w.l0 = make_layer0(wp, world_seed);
    if (wp.knee) {
        apply_land_knee(w.l0, wp, w.knee_raw_land, w.knee_land, w.knee_ms);
    }
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

            const float h = layer0_height(x, z, rr, w.l0);
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
    if (!wp.knee) { w.knee_raw_land = w.land_fraction; w.knee_land = w.land_fraction; }

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

    // Layer 5b: level sites, clear of the channels just traced.
    build_flats(w, wp, table);

    // Layer 7 after that again: the descent walks against the finished
    // density, which needs the carved heightfield under it.
    build_entrances(w, wp, table);
    return w;
}

// --- layer 5b: flat sites -----------------------------------------------------
// One candidate per cell, a pure function of the cell like a tree: jitter,
// keep roll, radius. Kept only where the land is already gentle -- the pad
// levels the last metre or two, it does not cut a terrace into a hillside --
// dry, and clear of every channel. The level is the mean of the pad's own
// heights, so cut and fill balance.
void build_flats(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table) {
    w.flats.clear();
    w.fgrid.clear();
    const FlatTable& ft = table.flats;
    if (!ft.present || w.rgrid_n <= 0) return;
    bool any = false;
    for (size_t b = 0; b < table.size(); ++b) if (table[b].flat_chance > 0.0f) any = true;
    if (!any) return;

    const float R = wp.radius;
    const float sea = table.sea_level;
    // Sixteen directions without trigonometry: the axes, the diagonals and
    // the eight between them (cos and sin of 22.5 degrees as literals).
    const float c1 = 0.92387953f, s1 = 0.38268343f, d = 0.70710678f;
    const float dx16[16] = {1.0f, c1, d, s1, 0.0f, -s1, -d, -c1, -1.0f, -c1, -d, -s1, 0.0f, s1, d, c1};
    const float dz16[16] = {0.0f, s1, d, c1, 1.0f, c1, d, s1, 0.0f, -s1, -d, -c1, -1.0f, -c1, -d, -s1};
    auto height_at = [&](float x, float z) {
        return terrain_height(w, wp, table, x, z, std::sqrt(x * x + z * z) / R);
    };

    const int n = static_cast<int>(std::ceil(R / ft.cell));
    for (int j = -n; j < n; ++j) {
        for (int i = -n; i < n; ++i) {
            // Jitter inside the middle 56% of the cell: with the outer edge
            // under 0.22 cell (checked at load), two sites never touch.
            const float ux = tree_unit(tree_hash(i, j, w.s_flats, 0u));
            const float uz = tree_unit(tree_hash(i, j, w.s_flats, 1u));
            const float x = (static_cast<float>(i) + 0.22f + 0.56f * ux) * ft.cell;
            const float z = (static_cast<float>(j) + 0.22f + 0.56f * uz) * ft.cell;
            if (x * x + z * z > 0.81f * R * R) continue;

            int   bio[4];
            float bw[4];
            const int nb = biome_mix(w, wp, x, z, bio, bw);
            if (nb == 0) continue;
            float chance = 0.0f;
            int dom = 0;
            for (int k = 0; k < nb; ++k) {
                chance = chance + bw[k] * table[bio[k]].flat_chance;
                if (bw[k] > bw[dom]) dom = k;
            }
            if (tree_unit(tree_hash(i, j, w.s_flats, 2u)) >= chance) continue;

            const float ur = tree_unit(tree_hash(i, j, w.s_flats, 3u));
            const float radius = ft.radius_min + (ft.radius_max - ft.radius_min) * ur;
            const float sh = radius * ft.shoulder;
            const float shoulder = sh > ft.shoulder_min ? sh : ft.shoulder_min;
            const float outer = radius + shoulder;

            // Clear of every channel, edge to edge. Two statements: see the
            // river exclusion in tree_at_cell.
            float rw = 0.0f, rd = 0.0f;
            const float river_d = nearest_river(w, wp, x, z, rw, rd);
            if (river_d < rw + outer + ft.river_clear) continue;

            // The pad: centre, half radius, radius. Its mean is the level.
            const float hc = height_at(x, z);
            float sum = hc, lo = hc, hi = hc;
            int cnt = 1;
            for (int ring = 1; ring <= 2; ++ring) {
                const float rr = radius * 0.5f * static_cast<float>(ring);
                for (int a = 0; a < 16; ++a) {
                    const float h = height_at(x + dx16[a] * rr, z + dz16[a] * rr);
                    sum = sum + h;
                    if (h < lo) lo = h;
                    if (h > hi) hi = h;
                    ++cnt;
                }
            }
            const float level = sum / static_cast<float>(cnt);
            if (lo < sea + ft.shore) continue;
            // Already gentle: across the pad, and out to the shoulder's edge,
            // where the eased ring has to meet the land again.
            if (hi - lo > ft.max_grade * 2.0f * radius) continue;
            bool ok = true;
            for (int a = 0; a < 16 && ok; ++a) {
                const float h = height_at(x + dx16[a] * outer, z + dz16[a] * outer);
                const float dh = h - level;
                if (h < sea + 0.5f * ft.shore) ok = false;
                if (dh > ft.max_grade * outer || -dh > ft.max_grade * outer) ok = false;
            }
            if (!ok) continue;

            SeedWorld::FlatSite f;
            f.x = x;
            f.z = z;
            f.radius = radius;
            f.shoulder = shoulder;
            f.level = level;
            f.id = flat_id(i, j);
            f.biome = bio[dom];
            w.flats.push_back(f);
        }
    }

    // Binned on the river grid's geometry, into every bin the site or its
    // tree clearance touches, so a lookup reads one bin.
    const float Rg = 0.5f * w.rgrid_cell * static_cast<float>(w.rgrid_n);
    w.fgrid.assign(static_cast<size_t>(w.rgrid_n) * w.rgrid_n, std::vector<int>());
    for (size_t s = 0; s < w.flats.size(); ++s) {
        const SeedWorld::FlatSite& f = w.flats[s];
        const float pad = f.radius + f.shoulder + ft.tree_clear + 8.0f;
        const int i0 = std::max(0, static_cast<int>((f.x - pad + Rg) / w.rgrid_cell));
        const int i1 = std::min(w.rgrid_n - 1, static_cast<int>((f.x + pad + Rg) / w.rgrid_cell));
        const int j0 = std::max(0, static_cast<int>((f.z - pad + Rg) / w.rgrid_cell));
        const int j1 = std::min(w.rgrid_n - 1, static_cast<int>((f.z + pad + Rg) / w.rgrid_cell));
        for (int jj = j0; jj <= j1; ++jj)
            for (int ii = i0; ii <= i1; ++ii)
                w.fgrid[static_cast<size_t>(jj) * w.rgrid_n + ii].push_back(static_cast<int>(s));
    }
}

namespace {

// The calmest biome's share of the blend at a point; several of its cells
// can be among the four.
float calm_weight(const SeedWorld& w, const WorldParams& wp, int calm, float x, float z) {
    int   bio[4];
    float bw[4];
    const int nb = biome_mix(w, wp, x, z, bio, bw);
    float s = 0.0f;
    for (int k = 0; k < nb; ++k) if (bio[k] == calm) s = s + bw[k];
    return s;
}

}  // namespace

SpawnPoint find_spawn(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table) {
    const float kStep      = 16.0f;   // search grid, metres
    const float kSpawnCore = 0.75f;   // calmest weight at the point
    const float kRingCalm  = 0.5f;    // ... and at each ring point
    const float kRing      = 60.0f;   // ring radius, metres
    const float kDry       = 1.5f;    // metres above sea at the point
    const float kRingDry   = 0.5f;    // ... and on the ring
    const float kMaxGrade  = 0.25f;   // rise over run, point to each ring point
    const float kTrunk     = 2.5f;    // metres clear of any trunk

    SpawnPoint out;
    if (table.size() == 0) return out;
    int calm = 0;
    for (size_t b = 1; b < table.size(); ++b)
        if (table[b].danger_preferred < table[static_cast<size_t>(calm)].danger_preferred)
            calm = static_cast<int>(b);
    out.biome = calm;

    const float R  = wp.radius;
    const float sea = table.sea_level;
    // Eight ring directions without trigonometry: the axes and the diagonals.
    const float d = 0.70710678f;
    const float rx[8] = {1.0f, d, 0.0f, -d, -1.0f, -d, 0.0f, d};
    const float rz[8] = {0.0f, d, 1.0f, d, 0.0f, -d, -1.0f, -d};

    auto height_at = [&](float x, float z) {
        const float r = std::sqrt(x * x + z * z) / R;
        return terrain_height(w, wp, table, x, z, r);
    };
    auto passes = [&](float x, float z, float& ground) {
        if (calm_weight(w, wp, calm, x, z) < kSpawnCore) return false;
        ground = height_at(x, z);
        if (ground < sea + kDry) return false;
        for (int k = 0; k < 8; ++k) {
            const float px = x + rx[k] * kRing, pz = z + rz[k] * kRing;
            if (calm_weight(w, wp, calm, px, pz) < kRingCalm) return false;
            const float h = height_at(px, pz);
            if (h < sea + kRingDry) return false;
            const float dh = h - ground;
            if (dh > kMaxGrade * kRing || -dh > kMaxGrade * kRing) return false;
        }
        float rw = 0.0f, rd = 0.0f;
        const float dr = nearest_river(w, wp, x, z, rw, rd);
        if (dr < rw + 10.0f) return false;
        const float cell = table.trees.cell;
        const int ci = static_cast<int>(std::floor(x / cell));
        const int cj = static_cast<int>(std::floor(z / cell));
        for (int j = cj - 1; j <= cj + 1; ++j) {
            for (int i = ci - 1; i <= ci + 1; ++i) {
                TreeInstance t;
                if (!tree_at_cell(w, wp, table, i, j, t)) continue;
                const float tx = t.x - x, tz = t.z - z;
                if (tx * tx + tz * tz < kTrunk * kTrunk) return false;
            }
        }
        return true;
    };

    // Square rings of grid points outward from the origin. A point found in
    // ring k may not be the nearest -- ring k + 1's middle is nearer than
    // ring k's corner -- so the search carries on until a ring cannot hold
    // anything nearer. Ties go to the first point visited.
    float best_d2 = 3.4e38f;
    bool  found = false;
    float fb_w = -1.0f, fb_d2 = 3.4e38f;
    const int kmax = static_cast<int>(R / kStep);
    for (int k = 0; k <= kmax; ++k) {
        const float inner = static_cast<float>(k) * kStep;
        if (found && inner * inner > best_d2) break;
        for (int j = -k; j <= k; ++j) {
            for (int i = -k; i <= k; ++i) {
                if (i != -k && i != k && j != -k && j != k) continue;   // the ring only
                const float x = static_cast<float>(i) * kStep;
                const float z = static_cast<float>(j) * kStep;
                const float d2 = x * x + z * z;
                if (d2 > 0.9f * 0.9f * R * R || d2 >= best_d2) continue;
                float g = 0.0f;
                if (passes(x, z, g)) {
                    best_d2 = d2; found = true;
                    out.x = x; out.z = z; out.ground = g; out.strict = true;
                    out.weight = calm_weight(w, wp, calm, x, z);
                } else if (!found) {
                    const float cw = calm_weight(w, wp, calm, x, z);
                    if (cw > fb_w || (cw == fb_w && d2 < fb_d2)) {
                        const float h = height_at(x, z);
                        if (h >= sea + kDry) {
                            fb_w = cw; fb_d2 = d2;
                            out.x = x; out.z = z; out.ground = h; out.weight = cw;
                        }
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace worldgen
