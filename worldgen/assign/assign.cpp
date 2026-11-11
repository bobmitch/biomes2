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
}

}  // namespace worldgen
