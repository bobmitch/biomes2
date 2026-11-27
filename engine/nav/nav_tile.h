// ---------------------------------------------------------------------------
// One navmesh tile from the field (ADR-007, decision 4). Recast normally
// rasterises triangles into a heightfield of SOLID SPANS per column; the
// field gives those spans directly. Per 0.5 m column -- the edit lattice, so
// a dig lands exactly -- the density is sampled at the lattice points down a
// band round the 2.5D height (padded by layer 6's overhang reach, and down to
// any placed cave under the column, and over any edited brick), and every
// run of rock becomes a span, its ends where the density crosses zero
// (linear between samples). Trunks, rocks, stumps and resting logs are
// stamped in as solid spans of their own. Then Recast's ordinary pipeline:
// ledge and headroom filters, erosion by the agent's radius, regions,
// contours, polygons, the detail mesh, and Detour's tile data.
//
// WATER is a half-space at sea level, so a floor below it is under water. A
// floor within `wade_depth` of the surface is area WADE; deeper, the floor
// is not walkable at all, and a thin span at sea level - 0.5 m stands for the
// surface, area SWIM -- 0.5 m down so the deepest wading floor (sea - 1 m) is
// one climb from it and the two connect.
//
// Pure: no GPU, no engine state. The edits arrive as a copy of the bricks in
// the tile's box, so a worker builds while the main thread digs.
// ---------------------------------------------------------------------------
#ifndef ENGINE_NAV_NAV_TILE_H
#define ENGINE_NAV_NAV_TILE_H

#include "engine/svdag/brick_store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace worldgen {
struct SeedWorld;
struct WorldParams;
struct ArchetypeTable;
}

namespace engine {
namespace nav {

// Poly areas and the flags a query filter includes.
enum Area : unsigned char { kAreaGround = 1, kAreaWade = 2, kAreaSwim = 3 };
enum Flag : unsigned short { kFlagWalk = 1, kFlagWade = 2, kFlagSwim = 4 };

struct Agent {
    float radius = 0.4f, height = 1.8f, climb = 0.5f;   // metres; climb over a 0.5 m cell is 45 degrees
};

// A solid shape stamped into the tile: a vertical cylinder (a trunk, a
// stump, a rock) from a[1] to b[1] round (a[0], a[2]), or a capsule from a
// to b (a log).
struct Obstacle {
    bool capsule = false;
    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    float r = 0.0f;
};

struct TileInput {
    int tx = 0, tz = 0;          // the zone: [tx, tx + 1) x size in x, likewise z
    float size = 64.0f;
    int tile_x = 0, tile_y = 0;  // Detour's tile coordinates
    Agent agent;
    float sea_level = 0.0f;
    float wade_depth = 1.0f;
    std::vector<Obstacle> obstacles;
    // The edits in the box: copies of the bricks, so the worker reads them
    // while the main thread digs.
    std::vector<std::pair<BrickKey, std::shared_ptr<Brick> > > bricks;
};

struct TileOutput {
    unsigned char* data = nullptr;   // Detour tile data (dtAlloc'd; the nav mesh frees it)
    int size = 0;
    int polys = 0;
    int columns = 0;                 // columns sampled
    long long samples = 0;           // field evaluations
    double ms = 0.0;
};

// False with `error` on a failure; true with data == nullptr when the tile
// has nothing walkable (the open sea far out, a cliff).
bool build_tile(const worldgen::SeedWorld& w, const worldgen::WorldParams& wp, const worldgen::ArchetypeTable& t,
                const TileInput& in, TileOutput& out, std::string& error);

}  // namespace nav
}  // namespace engine

#endif
