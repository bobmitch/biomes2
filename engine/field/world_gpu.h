// ---------------------------------------------------------------------------
// M1: one seed's world, on the GPU.
//
// The field function itself is shared source (ADR-001) and compiles as HLSL
// unmodified. What does NOT cross is everything around it: which cells are
// near a point, which river segments, which entrance shafts. Those are
// spatial-index questions, the ADR puts them on the caller's side of the
// line, and this is the caller's side for the GPU -- the same job
// worldgen/world.cpp does for the CPU, against the same buffers.
//
// Everything here is static for a seed. It is built once by build_world() and
// uploaded once; nothing in the frame loop touches it.
// ---------------------------------------------------------------------------
#ifndef ENGINE_WORLD_GPU_H
#define ENGINE_WORLD_GPU_H

#include "worldgen/world.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Mirrors the cbuffer in the kernel. Scalars only: the buffers carry
// everything variable-length.
struct WorldConstants {
    float radius;
    float continent_freq;
    int   continent_octaves;
    float centre_floor;

    float edge_drop;
    float sea_level;
    float land_offset;
    float danger_w;

    float danger_freq;
    int   danger_octaves;
    float warp_amp;
    float warp_freq;

    float continent_scale;
    float biome_freq;
    float blend_width;
    float overhang_freq;

    float overhang_scale;
    float cave_freq;
    float cave_carve;
    float entrance_falloff;

    unsigned s_cont, s_dang, s_wx, s_wz;
    unsigned s_height, s_overhang, s_cave, s_entrance;

    // Spatial indices. The grids are uniform, so an origin and a cell size is
    // the whole index.
    int   cell_buckets;      // B, so B*B buckets
    float cell_bucket_size;
    int   river_grid;        // N, so N*N bins
    float river_bin_size;

    int   entrance_grid;
    float entrance_bin_size;
    int   cell_count;
    int   biome_count;
};

class WorldGpu {
public:
    // Builds the seed on the CPU and uploads it. The CPU copy is kept: the
    // tolerance check compares against it, and physics has no other source.
    bool build(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
               uint64_t seed, std::string& error);
    // Upload buffers must outlive the command list's execution.
    void release_staging();

    const worldgen::SeedWorld&     world() const { return world_; }
    const worldgen::WorldParams&   params() const { return params_; }
    const worldgen::ArchetypeTable& table() const { return table_; }
    const WorldConstants&          constants() const { return constants_; }

    // What a save file is anchored to. Invariant 5 stores edits as ABSOLUTE
    // density, so a save only means anything against the base terrain it was
    // cut from -- which is the generator version AND the seed, because a
    // different seed is as different a world as a different generator. B7
    // wrote a literal zero here because the trivial field had no version to
    // name.
    uint32_t field_id() const { return field_id_; }

    // The same field the kernel evaluates, on this side of the line. Every
    // CPU-side check in the engine goes through these two so there is one
    // place that knows how the generator is called.
    float height(float x, float z, float voxel = 0.0f) const {
        const float r = std::sqrt(x * x + z * z) / params_.radius;
        return worldgen::terrain_height(world_, params_, table_, x, z, r, voxel);
    }
    float density(float x, float y, float z, float voxel = 0.0f) const {
        return worldgen::terrain_density(world_, params_, table_, x, y, z, voxel);
    }

    // The band of heights any surface can occupy, for chunk residency. The
    // trivial field answered this with two literals; the real one has to be
    // derived from the continent scale and the biome amplitudes, and M1 is
    // where that requirement stops being hypothetical.
    void band(float& lo, float& hi) const;

    // The band for ONE COLUMN, which is the version that matters. A global
    // band over the real world spans about 700 m, so residency considers 44
    // chunks per column at LOD 0 and the slot pool fills with empty air
    // before it ever reaches the ground -- measured at M1 as 768 chunks
    // resident and zero triangles. Evaluating layer 0 over the column costs
    // one fBm and bounds the surface to within the biome amplitudes.
    void column_band(float x0, float z0, float x1, float z1,
                     float& lo, float& hi) const;

    ID3D12Resource* cells() const { return cells_.Get(); }
    ID3D12Resource* cell_offsets() const { return cell_offsets_.Get(); }
    ID3D12Resource* cell_items() const { return cell_items_.Get(); }
    ID3D12Resource* archetypes() const { return archetypes_.Get(); }
    ID3D12Resource* rivers() const { return rivers_.Get(); }
    ID3D12Resource* river_offsets() const { return river_offsets_.Get(); }
    ID3D12Resource* river_items() const { return river_items_.Get(); }
    ID3D12Resource* entrances() const { return entrances_.Get(); }
    ID3D12Resource* entrance_offsets() const { return entrance_offsets_.Get(); }
    ID3D12Resource* entrance_items() const { return entrance_items_.Get(); }
    ID3D12Resource* world_cb() const { return world_cb_.Get(); }

private:
    worldgen::SeedWorld      world_;
    worldgen::WorldParams    params_;
    worldgen::ArchetypeTable table_;
    WorldConstants           constants_;
    uint32_t                 field_id_ = 0;

    ComPtr<ID3D12Resource> cells_, cell_offsets_, cell_items_, archetypes_;
    ComPtr<ID3D12Resource> rivers_, river_offsets_, river_items_;
    ComPtr<ID3D12Resource> entrances_, entrance_offsets_, entrance_items_;
    ComPtr<ID3D12Resource> world_cb_;
    std::vector<ComPtr<ID3D12Resource> > staging_;
};

}  // namespace engine

#endif  // ENGINE_WORLD_GPU_H
