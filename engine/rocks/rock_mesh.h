// ---------------------------------------------------------------------------
// Rock meshes (layer 9's boulders and outcrops), generated here at startup.
//
// Every model is an icosphere pushed into shape: boulders cut by a handful of
// random planes into chunky facets, flattened, with a little noise; outcrops
// elongated and banded into ledges by a sawtooth of strata, so they read as
// bedrock breaking the surface. Unit size -- one metre across -- with the
// origin at the centre of the base, the way worldgen::RockInstance places
// it; the instance scales by its size. Two levels per model. Presentation:
// sin and noise are fine here, nothing reaches a hash.
//
// Indices are ABSOLUTE into the one vertex array (draws pass base vertex 0):
// SV_VertexID does not include BaseVertexLocation on this driver (CLAUDE.md).
// ---------------------------------------------------------------------------
#ifndef ENGINE_ROCK_MESH_H
#define ENGINE_ROCK_MESH_H

#include <cstdint>
#include <vector>

namespace engine {

struct RockVertex {
    float pos[3];
    float nrm[3];
};

struct RockModel {
    uint32_t first[2], count[2];   // index range per level: 0 near, 1 far
    uint32_t vfirst[2], vcount[2]; // vertex range per level (collision reads level 1)
    float    height;               // top of the mesh above its base, unit size
    float    radius;               // horizontal reach from the axis, unit size
};

static const uint32_t kBoulderModels = 8;
static const uint32_t kOutcropModels = 4;

struct RockMeshes {
    std::vector<RockVertex> v;
    std::vector<uint32_t>   idx;
    std::vector<RockModel>  models;   // boulders, then outcrops
};

void build_rock_meshes(RockMeshes& out);

// Which model a rock is drawn with.
inline uint32_t rock_model(int kind, int variant) {
    return kind == 0 ? uint32_t(variant) % kBoulderModels : kBoulderModels + uint32_t(variant) % kOutcropModels;
}

}  // namespace engine

#endif  // ENGINE_ROCK_MESH_H
