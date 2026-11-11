#include "assign.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace worldgen {

namespace {

// Cost of placing biome b on this cell, given whatever neighbours are already
// assigned. Band membership is handled by the caller as a gate, not a cost.
float placement_cost(size_t b,
                     int cell,
                     const CellRecord& rec,
                     const CellGraph& graph,
                     const ArchetypeTable& table,
                     const AssignParams& params,
                     const std::vector<int>& biome) {
    float cost = std::fabs(rec.danger - table[b].danger_preferred);

    const int begin = graph.offset[cell];
    const int end   = graph.offset[cell + 1];
    for (int k = begin; k < end; ++k) {
        const int nb = graph.list[k];
        const int nb_biome = biome[nb];
        if (nb_biome < 0) continue;  // unassigned or ocean

        if ((table[b].adjacency_mask >> nb_biome) & 1u) {
            cost += params.adjacency_penalty;
        }
        if (nb_biome == static_cast<int>(b)) {
            cost -= params.contiguity_bonus;
        }
    }
    return cost;
}

int same_neighbours(int cell, const CellGraph& g, const std::vector<int>& biome) {
    const int b = biome[cell];
    int n = 0;
    for (int k = g.offset[cell]; k < g.offset[cell + 1]; ++k) {
        if (biome[g.list[k]] == b) ++n;
    }
    return n;
}

bool adjacent(int a, int b, const CellGraph& g) {
    for (int k = g.offset[a]; k < g.offset[a + 1]; ++k) {
        if (g.list[k] == b) return true;
    }
    return false;
}

// How many of this cell's borders are forbidden by its current biome?
int forbidden_count(int cell, const CellGraph& g, const std::vector<int>& biome,
                    const ArchetypeTable& t) {
    const int b = biome[cell];
    if (b < 0) return 0;
    int n = 0;
    for (int k = g.offset[cell]; k < g.offset[cell + 1]; ++k) {
        const int nb = biome[g.list[k]];
        if (nb < 0) continue;
        if ((t[b].adjacency_mask >> nb) & 1u) ++n;
    }
    return n;
}

// Grow contiguous patches at constant quota by swapping biome labels between
// pairs of cells. A swap moves cell i to a neighbouring biome -- which is what
// grows the patch -- and pays for it by moving some cell j of that biome back,
// so neither biome's cell count changes. Only the area difference between i
// and j drifts, and that is held inside a budget.
//
// Rejected outright: a swap that puts either cell outside its new danger band,
// that creates a forbidden border, or that would push a biome's area drift past
// the budget. Accepted when the contiguity gained outweighs the danger
// displacement, so the radial ordering survives the rearrangement.
void repair_patches(const std::vector<CellRecord>& cells,
                    const CellGraph& graph,
                    const ArchetypeTable& table,
                    const AssignParams& params,
                    std::vector<int>& biome,
                    AssignStats& stats) {
    const int    n  = static_cast<int>(cells.size());
    const size_t nb = table.size();

    double total_land = 0.0;
    int    land_n = 0;
    for (int i = 0; i < n; ++i) {
        if (!cells[i].is_land) continue;
        total_land += cells[i].land_area;
        ++land_n;
    }
    if (land_n == 0) return;
    const double budget = static_cast<double>(params.repair_quota_budget) * (total_land / land_n);

    std::vector<double> drift(nb, 0.0);
    std::vector<std::vector<int> > members(nb);

    for (int pass = 0; pass < params.repair_passes; ++pass) {
        for (size_t b = 0; b < nb; ++b) members[b].clear();
        for (int i = 0; i < n; ++i) {
            if (biome[i] >= 0) members[biome[i]].push_back(i);
        }

        int swapped = 0;
        for (int i = 0; i < n; ++i) {
            const int bi = biome[i];
            if (bi < 0) continue;

            for (int k = graph.offset[i]; k < graph.offset[i + 1]; ++k) {
                const int bp = biome[graph.list[k]];
                if (bp < 0 || bp == bi) continue;

                // consider each neighbouring biome once
                bool seen = false;
                for (int k2 = graph.offset[i]; k2 < k; ++k2) {
                    if (biome[graph.list[k2]] == bp) { seen = true; break; }
                }
                if (seen) continue;

                if (cells[i].danger < table[bp].danger_band_min ||
                    cells[i].danger > table[bp].danger_band_max) continue;

                int   best_j = -1;
                float best_gain = 0.0f;

                for (size_t m = 0; m < members[bp].size(); ++m) {
                    const int j = members[bp][m];
                    if (j == i || biome[j] != bp) continue;   // membership may be stale
                    if (cells[j].danger < table[bi].danger_band_min ||
                        cells[j].danger > table[bi].danger_band_max) continue;

                    const double ai = cells[i].land_area, aj = cells[j].land_area;
                    if (std::fabs(drift[bi] - ai + aj) > budget) continue;
                    if (std::fabs(drift[bp] - aj + ai) > budget) continue;

                    const int before = same_neighbours(i, graph, biome) +
                                       same_neighbours(j, graph, biome);
                    const int viol_before = forbidden_count(i, graph, biome, table) +
                                            forbidden_count(j, graph, biome, table);
                    biome[i] = bp; biome[j] = bi;
                    int after = same_neighbours(i, graph, biome) +
                                same_neighbours(j, graph, biome);
                    if (adjacent(i, j, graph) && biome[i] == biome[j]) after -= 1;
                    const int viol_after = forbidden_count(i, graph, biome, table) +
                                           forbidden_count(j, graph, biome, table);
                    biome[i] = bi; biome[j] = bp;
                    if (viol_after > viol_before) continue;   // never make it worse

                    const float dd =
                        std::fabs(cells[i].danger - table[bp].danger_preferred) +
                        std::fabs(cells[j].danger - table[bi].danger_preferred) -
                        std::fabs(cells[i].danger - table[bi].danger_preferred) -
                        std::fabs(cells[j].danger - table[bp].danger_preferred);

                    const float gain = static_cast<float>(after - before) -
                                       params.repair_danger_weight * dd +
                                       params.repair_adjacency_weight *
                                           static_cast<float>(viol_before - viol_after);
                    if (gain > best_gain) { best_gain = gain; best_j = j; }
                }

                if (best_j >= 0) {
                    drift[bi] += cells[best_j].land_area - cells[i].land_area;
                    drift[bp] += cells[i].land_area - cells[best_j].land_area;
                    biome[i] = bp;
                    biome[best_j] = bi;
                    ++swapped;
                    ++stats.swaps;
                    break;
                }
            }
        }
        if (swapped == 0) break;
    }
}

}  // namespace

void assign_biomes(const std::vector<CellRecord>& cells,
                   const CellGraph& graph,
                   const ArchetypeTable& table,
                   const AssignParams& params,
                   std::vector<int>& out_biome,
                   AssignStats& stats) {
    const size_t n = cells.size();
    const size_t nb_biomes = table.size();

    out_biome.assign(n, -1);
    stats = AssignStats();

    // land_cells = cells where the layer-0 height clears sea level.
    std::vector<int> land;
    land.reserve(n);
    double total_land_area = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if (!cells[i].is_land) continue;
        land.push_back(static_cast<int>(i));
        total_land_area += cells[i].land_area;
    }
    stats.land_cells = static_cast<int>(land.size());
    if (land.empty()) return;

    // quota[b] = b.area_fraction * total_land_area.
    // Quotas are fractions of LAND area, never world area -- mixing them means
    // a seed with more coastline silently shrinks every biome.
    std::vector<double> quota(nb_biomes, 0.0);
    std::vector<double> target(nb_biomes, 0.0);
    for (size_t b = 0; b < nb_biomes; ++b) {
        target[b] = static_cast<double>(table[b].area_fraction) * total_land_area;
        quota[b]  = target[b];
    }

    // Sorting by danger before assigning is what produces the radial ordering.
    // Ties break on index so the result is deterministic regardless of the
    // sort implementation.
    std::sort(land.begin(), land.end(), [&cells](int a, int b) {
        if (cells[a].danger != cells[b].danger) return cells[a].danger < cells[b].danger;
        return a < b;
    });

    long long total_candidates = 0;

    for (size_t idx = 0; idx < land.size(); ++idx) {
        const int cell = land[idx];
        const CellRecord& rec = cells[cell];

        int   best = -1;
        float best_cost = std::numeric_limits<float>::infinity();

        // How much quota a biome must still hold to stay a candidate.
        double floor_area = 0.0;
        if (params.quota_mode == QUOTA_HALF_CELL) {
            floor_area = 0.5 * static_cast<double>(rec.land_area);
        }

        // Pass 1: in band, quota remaining.
        int candidates = 0;
        for (size_t b = 0; b < nb_biomes; ++b) {
            if (rec.danger < table[b].danger_band_min || rec.danger > table[b].danger_band_max) continue;
            if (params.quota_mode != QUOTA_SOFT_FILL && quota[b] <= floor_area) continue;
            ++candidates;
            float c = placement_cost(b, cell, rec, graph, table, params, out_biome);
            if (params.quota_mode == QUOTA_SOFT_FILL && target[b] > 0.0) {
                // Being ahead of schedule costs. A biome at 0% fill is always
                // cheapest, which is what keeps presence guaranteed.
                const double fill = 1.0 - quota[b] / target[b];
                c += params.fill_weight * static_cast<float>(fill);
            }
            if (c < best_cost) { best_cost = c; best = static_cast<int>(b); }
        }
        total_candidates += candidates;

        // Pass 2 -- relaxation: quota remaining, band ignored. This is what
        // guarantees presence on a pathological seed, at the cost of radial
        // ordering for that cell. Counted, because a high rate means the bands
        // are wrong rather than the seed being unusual.
        if (best < 0) {
            for (size_t b = 0; b < nb_biomes; ++b) {
                if (quota[b] <= floor_area) continue;
                const float c = placement_cost(b, cell, rec, graph, table, params, out_biome);
                if (c < best_cost) { best_cost = c; best = static_cast<int>(b); }
            }
            if (best >= 0) ++stats.relaxed_cells;
        }

        // Pass 3 -- overflow: every quota is spent. Only reachable on the last
        // cells, since the quotas sum to exactly the land area and each cell
        // can overshoot its biome by at most one cell.
        if (best < 0) {
            for (size_t b = 0; b < nb_biomes; ++b) {
                const float c = placement_cost(b, cell, rec, graph, table, params, out_biome);
                if (c < best_cost) { best_cost = c; best = static_cast<int>(b); }
            }
            ++stats.overflow_cells;
        }

        out_biome[cell] = best;
        quota[best] -= static_cast<double>(rec.land_area);
    }

    if (!land.empty()) {
        stats.mean_candidates = static_cast<float>(static_cast<double>(total_candidates) / land.size());
    }

    if (params.repair_passes > 0) {
        repair_patches(cells, graph, table, params, out_biome, stats);
    }
}

}  // namespace worldgen
