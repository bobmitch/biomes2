#pragma once
// ---------------------------------------------------------------------------
// The ray-tracing scene (ADR-004 L5): what gi_trace and gi_occluded trace
// against on DXR 1.1.
//
//   terrain  one BLAS per drawn chunk at LOD 0-3 (+-512 m, the cascades'
//            reach), built from the chunk's own slot in the mesh pool: the
//            BLAS points at the whole vertex pool and the slot's index range,
//            because the pool's indices are absolute. DXR 1.1 has no
//            indirect build, so a chunk's index count must be on the CPU:
//            it is read back two frames after the chunk is meshed, then the
//            build is queued, at most kBuildsPerFrame a frame.
//   trees    one BLAS per model from its LOD 1 mesh, built once: bark and
//            caps opaque, leaves and needles NON-opaque -- a shadow ray
//            passing a card keeps a share of its light, so a crown is a
//            transmittance, as the ADR asks, without proxy hulls.
//   crowns   (ADR-004 L6) one shared procedural BLAS, a unit-sphere AABB,
//            instanced per standing tree as an ellipsoid round its crown
//            with the species' leaf area density: a volume the sky rays
//            take Beer-Lambert through, smooth where the cards are binary.
//            Mask kMaskCrowns, so the key-light shadow never sees them.
//   TLAS     rebuilt every frame from the drawn chunks and the standing
//            trees within range. InstanceID is the slot for terrain, and
//            kTreeFlag | model for a tree, which is how gi_trace finds a
//            hit's normal and albedo.
//
// Nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "engine/streaming/streamer.h"
#include "engine/trees/tree_field.h"
#include "engine/trees/tree_mesh.h"

namespace engine {

class ChunkMesher;

class RtScene {
public:
    static const UINT kBuildsPerFrame = 32;
    static const UINT kMaxTreeInstances = 65536;
    static const UINT kTreeFlag = 0x800000u;
    static const UINT kCrownFlag = 0x400000u;   // InstanceID of a crown: kCrownFlag | model
    static const UINT kMaskTerrain = 0x1u, kMaskTrees = 0x2u, kMaskCrowns = 0x4u;
    static const int  kMaxLod = 3;

    bool init(ID3D12Device* device, std::string& error);
    bool ready() const { return ready_; }

    // Chunks meshed this frame: their BLAS is due once their counts land,
    // two recorded frames on -- or now, for chunks meshed long ago
    // (`landed`), as when the scene is switched on over a meshed world.
    // Frames are counted by record() itself, not the renderer's frame index:
    // a still's warm-up records frames without advancing that.
    void note_meshed(const std::vector<Streamer::MeshedChunk>& meshed, bool landed = false);
    // Bisection switches (BIOMES_RT_DBG): 2 no terrain BLAS, 4 no trees in the TLAS.
    unsigned debug_ = 0;
    // Crown volumes in the TLAS (L6). Off unless something traces them: they
    // double the tree instances.
    bool crowns_ = false;

    // Builds what is due and the TLAS, on `cmd`, after the frame's meshing
    // and before anything traces. The pool's vertices and indices must be in
    // their draw states (the mesher's record_to_draw).
    void record(ID3D12GraphicsCommandList* cmd, UINT frame,
                const Streamer& streamer, ChunkMesher& mesher,
                const std::vector<TreeModel>& models, const std::vector<TreeLook>& looks,
                const std::vector<TreeGpuInstance>& trees, const float cam[3], float tree_range);

    D3D12_GPU_VIRTUAL_ADDRESS tlas() const;
    ID3D12Resource* tree_positions() const { return tree_pos_.Get(); }
    ID3D12Resource* tree_indices() const { return tree_idx_.Get(); }
    ID3D12Resource* tree_info() const { return tree_info_.Get(); }
    ID3D12Resource* tree_attributes() const { return tree_attr_.Get(); }

    struct Stats {
        UINT terrain_blas = 0, tree_blas = 0;
        UINT64 blas_bytes = 0, tlas_bytes = 0, scratch_bytes = 0;
        UINT built_this_frame = 0, pending = 0;
        UINT instances_terrain = 0, instances_trees = 0, instances_crowns = 0;
        UINT drawn_without_blas = 0;     // drawn LOD 0-3 chunks not (yet) in the TLAS
        // Where queued chunks went, since start: the queue's own accounts.
        UINT64 noted = 0, dropped_remeshed = 0, dropped_moved = 0, empty = 0, built_total = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct SlotBlas {
        ChunkKey key = {0, 0, 0, -1};     // what the BLAS (or the empty verdict) is for
        ComPtr<ID3D12Resource> blas;
        UINT64 bytes = 0;
        unsigned long long seen = 0;
        ChunkKey queued = {0, 0, 0, -1};  // queued for this key, not built yet
    };
    struct Pending { ChunkKey key; UINT slot; unsigned long long frame; };

    bool build_trees(ID3D12GraphicsCommandList4* cl, const std::vector<TreeModel>& models,
                     const std::vector<TreeLook>& looks);
    bool make_buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav,
                     ComPtr<ID3D12Resource>& out);

    ComPtr<ID3D12Device5> dev_;
    bool ready_ = false, trees_built_ = false;
    unsigned long long tick_ = 3;   // recorded frames; starts at 3 so "landed" can be tick - 3
    std::vector<SlotBlas> slots_;
    std::deque<Pending> pending_;
    std::vector<unsigned long long> latest_mesh_;   // per slot: the newest meshing frame
    std::vector<std::pair<unsigned long long, ComPtr<ID3D12Resource> > > graveyard_;
    ComPtr<ID3D12Resource> scratch_, tlas_[2], tlas_scratch_, inst_up_[2];
    void* inst_cpu_[2] = {};
    UINT64 scratch_size_ = 0, tlas_size_ = 0;
    UINT max_instances_ = 0;
    UINT tlas_cur_ = 0;
    // Trees: positions (float3), indices, and per-model info for gi_trace.
    ComPtr<ID3D12Resource> tree_pos_, tree_idx_, tree_info_, tree_attr_;
    std::vector<ComPtr<ID3D12Resource> > tree_blas_;
    // Crowns: the unit AABB, its BLAS, and per model the ellipsoid in model
    // space (centre height, horizontal and vertical radii); radius 0 = none.
    ComPtr<ID3D12Resource> crown_aabb_, crown_blas_;
    struct Crown { float cy, rh, rv; };
    std::vector<Crown> crowns_by_model_;
    Stats stats_;
};

}  // namespace engine
