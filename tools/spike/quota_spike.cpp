// ---------------------------------------------------------------------------
// Quota spike -- does layer 3 actually deliver "every biome, every seed, within
// tolerance, radially ordered"?
//
// No graphics. It scatters cells, rasterises the disc to get real cell areas
// and a land mask, runs the real worldgen/assign/ code, and reports the four
// numbers the validation harness will later assert on:
//
//   presence      every biome gets at least one cell, every seed
//   quota         per-biome land share within +/-10% of target
//   radial order  Spearman(danger_preferred, mean normalised area) >= 0.8
//   land fraction 55% +/- 5%
//
// The headline question it exists to answer: at R = 10 km the scale table
// implies ~15 land cells per biome, so quota granularity is ~+/-7% against a
// +/-10% budget. Is that enough headroom, and what does the contiguity bonus
// cost us?
// ---------------------------------------------------------------------------
#include "../../worldgen/assign/assign.h"
#include "../../worldgen/assign/archetypes.h"
#include "noise_standin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace spike;
using namespace worldgen;

// --- world parameters ------------------------------------------------------
struct WorldParams {
    float radius            = 10000.0f;   // R, metres
    float cell_area_target  = 1.28e6f;    // m^2; held constant across R
    int   grid              = 1024;       // sample grid per side

    // layer 0 stand-in
    float continent_freq    = 1.0f / 5200.0f;
    int   continent_octaves = 5;
    float centre_bias       = 0.70f;      // A
    float edge_drop         = 1.60f;      // B
    float sea_level         = 0.0f;
    // Stand-in only: shifts the continent mask to hit the 55% land target.
    // In the real generator this is sea level, which is a world constant.
    float land_offset       = 0.17f;   // calibrated to the 55% land target

    // layer 1
    float danger_w          = 0.20f;      // the feel parameter
    float danger_freq       = 1.0f / 4500.0f;
    int   danger_octaves    = 3;

    // layer 2
    float warp_amp          = 1300.0f;    // ~one cell radius
    float warp_freq         = 1.0f / 6000.0f;
};

// Layer 0 stand-in: continent mask and edge taper.
static inline float layer0_height(float x, float z, float r, const WorldParams& wp, uint32_t seed) {
    return fbm(x * wp.continent_freq, z * wp.continent_freq,
               wp.continent_octaves, 2.0f, 0.5f, seed)
         + wp.land_offset
         + wp.centre_bias * (1.0f - smoothstep(0.0f, 0.25f, r))
         - wp.edge_drop   * smoothstep(0.80f, 1.0f, r);
}

// Layer 1: danger on normalised AREA, not normalised radius.
static inline float layer1_danger(float x, float z, float r, const WorldParams& wp, uint32_t seed) {
    const float r2 = r * r;
    return saturate(r2 + wp.danger_w * fbm(x * wp.danger_freq, z * wp.danger_freq,
                                           wp.danger_octaves, 2.0f, 0.5f, seed));
}

// --- layer 2: blue-noise scatter (Bridson) ---------------------------------
static std::vector<std::pair<float, float> > scatter_cells(const WorldParams& wp, uint32_t seed) {
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
            const float ang = rng.uniform() * 6.28318530718f;
            const float rad = rmin * (1.0f + rng.uniform());
            const float x = pts[pi].first  + std::cos(ang) * rad;
            const float z = pts[pi].second + std::sin(ang) * rad;
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

// --- per-seed evaluation ---------------------------------------------------
struct SeedResult {
    int   cells = 0, land_cells = 0;
    float land_fraction = 0.0f;
    float max_quota_err = 0.0f;   // max |relative error| over biomes
    float spearman = 0.0f;
    int   missing_biomes = 0;
    int   relaxed = 0, overflow = 0;
    float mean_candidates = 0.0f;
    float mean_patch_cells = 0.0f;
    float mean_patch_km2 = 0.0f;
    std::vector<int>   biome_cells;   // per biome
    std::vector<float> biome_share;   // per biome, share of land area
};

static SeedResult eval_seed(uint64_t world_seed,
                            const WorldParams& wp,
                            const ArchetypeTable& table,
                            const AssignParams& ap) {
    SeedResult out;

    const uint32_t s_cont = feature_seed(world_seed, FEATURE_CONTINENT);
    const uint32_t s_dang = feature_seed(world_seed, FEATURE_DANGER);
    const uint32_t s_cell = feature_seed(world_seed, FEATURE_CELLS);
    const uint32_t s_wx   = feature_seed(world_seed, FEATURE_WARP_X);
    const uint32_t s_wz   = feature_seed(world_seed, FEATURE_WARP_Z);

    const std::vector<std::pair<float, float> > pts = scatter_cells(wp, s_cell);
    const int n = static_cast<int>(pts.size());
    out.cells = n;
    if (n == 0) return out;

    // Bucket grid for nearest-cell lookup, ~2 cells per bucket.
    const int B = std::max(4, static_cast<int>(std::ceil(std::sqrt(n / 2.0))));
    const float R = wp.radius;
    const float bucket = 2.0f * R / B;
    std::vector<std::vector<int> > buckets(static_cast<size_t>(B) * B);
    for (int i = 0; i < n; ++i) {
        const int bx = std::min(B - 1, std::max(0, static_cast<int>((pts[i].first  + R) / bucket)));
        const int bz = std::min(B - 1, std::max(0, static_cast<int>((pts[i].second + R) / bucket)));
        buckets[static_cast<size_t>(bz) * B + bx].push_back(i);
    }

    auto nearest = [&](float x, float z) {
        const int bx = std::min(B - 1, std::max(0, static_cast<int>((x + R) / bucket)));
        const int bz = std::min(B - 1, std::max(0, static_cast<int>((z + R) / bucket)));
        int   best = -1;
        float best_d2 = 3.4e38f;
        for (int ring = 0; ring < B; ++ring) {
            if (best >= 0) {
                const float safe = static_cast<float>(ring - 1) * bucket;
                if (safe > 0.0f && safe * safe > best_d2) break;
            }
            const int x0 = bx - ring, x1 = bx + ring, z0 = bz - ring, z1 = bz + ring;
            for (int j = z0; j <= z1; ++j) {
                if (j < 0 || j >= B) continue;
                for (int i = x0; i <= x1; ++i) {
                    if (i < 0 || i >= B) continue;
                    // only the ring shell
                    if (ring > 0 && i != x0 && i != x1 && j != z0 && j != z1) continue;
                    const std::vector<int>& bk = buckets[static_cast<size_t>(j) * B + i];
                    for (size_t m = 0; m < bk.size(); ++m) {
                        const float dx = pts[bk[m]].first - x, dz = pts[bk[m]].second - z;
                        const float d2 = dx * dx + dz * dz;
                        if (d2 < best_d2) { best_d2 = d2; best = bk[m]; }
                    }
                }
            }
        }
        return best;
    };

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

    out.land_fraction = disc_area > 0.0 ? static_cast<float>(land_area / disc_area) : 0.0f;

    // Build cell records from the rasterised truth.
    std::vector<CellRecord> cells(n);
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

    CellGraph graph;
    graph.offset.resize(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        graph.offset[i] = static_cast<int>(graph.list.size());
        for (int j = 0; j < n; ++j) if (adj[static_cast<size_t>(i) * n + j]) graph.list.push_back(j);
    }
    graph.offset[n] = static_cast<int>(graph.list.size());

    // --- the thing under test ---
    std::vector<int> biome;
    AssignStats stats;
    assign_biomes(cells, graph, table, ap, biome, stats);

    out.land_cells      = stats.land_cells;
    out.relaxed         = stats.relaxed_cells;
    out.overflow        = stats.overflow_cells;
    out.mean_candidates = stats.mean_candidates;

    // --- metrics ---
    const size_t nb = table.size();
    std::vector<double> got(nb, 0.0), mean_na(nb, 0.0);
    std::vector<int> count(nb, 0);
    double total_land = 0.0;
    for (int i = 0; i < n; ++i) {
        if (biome[i] < 0) continue;
        got[biome[i]] += cells[i].land_area;
        mean_na[biome[i]] += cells[i].norm_area;
        count[biome[i]] += 1;
        total_land += cells[i].land_area;
    }

    out.biome_cells.assign(nb, 0);
    out.biome_share.assign(nb, 0.0f);
    for (size_t b = 0; b < nb; ++b) {
        const double actual = total_land > 0.0 ? got[b] / total_land : 0.0;
        out.biome_cells[b] = count[b];
        out.biome_share[b] = static_cast<float>(actual);
        if (count[b] == 0) { out.missing_biomes += 1; }
        else { mean_na[b] /= count[b]; }
        const double target = table[b].area_fraction;
        const double rel = std::fabs(actual - target) / target;
        out.max_quota_err = std::max(out.max_quota_err, static_cast<float>(rel));
    }

    // Spearman over biomes that got cells.
    {
        std::vector<int> idx;
        for (size_t b = 0; b < nb; ++b) if (count[b] > 0) idx.push_back(static_cast<int>(b));
        const int m = static_cast<int>(idx.size());
        if (m >= 3) {
            std::vector<int> ra(m), rb(m);
            std::vector<int> ord(m);
            for (int i = 0; i < m; ++i) ord[i] = i;
            std::sort(ord.begin(), ord.end(), [&](int a, int b2) {
                return table[idx[a]].danger_preferred < table[idx[b2]].danger_preferred; });
            for (int i = 0; i < m; ++i) ra[ord[i]] = i;
            for (int i = 0; i < m; ++i) ord[i] = i;
            std::sort(ord.begin(), ord.end(), [&](int a, int b2) {
                return mean_na[idx[a]] < mean_na[idx[b2]]; });
            for (int i = 0; i < m; ++i) rb[ord[i]] = i;
            double d2 = 0.0;
            for (int i = 0; i < m; ++i) { const double d = ra[i] - rb[i]; d2 += d * d; }
            out.spearman = static_cast<float>(1.0 - 6.0 * d2 / (static_cast<double>(m) * (static_cast<double>(m) * m - 1.0)));
        } else {
            out.spearman = 0.0f;
        }
    }

    // Patch stats: connected components of same-biome land cells.
    {
        std::vector<char> seen(n, 0);
        std::vector<int> stack;
        int comps = 0, comp_cells = 0;
        double comp_area = 0.0;
        for (int i = 0; i < n; ++i) {
            if (seen[i] || biome[i] < 0) continue;
            ++comps;
            stack.push_back(i);
            seen[i] = 1;
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back();
                ++comp_cells;
                comp_area += cells[c].land_area;
                for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                    const int nb2 = graph.list[k];
                    if (!seen[nb2] && biome[nb2] == biome[c]) { seen[nb2] = 1; stack.push_back(nb2); }
                }
            }
        }
        if (comps > 0) {
            out.mean_patch_cells = static_cast<float>(comp_cells) / comps;
            out.mean_patch_km2   = static_cast<float>(comp_area / comps / 1.0e6);
        }
    }

    return out;
}

// --- aggregate over seeds --------------------------------------------------
struct Summary {
    int seeds = 0;
    int presence_failures = 0;
    int quota_failures = 0;      // max rel err > 0.10
    int radial_failures = 0;     // spearman < 0.8
    int land_failures = 0;       // |land - 0.55| > 0.05
    float mean_cells = 0, mean_land_cells = 0, mean_land_frac = 0;
    float mean_quota_err = 0, max_quota_err = 0, p95_quota_err = 0;
    float mean_spearman = 0, min_spearman = 2.0f;
    float mean_patch_cells = 0, mean_patch_km2 = 0;
    float mean_relaxed = 0, mean_overflow = 0;
    float mean_candidates = 0;
};

static Summary run(int nseeds, const WorldParams& wp, const ArchetypeTable& table, const AssignParams& ap) {
    std::vector<SeedResult> results(nseeds);
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, static_cast<unsigned>(nseeds));

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = static_cast<int>(t); s < nseeds; s += static_cast<int>(nthreads)) {
                results[s] = eval_seed(0x5EEDull * 1000003ull + static_cast<uint64_t>(s), wp, table, ap);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();

    Summary sm;
    sm.seeds = nseeds;
    std::vector<float> errs;
    errs.reserve(nseeds);
    for (int s = 0; s < nseeds; ++s) {
        const SeedResult& r = results[s];
        if (r.missing_biomes > 0) ++sm.presence_failures;
        if (r.max_quota_err > 0.10f) ++sm.quota_failures;
        if (r.spearman < 0.8f) ++sm.radial_failures;
        if (std::fabs(r.land_fraction - 0.55f) > 0.05f) ++sm.land_failures;
        sm.mean_cells += r.cells;
        sm.mean_land_cells += r.land_cells;
        sm.mean_land_frac += r.land_fraction;
        sm.mean_quota_err += r.max_quota_err;
        sm.max_quota_err = std::max(sm.max_quota_err, r.max_quota_err);
        sm.mean_spearman += r.spearman;
        sm.min_spearman = std::min(sm.min_spearman, r.spearman);
        sm.mean_patch_cells += r.mean_patch_cells;
        sm.mean_patch_km2 += r.mean_patch_km2;
        sm.mean_relaxed += r.relaxed;
        sm.mean_overflow += r.overflow;
        sm.mean_candidates += r.mean_candidates;
        errs.push_back(r.max_quota_err);
    }
    const float inv = 1.0f / nseeds;
    sm.mean_cells *= inv; sm.mean_land_cells *= inv; sm.mean_land_frac *= inv;
    sm.mean_quota_err *= inv; sm.mean_spearman *= inv;
    sm.mean_patch_cells *= inv; sm.mean_patch_km2 *= inv;
    sm.mean_relaxed *= inv; sm.mean_overflow *= inv; sm.mean_candidates *= inv;
    std::sort(errs.begin(), errs.end());
    sm.p95_quota_err = errs[std::min<size_t>(errs.size() - 1, static_cast<size_t>(errs.size() * 0.95))];
    return sm;
}

static void print_summary(const char* label, const Summary& s) {
    std::printf("%-24s %5.0f %5.0f %5.1f%%  %5.1f%% %5.1f%% %5.1f%%  %5.2f %5.2f  %4.1f %5.2f  %4.1f %4.1f  %3d %3d %3d %3d\n",
                label, s.mean_cells, s.mean_land_cells, 100.0 * s.mean_land_frac,
                100.0 * s.mean_quota_err, 100.0 * s.p95_quota_err, 100.0 * s.max_quota_err,
                s.mean_spearman, s.min_spearman,
                s.mean_patch_cells, s.mean_patch_km2,
                s.mean_relaxed, s.mean_candidates,
                s.presence_failures, s.quota_failures, s.radial_failures, s.land_failures);
}

static void print_header() {
    std::printf("\n%-24s %5s %5s %6s  %-21s %-12s %-11s %-10s %s\n",
                "config", "cells", "land", "land%", "quota err mean/p95/max",
                "spearman", "patch c/km2", "relax/cand", "fail: pres quota rad land");
    std::printf("%s\n", std::string(136, '-').c_str());
}

static void print_detail(const WorldParams& wp, const ArchetypeTable& table,
                         const AssignParams& ap, uint64_t seed) {
    const SeedResult r = eval_seed(seed, wp, table, ap);
    std::printf("\nper-biome, one seed (R=%.0fkm, contiguity=%.2f): %d cells, %d land, land %.1f%%\n",
                wp.radius / 1000.0f, ap.contiguity_bonus, r.cells, r.land_cells, 100.0f * r.land_fraction);
    std::printf("%-12s %6s %8s %8s %9s\n", "biome", "cells", "target", "actual", "rel err");
    std::printf("%s\n", std::string(48, '-').c_str());
    for (size_t b = 0; b < table.size(); ++b) {
        const float target = table[b].area_fraction;
        const float actual = r.biome_share[b];
        std::printf("%-12s %6d %7.1f%% %7.1f%% %8.1f%%\n",
                    table[b].name.c_str(), r.biome_cells[b],
                    100.0f * target, 100.0f * actual, 100.0f * (actual - target) / target);
    }
    std::printf("relaxed %d, overflow %d\n", r.relaxed, r.overflow);
}

int main(int argc, char** argv) {
    int  nseeds = 200;
    bool sweep = false;
    bool sweep_cells = false;
    bool sweep_bands = false;
    bool sweep_w = false;
    bool detail = false;
    WorldParams wp;
    AssignParams ap;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc)       nseeds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--grid") && i + 1 < argc)   wp.grid = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--radius") && i + 1 < argc) wp.radius = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--w") && i + 1 < argc)      wp.danger_w = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--contiguity") && i + 1 < argc) ap.contiguity_bonus = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--offset") && i + 1 < argc) wp.land_offset = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--sweep"))                  sweep = true;
        else if (!std::strcmp(argv[i], "--detail"))                 detail = true;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc)   ap.quota_mode = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cellarea") && i + 1 < argc) wp.cell_area_target = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--sweep-cells"))            sweep_cells = true;
        else if (!std::strcmp(argv[i], "--sweep-bands"))            sweep_bands = true;
        else if (!std::strcmp(argv[i], "--sweep-w"))                sweep_w = true;
    }

    ArchetypeTable table;
    try {
        table = load_archetypes("worldgen/data/archetypes.json");
    } catch (const std::exception& e) {
        std::printf("FAILED to load archetypes: %s\n", e.what());
        return 1;
    }
    std::printf("archetypes: %d biomes, invariants OK\n", static_cast<int>(table.size()));

    if (sweep_w) {
        // If the contiguity bonus is not what clumps cells into patches, the
        // danger field's own spatial correlation must be. Sweep w and the
        // danger feature size and watch patch size and radial ordering.
        const float ws[]    = {0.05f, 0.10f, 0.20f, 0.35f};
        const float feats[] = {2500.0f, 4500.0f, 8000.0f};
        print_header();
        for (size_t fi = 0; fi < 3; ++fi) {
            for (size_t wi = 0; wi < 4; ++wi) {
                WorldParams w2 = wp;
                w2.danger_w    = ws[wi];
                w2.danger_freq = 1.0f / feats[fi];
                AssignParams a2 = ap;
                a2.quota_mode = QUOTA_HALF_CELL;
                char label[64];
                std::snprintf(label, sizeof(label), "w=%.2f feat=%.1fkm", ws[wi], feats[fi] / 1000.0f);
                print_summary(label, run(nseeds, w2, table, a2));
            }
        }
        return 0;
    }

    if (sweep_bands) {
        // The candidate set is ~1 because quota exhaustion closes biomes out.
        // Widening the danger bands gives the cost function something to
        // choose between -- does the contiguity bonus come back to life, and
        // what does the wider band cost in radial ordering?
        const float scales[] = {1.0f, 1.5f, 2.0f, 3.0f};
        const float contig[] = {0.0f, 0.4f};
        print_header();
        for (size_t ci = 0; ci < 2; ++ci) {
            for (size_t si = 0; si < 4; ++si) {
                ArchetypeTable t2 = table;
                for (size_t b = 0; b < t2.biomes.size(); ++b) {
                    BiomeArchetype& a = t2.biomes[b];
                    const float lo = a.danger_preferred - (a.danger_preferred - a.danger_band_min) * scales[si];
                    const float hi = a.danger_preferred + (a.danger_band_max - a.danger_preferred) * scales[si];
                    a.danger_band_min = std::max(0.0f, lo);
                    a.danger_band_max = std::min(1.0f, hi);
                }
                AssignParams a2 = ap;
                a2.quota_mode = QUOTA_HALF_CELL;
                a2.contiguity_bonus = contig[ci];
                char label[64];
                std::snprintf(label, sizeof(label), "band x%.1f contig=%.1f", scales[si], contig[ci]);
                print_summary(label, run(nseeds, wp, t2, a2));
            }
        }
        return 0;
    }

    if (sweep_cells) {
        // At R = 10 km, how small do cells have to get before the +/-10%
        // quota gate passes -- and what does that cost in patch size?
        const float areas[] = {1.28e6f, 0.90e6f, 0.64e6f, 0.45e6f, 0.32e6f};
        const int   modes[] = {QUOTA_DOC, QUOTA_HALF_CELL};
        const char* names[] = {"doc", "half-cell"};
        print_header();
        for (size_t mi = 0; mi < 2; ++mi) {
            for (size_t ai = 0; ai < 5; ++ai) {
                WorldParams  w2 = wp; w2.cell_area_target = areas[ai];
                AssignParams a2 = ap; a2.quota_mode = modes[mi];
                char label[64];
                std::snprintf(label, sizeof(label), "%s cell=%.2fkm2", names[mi], areas[ai] / 1.0e6f);
                print_summary(label, run(nseeds, w2, table, a2));
            }
        }
        return 0;
    }

    if (detail) {
        print_detail(wp, table, ap, 0x5EEDull * 1000003ull);
        return 0;
    }

    if (!sweep) {
        std::printf("R = %.0f m, %d seeds, %d^2 grid, w = %.2f, contiguity = %.2f\n",
                    wp.radius, nseeds, wp.grid, wp.danger_w, ap.contiguity_bonus);
        print_header();
        char label[64];
        std::snprintf(label, sizeof(label), "R=%.0fkm c=%.2f", wp.radius / 1000.0f, ap.contiguity_bonus);
        print_summary(label, run(nseeds, wp, table, ap));
        return 0;
    }

    // The experiment: the doc's gate dumps the whole accumulated rounding
    // error on the last biome in sort order. Do the two alternative gates fix
    // it, and what do they cost in radial ordering?
    const float radii[] = {10000.0f, 20000.0f};
    const int   modes[] = {QUOTA_DOC, QUOTA_HALF_CELL, QUOTA_SOFT_FILL};
    const char* names[] = {"doc", "half-cell", "soft-fill"};
    print_header();
    for (size_t ri = 0; ri < 2; ++ri) {
        for (size_t mi = 0; mi < 3; ++mi) {
            WorldParams  w2 = wp; w2.radius = radii[ri];
            AssignParams a2 = ap; a2.quota_mode = modes[mi];
            char label[64];
            std::snprintf(label, sizeof(label), "R=%.0fkm gate=%s", radii[ri] / 1000.0f, names[mi]);
            print_summary(label, run(nseeds, w2, table, a2));
        }
    }
    return 0;
}
