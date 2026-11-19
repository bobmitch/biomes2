// ---------------------------------------------------------------------------
// Opaque cutouts of the leaf cards, for the ray-traced key-light shadow.
//
// The RT scene's leaf cards are non-opaque: every card a shadow ray meets
// returns to the shader, which evaluates leaf.hlsli's leaf_coverage at the
// hit and commits it or not. That was 55-60% of the RT shadow pass in a wood
// (BIOMES_RT_LEAVES=1 and 2 measured it), more of it the round trips than the
// pattern. Here the pattern is cut out of each LOD 1 card ONCE, as opaque
// triangles, and shadow rays traverse them in hardware with no shader call.
//
// The cut is the RT alpha test's own: leaf_coverage at blur 0.1, kept where
// it reaches 0.5. Broad leaves become one polygon per leaf, its outline found
// by marching and bisecting the leaf's own coverage along rays from its
// centre (each leaf is star-shaped about it, and clipped to its cell as the
// shader clips it). Needles become marching squares in a sheared frame per
// half card, (|u - 0.5|, v*9 + |u - 0.5|*6), where every stroke is an
// axis-aligned band, with edge crossings bisected rather than interpolated.
// ---------------------------------------------------------------------------
#ifndef ENGINE_LEAF_CUTOUT_H
#define ENGINE_LEAF_CUTOUT_H

#include "engine/trees/tree_mesh.h"

#include <cstdint>
#include <vector>

namespace engine {

struct LeafCutoutStats {
    uint32_t cards = 0;
    uint32_t leaves = 0;      // broad-leaf polygons
    uint32_t triangles = 0;
    uint32_t needle_tris = 0; // of which needle cards'
};

// Appends the cutouts of every leaf and needle card in `mesh` to `tri_pos`,
// three floats a vertex and three vertices a triangle (no index buffer), in
// the mesh's own model metres. `narrow`: the species' willow leaves.
void leaf_cutouts(const TreeMesh& mesh, bool narrow, std::vector<float>& tri_pos, LeafCutoutStats& stats);

// leaf.hlsli's leaf_coverage, ported. KEEP THE TWO EQUAL: a change to the
// pattern there that is not made here leaves the traced shadows cut from the
// old leaves.
float leaf_coverage_cpu(float u, float v, float kind, float seed, float blur, float narrow);

}  // namespace engine

#endif  // ENGINE_LEAF_CUTOUT_H
