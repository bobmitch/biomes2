// ---------------------------------------------------------------------------
// B2: meshing one chunk with GPU surface nets.
//
// Owns the buffers and the four compute passes. It records into a caller's
// command list rather than submitting its own, because B3 wants several chunks
// meshed in one submission and a class that submits for itself cannot be
// batched.
//
// The counts never come back to the CPU. The draw is indirect, from arguments
// the last pass writes, so adding a chunk never adds a stall. That is the
// whole reason this is structured the way it is -- see the design document's
// readback budget.
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

// The design's chunk: 32 voxels of 0.5 m, 33 corners of field samples.
static const UINT kChunkCells   = 32;
static const UINT kChunkCorners = 33;

// Provably sufficient for one chunk, not guesses. At most one vertex per cell
// (32^3 = 32,768) and at most three quads per cell (32^3 * 3 * 6 = 589,824
// indices). The bounds checks in the kernel are belt and braces.
static const UINT kMaxVertices = 40000;
static const UINT kMaxIndices  = 600000;

struct MeshVertex {
    float pos[3];
    float nrm[3];
};

class ChunkMesher {
public:
    // timestamp_freq comes from the command queue the work will run on:
    // ticks are per-queue, so there is no device-wide answer to ask for.
    bool init(ID3D12Device* device, UINT64 timestamp_freq, std::string& error);
    bool reload_shaders(ID3D12Device* device, std::string& error);

    // Records the four dispatches. The caller executes and waits.
    void record(ID3D12GraphicsCommandList* cmd, const float origin[3], float voxel);

    // Leaves the buffers in the states the indirect draw needs. Called once
    // after meshing, not per frame: nothing writes them again until a remesh.
    void record_transition_to_draw(ID3D12GraphicsCommandList* cmd);

    // Valid after the recorded work has completed.
    ID3D12Resource* vertices()  const { return vertices_.Get(); }
    ID3D12Resource* indices()   const { return indices_.Get(); }
    ID3D12Resource* draw_args() const { return draw_args_.Get(); }

    // Copies the counters and the GPU timestamps to readback buffers. Only for
    // reporting -- nothing in the draw path reads them.
    void record_readback(ID3D12GraphicsCommandList* cmd);
    bool read_stats(ID3D12Device* device, UINT& verts, UINT& indices, double& ms);

    // The meshed vertices, on the CPU. For checking the mesh against the field
    // it came from -- not a path anything in the frame uses.
    bool read_vertices(UINT count, std::vector<MeshVertex>& out);

private:
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_clear_, pso_density_, pso_vertices_, pso_quads_, pso_args_;

    ComPtr<ID3D12Resource> density_, cell_vertex_, vertices_, indices_;
    ComPtr<ID3D12Resource> counters_, draw_args_;
    ComPtr<ID3D12Resource> stats_readback_, vertex_readback_;

    ComPtr<ID3D12QueryHeap> timestamps_;
    ComPtr<ID3D12Resource>  timestamp_readback_;
    UINT64                  timestamp_freq_ = 0;
};

}  // namespace engine

#endif  // ENGINE_CHUNK_MESHER_H
