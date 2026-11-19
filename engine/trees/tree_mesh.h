// ---------------------------------------------------------------------------
// Procedural tree meshes. Made here, at startup, from the species table in
// worldgen/data/archetypes.json: placement reads a species' height and
// altitude band, and this reads its "form" and "shape" -- presentation only,
// on the far side of the golden hash, so libm is allowed.
//
// A mesh is built in metres at the species' typical height; an instance
// scales it by height / nominal. Two levels per variant: LOD 0 has every
// branch level and small leaf cards, LOD 1 drops the twigs and uses fewer,
// larger cards. Beyond LOD 1 a tree is an octahedral impostor baked from LOD 0
// (tree_renderer.cpp).
//
// Every mesh carries a ring at kCutHeight with two cap discs, one facing up
// and one down. A standing tree discards both; a stump draws the trunk below
// the cut and the upper cap; a felled log draws everything above it and the
// lower cap. One mesh, three uses, chosen per instance in the shader.
// ---------------------------------------------------------------------------
#ifndef ENGINE_TREE_MESH_H
#define ENGINE_TREE_MESH_H

#include "worldgen/assign/archetypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

enum class TreeForm : uint32_t { Broadleaf = 0, Weeping, Conifer, Pine, Shrub, Snag };

// Vertex kinds, read by the shader.
enum TreeVertexKind : uint32_t {
    kKindBark = 0, kKindLeaf = 1, kKindNeedle = 2, kKindCapUp = 3, kKindCapDown = 4
};

// Metres above the trunk base where a tree is felled.
static const float kCutHeight = 0.55f;

struct TreeLook {
    TreeForm form = TreeForm::Broadleaf;
    float bark[3] = {0.3f, 0.25f, 0.2f};
    float leaf[3] = {0.2f, 0.3f, 0.1f};
    float crown_start = 0.3f;     // fraction of height where the crown begins
    float crown_radius = 0.35f;   // fraction of height
    float trunk = 0.025f;         // base radius as a fraction of height
    float leaf_size = 1.0f;       // card edge, metres
    int   limbs = 6;
    float leaf_density = 0.5f;    // leaf area density in the crown, m^2/m^3: the RT crown volume's extinction
    float nominal_height = 15.0f; // metres the mesh is built at
    float bend = 1.0f;            // how far the wind bends it: a birch more, a spruce less (tree_sway.h)
    // A CC0 bark scan (assets/textures/scans.json), retinted to `bark`;
    // empty keeps the procedural bark. The renderer fills the rest from the
    // scan library before the trees initialise.
    std::string bark_scan;
    float bark_layer = -1.0f, bark_inv_tile = 1.0f, bark_tint[3] = {1.0f, 1.0f, 1.0f}, bark_depth = 0.0f;
};

// A model's crown as an ellipsoid about the trunk axis, in model metres:
// centre height, horizontal and vertical radii, and the extinction through
// it (1/m). From where the crown starts to the top, 0.9 of the leaves'
// reach across. rh == 0: no crown (a snag). The RT scene's crown volumes
// and the canopy map (ADR-004 L6) both read it.
struct CrownVolume { float cy = 0.0f, rh = 0.0f, rv = 0.0f, sigma = 0.0f; };
CrownVolume crown_volume(const struct TreeModel& model, const TreeLook& look);

// One per species, in TreeTable order. Throws nothing; fills `error`.
bool load_tree_looks(const worldgen::ArchetypeTable& table, const char* json_path,
                     std::vector<TreeLook>& out, std::string& error);

struct TreeVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float sway;    // 0 rigid .. 1 a free twig tip
    float ao;      // baked occlusion, 0 dark .. 1 open sky
    float kind;    // TreeVertexKind
    float seed;    // per card, [0, 1): the leaf pattern's own randomness
};
static_assert(sizeof(TreeVertex) == 48, "TreeVertex is read as a StructuredBuffer");

struct TreeMesh {
    std::vector<TreeVertex> v;
    std::vector<uint32_t>   idx;
};

// What one species variant looks like at both levels, plus what culling,
// impostors and physics need to know about its extent. Metres at nominal.
struct TreeModel {
    TreeMesh lod[2];
    float radius = 1.0f;          // horizontal extent from the trunk axis
    float top = 1.0f;             // highest point
    float trunk_radius = 0.2f;    // at the base
    float crown_base = 3.0f;      // where the crown begins
    float crown_centre = 8.0f;    // centre of the crown's volume, for physics
    float crown_r = 3.0f;         // radius of a sphere standing for the crown
};

TreeModel build_tree_model(const TreeLook& look, uint32_t species, uint32_t variant);

}  // namespace engine

#endif  // ENGINE_TREE_MESH_H
