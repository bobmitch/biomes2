// ---------------------------------------------------------------------------
// GPU surface nets into a shared pool.
//
// B2 gave one chunk its own buffers. That does not scale: a resident world is
// hundreds of chunks, and sizing each one for its worst case would spend
// gigabytes on mostly-empty space. Instead there is one vertex buffer and one
// index buffer divided into fixed slots, and a chunk owns a slot while it is
// resident.
//
// Fixed slots rather than a suballocator, on purpose. Chunks come and go
// constantly as the camera moves and a variable-size allocator would fragment;
// a slot is either free or it is not. The cost is the headroom in each slot,
// and the overflow count reports when that headroom was not enough.
//
// Indices are stored SLOT-RELATIVE and the draw's BaseVertexLocation rebases
// them, so a chunk can be meshed into any slot without rewriting its indices.
//
// The counts never come back to the CPU on the draw path. One indirect
// argument entry per slot means the entire resident world is a single
// ExecuteIndirect, and adding a chunk never adds a stall.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CHUNK_MESHER_H
#define ENGINE_CHUNK_MESHER_H

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace engine {

using Microsoft::WRL::ComPtr;

// A chunk owns 32 cells, so chunk origins are 32 voxels apart at every level.
// It meshes 33: one ghost layer on the high side, so the quad joining it to
// its neighbour exists. See the note in surface_nets.hlsl -- without it every
// chunk boundary is a one-cell crack, which at LOD 4 is eight metres of sky.
//
// The extra layer costs 34^3 = 39,304 field samples per chunk rather than the
// 33^3 = 35,937 the design document budgets. 33^3 is exactly the corners of 32
// cells with nothing spare, so the apron the doc mentions was never actually
// paid for.
static const UINT kChunkOwnedCells = 32;
static const UINT kChunkCells      = 33;
static const UINT kChunkCorners    = 34;

// A dense LOD-0 terrain chunk measured 1797 vertices and 10,056 indices at B2.
// A slot is sized at roughly 1.7x that, not at the theoretical worst case of
// one vertex per cell (32,768), which no surface reaches: a chunk holds a
// sheet through a volume, not a solid. Overflow is counted rather than assumed
// away.
static const UINT kSlots        = 768;
static const UINT kSlotVertices = 4096;
static const UINT kSlotIndices  = 16384;

struct MeshVertex {
    float pos[3];
    float nrm[3];
};

// One height sample per corner of the chunk, ghost layer included, so
// neighbouring shapes overlap by a cell instead of leaving a seam a character
// could fall through.
static const UINT kHeightGrid = kChunkCorners;          // 34
static const UINT kHeightSamples = kHeightGrid * kHeightGrid;

// Three staging buffers, the design's figure. Written in frame N, read at the
// start of frame N+2: with two frames in flight the CPU has already waited on
// frame N's fence by then, so the map never blocks.
static const UINT kReadbackRing = 3;
static const UINT kReadbackChunksPerFrame = 8;

// One chunk's heights, as handed to physics.
struct HeightPatch {
    UINT  slot;
    float origin[3];     // world position of sample (0,0)
    float voxel;         // metres between samples
    float h[kHeightSamples];
};

class ChunkMesher {
public:
    // timestamp_freq comes from the command queue the work will run on: ticks
    // are per-queue, so there is no device-wide answer to ask for.
    bool init(ID3D12Device* device, UINT64 timestamp_freq, std::string& error);
    bool reload_shaders(ID3D12Device* device, std::string& error);

    // Mesh one chunk into one slot. The scratch buffers are shared, so these
    // serialise against each other -- see the barriers in the implementation.
    void record_mesh(ID3D12GraphicsCommandList* cmd, UINT slot,
                     const float origin[3], float voxel);
    // Zero a slot's draw arguments. An all-zero indirect draw is skipped.
    void record_retire(ID3D12GraphicsCommandList* cmd, UINT slot);

    // The pool is written by compute and read by the draw every frame, so it
    // changes state twice a frame.
    void record_to_draw(ID3D12GraphicsCommandList* cmd);
    void record_to_compute(ID3D12GraphicsCommandList* cmd);

    // Copies the per-slot counters for the heads-up display. Read a frame or
    // two stale on purpose rather than waited on: a stall here would be a
    // stall in the very thing B3 is measuring.
    void record_counter_copy(ID3D12GraphicsCommandList* cmd);
    void read_counts(UINT& vertices, UINT& indices, UINT& slots_overflowed);
    UINT slot_vertex_count(UINT slot);
    // The indirect arguments as the kernel actually wrote them. Debug only.
    void read_draw_args(UINT first, UINT count, std::vector<UINT>& out);

    // The meshed vertices, on the CPU. For checking the mesh against the field
    // it came from -- not a path anything in the frame uses.
    void record_vertex_copy(ID3D12GraphicsCommandList* cmd);
    bool read_vertices(UINT slot, UINT count, std::vector<MeshVertex>& out);

    // Copy the heights of the chunks meshed this frame into one staging
    // buffer of the ring. `ring` is the frame's own slot.
    void record_height_copy(ID3D12GraphicsCommandList* cmd, UINT ring,
                            const UINT* slots, UINT count);
    // Read back what was staged `kReadbackRing - 1` frames ago. Never blocks.
    const float* mapped_heights(UINT ring, UINT count);
    void         unmap_heights(UINT ring);

    ID3D12Resource* vertices()  const { return vertices_.Get(); }
    ID3D12Resource* indices()   const { return indices_.Get(); }
    ID3D12Resource* draw_args() const { return draw_args_.Get(); }

    void   record_timestamp(ID3D12GraphicsCommandList* cmd, UINT which);
    void   resolve_timestamps(ID3D12GraphicsCommandList* cmd);
    void   read_timestamps();
    double last_mesh_ms() const { return last_mesh_ms_; }

private:
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_clear_, pso_density_, pso_vertices_,
                                pso_quads_, pso_args_, pso_retire_, pso_heights_;

    ComPtr<ID3D12Resource> density_, cell_vertex_, vertices_, indices_;
    ComPtr<ID3D12Resource> counters_, draw_args_;
    ComPtr<ID3D12Resource> counter_readback_, vertex_readback_, args_readback_;
    ComPtr<ID3D12Resource> heights_;
    ComPtr<ID3D12Resource> height_readback_[kReadbackRing];

    ComPtr<ID3D12QueryHeap> timestamps_;
    ComPtr<ID3D12Resource>  timestamp_readback_;
    UINT64                  timestamp_freq_ = 0;
    double                  last_mesh_ms_ = 0.0;
};

}  // namespace engine

#endif  // ENGINE_CHUNK_MESHER_H
