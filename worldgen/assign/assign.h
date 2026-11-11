// Layer 3 -- quota assignment. Runs once per world on the CPU and uploads a
// small buffer. This is the layer that makes "every biome, every seed, in a
// known proportion" true by construction.
//
// Portable C++, no host dependencies (CLAUDE.md invariant 1). It takes cells
// and their danger values as input and knows nothing about how either was
// produced -- no noise, no field function, no graphics.
#pragma once
#include <vector>
#include <cstdint>
#include "archetypes.h"

namespace worldgen {

struct CellRecord {
    float cx = 0.0f, cz = 0.0f;  // centroid in world metres, centred on origin
    float land_area = 0.0f;      // m^2 of LAND in this cell -- not total cell area
    float total_area = 0.0f;     // m^2 of disc covered by this cell
    float danger = 0.0f;         // D at the centroid, [0,1]
    float norm_area = 0.0f;      // (r/R)^2 at the centroid; diagnostics only
    bool  is_land = false;
};

// Undirected neighbour graph over cells, CSR form.
struct CellGraph {
    std::vector<int> offset;  // size cells+1
    std::vector<int> list;
};

// How a biome stops accepting cells once it approaches its quota.
enum QuotaMode {
    // The original specification: a biome stays a candidate while any quota
    // remains, so it can overshoot by almost a whole cell. Retained only to
    // reproduce the measurement -- it misses the +/-10% quota requirement on
    // 200 seeds out of 200.
    QUOTA_DOC = 0,
    // The default. Stop accepting once less than half a cell of quota
    // remains, so the rounding error is symmetric rather than always
    // positive and stops accumulating onto the last biome in sort order.
    QUOTA_HALF_CELL = 1,
    // No hard gate: being ahead of schedule is a cost rather than a bar.
    // Measured at 83% mean quota error -- the hard gate is what *makes* the
    // guarantee and cannot be relaxed into a preference. Kept as a signpost.
    QUOTA_SOFT_FILL = 2
};

struct AssignParams {
    // Cost subtracted per already-assigned neighbour sharing the candidate
    // biome. Clumps cells into patches without touching quota granularity,
    // because the quota gate below is hard.
    float contiguity_bonus = 0.10f;
    // Cost added when an assigned neighbour forbids the candidate. A cost, not
    // a constraint, so the solver can never deadlock.
    float adjacency_penalty = 10.0f;

    int   quota_mode  = QUOTA_HALF_CELL;
    float fill_weight = 2.0f;   // QUOTA_SOFT_FILL only
};

struct AssignStats {
    int   land_cells     = 0;
    int   relaxed_cells  = 0;  // no in-band candidate had quota left
    int   overflow_cells = 0;  // every quota exhausted; band ignored too
    // Mean size of the candidate set in pass 1. If this is ~1 the cost
    // function is inert and quota exhaustion is deciding everything.
    float mean_candidates = 0.0f;
};

// out_biome is sized to cells.size(); ocean cells get -1.
void assign_biomes(const std::vector<CellRecord>& cells,
                   const CellGraph& graph,
                   const ArchetypeTable& table,
                   const AssignParams& params,
                   std::vector<int>& out_biome,
                   AssignStats& stats);

}  // namespace worldgen
