#include "engine/meshing/chunk_mesher.h"
#include "engine/render/shader_compile.h"

#include <cstring>

namespace engine {
namespace {

const char* kKernelPath = "engine/meshing/surface_nets.hlsl";

std::string hr_text(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

bool make_buffer(ID3D12Device* device, UINT64 bytes, D3D12_RESOURCE_STATES state,
                 ComPtr<ID3D12Resource>& out, std::string& error, const char* what) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       state, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

bool make_readback(ID3D12Device* device, UINT64 bytes,
                   ComPtr<ID3D12Resource>& out, std::string& error, const char* what) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

void uav_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* a, ID3D12Resource* b) {
    D3D12_RESOURCE_BARRIER bs[2] = {};
    UINT n = 0;
    if (a) { bs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; bs[n].UAV.pResource = a; ++n; }
    if (b) { bs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; bs[n].UAV.pResource = b; ++n; }
    if (n) cmd->ResourceBarrier(n, bs);
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

struct ChunkConstants {
    float origin[3];
    float voxel;
    UINT  slot;
    UINT  vbase;
    UINT  ibase;
    UINT  slot_caps;
    UINT  brick_first;
    UINT  brick_count;
    UINT  pad0;
    UINT  pad1;
};

// One brick's bytes in the staging buffer: density then mask, so a single
// memcpy pair fills an entry and two copies move it.
const UINT kBrickStageBytes = kBrickVoxelsGpu * 4 + kBrickMaskWordsGpu * 4;

bool make_upload(ID3D12Device* device, UINT64 bytes, ComPtr<ID3D12Resource>& out,
                 std::string& error, const char* what) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

}  // namespace

bool ChunkMesher::init(ID3D12Device* device, UINT64 timestamp_freq, std::string& error) {
    timestamp_freq_ = timestamp_freq;

    // One root parameter for the chunk constants and six root UAVs. No
    // descriptor heap: root descriptors are enough for plain buffers.
    D3D12_ROOT_PARAMETER params[12] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(ChunkConstants) / 4;
    for (UINT i = 0; i < 11; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1 + i].Descriptor.ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 12;
    rsd.pParameters = params;

    ComPtr<ID3DBlob> sig, sig_err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err);
    if (FAILED(hr)) {
        error = "mesher root signature: ";
        error += sig_err ? static_cast<const char*>(sig_err->GetBufferPointer())
                         : hr_text("D3D12SerializeRootSignature", hr).c_str();
        return false;
    }
    hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                     IID_PPV_ARGS(&root_));
    if (FAILED(hr)) { error = hr_text("CreateRootSignature (mesher)", hr); return false; }

    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Scratch, one chunk's worth, reused by every chunk. Meshing serialises on
    // it, which at the measured 0.016 ms a chunk is cheaper than the memory
    // per-chunk scratch would cost.
    if (!make_buffer(device, UINT64(kChunkCorners) * kChunkCorners * kChunkCorners * 4,
                     ua, density_, error, "density scratch")) return false;
    if (!make_buffer(device, UINT64(kChunkCells) * kChunkCells * kChunkCells * 4,
                     ua, cell_vertex_, error, "cell-vertex scratch")) return false;

    if (!make_buffer(device, UINT64(kSlots) * kSlotVertices * sizeof(MeshVertex),
                     ua, vertices_, error, "vertex pool")) return false;
    if (!make_buffer(device, UINT64(kSlots) * kSlotIndices * 4,
                     ua, indices_, error, "index pool")) return false;
    if (!make_buffer(device, UINT64(kSlots) * 2 * 4, ua, counters_, error,
                     "counter buffer")) return false;
    if (!make_buffer(device, UINT64(kSlots) * 5 * 4, ua, draw_args_, error,
                     "draw-args buffer")) return false;

    if (!make_readback(device, UINT64(kSlots) * 2 * 4, counter_readback_, error,
                       "counter readback")) return false;
    if (!make_readback(device, UINT64(kSlotVertices) * sizeof(MeshVertex),
                       vertex_readback_, error, "vertex readback")) return false;
    if (!make_readback(device, UINT64(kSlots) * 5 * 4, args_readback_, error,
                       "args readback")) return false;

    if (!make_buffer(device, UINT64(kSlots) * kHeightSamples * 4, ua, heights_, error,
                     "height pool")) return false;
    for (UINT i = 0; i < kReadbackRing; ++i) {
        if (!make_readback(device, UINT64(kReadbackChunksPerFrame) * kHeightSamples * 4,
                           height_readback_[i], error, "height readback")) return false;
        if (!make_readback(device, UINT64(kReadbackChunksPerFrame) * 4,
                           flag_readback_[i], error, "flag readback")) return false;
        if (!make_readback(device, UINT64(kMeshChunksPerFrame) * kMeshChunkBytes,
                           mesh_readback_[i], error, "mesh readback")) return false;
    }
    if (!make_buffer(device, UINT64(kSlots) * 4, ua, chunk_flags_, error,
                     "chunk flags")) return false;

    if (!make_buffer(device, UINT64(kMaxBricks) * kBrickVoxelsGpu * 4, ua,
                     brick_den_, error, "brick density pool")) return false;
    if (!make_buffer(device, UINT64(kMaxBricks) * kBrickMaskWordsGpu * 4, ua,
                     brick_mask_, error, "brick mask pool")) return false;
    // The header list is small and rewritten every frame, so it is read
    // straight out of an upload heap rather than copied into one more buffer.
    if (!make_upload(device, UINT64(kMaxBrickHeaders) * 16, brick_hdr_, error,
                     "brick headers")) return false;
    if (!make_upload(device, UINT64(kBrickUploadsPerFrame) * kBrickStageBytes,
                     brick_stage_, error, "brick staging")) return false;
    {
        void* p = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(brick_hdr_->Map(0, &none, &p))) {
            error = "could not map brick headers";
            return false;
        }
        brick_hdr_cpu_ = static_cast<int*>(p);
        if (FAILED(brick_stage_->Map(0, &none, &p))) {
            error = "could not map brick staging";
            return false;
        }
        brick_stage_cpu_ = static_cast<unsigned char*>(p);
    }

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2;
    hr = device->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestamps_));
    if (FAILED(hr)) { error = hr_text("CreateQueryHeap", hr); return false; }
    if (!make_readback(device, 16, timestamp_readback_, error, "timestamp readback")) return false;

    return reload_shaders(device, error);
}

bool ChunkMesher::reload_shaders(ID3D12Device* device, std::string& error) {
    const char* entries[7] = {"cs_clear", "cs_density", "cs_vertices",
                              "cs_quads", "cs_args", "cs_retire", "cs_heights"};
    ComPtr<ID3D12PipelineState> built[7];
    for (int i = 0; i < 7; ++i) {
        ComPtr<ID3DBlob> blob;
        if (!compile_shader(kKernelPath, entries[i], "cs_5_1", blob, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root_.Get();
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength = blob->GetBufferSize();
        const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&built[i]));
        if (FAILED(hr)) {
            error = hr_text("CreateComputePipelineState", hr);
            error += std::string(" for ") + entries[i];
            return false;
        }
    }
    // Swapped in only once every kernel compiled, so a bad edit leaves the
    // previous set intact rather than taking the window down.
    pso_clear_    = built[0];
    pso_density_  = built[1];
    pso_vertices_ = built[2];
    pso_quads_    = built[3];
    pso_args_     = built[4];
    pso_retire_   = built[5];
    pso_heights_  = built[6];
    return true;
}

namespace {
void bind_pool(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* root,
               const ChunkConstants& c,
               ID3D12Resource* density, ID3D12Resource* cell_vertex,
               ID3D12Resource* vertices, ID3D12Resource* indices,
               ID3D12Resource* counters, ID3D12Resource* args,
               ID3D12Resource* heights, ID3D12Resource* flags,
               ID3D12Resource* bhdr, ID3D12Resource* bden, ID3D12Resource* bmask) {
    cmd->SetComputeRootSignature(root);
    cmd->SetComputeRoot32BitConstants(0, sizeof(ChunkConstants) / 4, &c, 0);
    cmd->SetComputeRootUnorderedAccessView(1, density->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, cell_vertex->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, vertices->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, indices->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, counters->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, args->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(7, heights->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, flags->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(9, bhdr->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, bden->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(11, bmask->GetGPUVirtualAddress());
}
}  // namespace

void ChunkMesher::begin_frame_bricks() {
    brick_hdr_cursor_ = 0;
    brick_uploads_ = 0;
}

UINT ChunkMesher::push_brick_headers(const int* origins, const UINT* slots, UINT n) {
    if (n == 0) return 0;
    if (brick_hdr_cursor_ + n > kMaxBrickHeaders) return 0;
    const UINT first = brick_hdr_cursor_;
    for (UINT i = 0; i < n; ++i) {
        int* e = brick_hdr_cpu_ + size_t(first + i) * 4;
        e[0] = origins[i * 3 + 0];
        e[1] = origins[i * 3 + 1];
        e[2] = origins[i * 3 + 2];
        e[3] = static_cast<int>(slots[i]);
    }
    brick_hdr_cursor_ += n;
    return first;
}

bool ChunkMesher::record_brick_upload(ID3D12GraphicsCommandList* cmd, UINT slot,
                                      const float* density, const UINT* mask) {
    if (slot >= kMaxBricks || brick_uploads_ >= kBrickUploadsPerFrame) return false;
    const UINT i = brick_uploads_++;
    unsigned char* dst = brick_stage_cpu_ + size_t(i) * kBrickStageBytes;
    std::memcpy(dst, density, kBrickVoxelsGpu * 4);
    std::memcpy(dst + kBrickVoxelsGpu * 4, mask, kBrickMaskWordsGpu * 4);

    transition(cmd, brick_den_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cmd, brick_mask_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(brick_den_.Get(), UINT64(slot) * kBrickVoxelsGpu * 4,
                          brick_stage_.Get(), UINT64(i) * kBrickStageBytes,
                          kBrickVoxelsGpu * 4);
    cmd->CopyBufferRegion(brick_mask_.Get(), UINT64(slot) * kBrickMaskWordsGpu * 4,
                          brick_stage_.Get(),
                          UINT64(i) * kBrickStageBytes + kBrickVoxelsGpu * 4,
                          kBrickMaskWordsGpu * 4);
    transition(cmd, brick_den_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, brick_mask_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

void ChunkMesher::record_mesh(ID3D12GraphicsCommandList* cmd, UINT slot,
                              const float origin[3], float voxel,
                              UINT brick_first, UINT brick_count) {
    ChunkConstants c = {};
    c.origin[0] = origin[0];
    c.origin[1] = origin[1];
    c.origin[2] = origin[2];
    c.voxel = voxel;
    c.slot = slot;
    c.vbase = slot * kSlotVertices;
    c.ibase = slot * kSlotIndices;
    c.slot_caps = (kSlotVertices & 0xFFFFu) | ((kSlotIndices & 0xFFFFu) << 16);
    c.brick_first = brick_first;
    c.brick_count = brick_count;

    bind_pool(cmd, root_.Get(), c, density_.Get(), cell_vertex_.Get(),
              vertices_.Get(), indices_.Get(), counters_.Get(), draw_args_.Get(),
              heights_.Get(), chunk_flags_.Get(), brick_hdr_.Get(),
              brick_den_.Get(), brick_mask_.Get());

    cmd->SetPipelineState(pso_clear_.Get());
    cmd->Dispatch(1, 1, 1);
    uav_barrier(cmd, counters_.Get(), nullptr);

    // 33 corners over 4-wide groups is 9 groups with a tail; the kernel bounds
    // checks rather than padding, because the odd corner count is the design's
    // and will not change.
    const UINT cg = (kChunkCorners + 3) / 4;
    cmd->SetPipelineState(pso_density_.Get());
    cmd->Dispatch(cg, cg, cg);
    uav_barrier(cmd, density_.Get(), nullptr);

    // Heights come straight off the density grid, so they go right after it
    // and before the vertex pass overwrites nothing it needs.
    const UINT hg = (kHeightGrid + 7) / 8;
    cmd->SetPipelineState(pso_heights_.Get());
    cmd->Dispatch(hg, hg, 1);
    uav_barrier(cmd, heights_.Get(), chunk_flags_.Get());

    const UINT vg = (kChunkCells + 3) / 4;
    cmd->SetPipelineState(pso_vertices_.Get());
    cmd->Dispatch(vg, vg, vg);
    uav_barrier(cmd, cell_vertex_.Get(), vertices_.Get());
    uav_barrier(cmd, counters_.Get(), nullptr);

    cmd->SetPipelineState(pso_quads_.Get());
    cmd->Dispatch(vg, vg, vg);
    uav_barrier(cmd, indices_.Get(), counters_.Get());

    cmd->SetPipelineState(pso_args_.Get());
    cmd->Dispatch(1, 1, 1);
    uav_barrier(cmd, draw_args_.Get(), nullptr);
}

void ChunkMesher::record_retire(ID3D12GraphicsCommandList* cmd, UINT slot) {
    ChunkConstants c = {};
    c.slot = slot;
    bind_pool(cmd, root_.Get(), c, density_.Get(), cell_vertex_.Get(),
              vertices_.Get(), indices_.Get(), counters_.Get(), draw_args_.Get(),
              heights_.Get(), chunk_flags_.Get(), brick_hdr_.Get(),
              brick_den_.Get(), brick_mask_.Get());
    cmd->SetPipelineState(pso_retire_.Get());
    cmd->Dispatch(1, 1, 1);
    uav_barrier(cmd, draw_args_.Get(), nullptr);
}

void ChunkMesher::record_to_draw(ID3D12GraphicsCommandList* cmd) {
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Vertices are read through a root SRV in the vertex shader rather than
    // bound as a vertex buffer: with an index buffer bound, SV_VertexID is
    // already the resolved vertex, so there is no input layout to keep in sync.
    transition(cmd, vertices_.Get(), ua, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, indices_.Get(), ua, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    transition(cmd, draw_args_.Get(), ua, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

void ChunkMesher::record_to_compute(ID3D12GraphicsCommandList* cmd) {
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(cmd, vertices_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ua);
    transition(cmd, indices_.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER, ua);
    transition(cmd, draw_args_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, ua);
}

void ChunkMesher::record_counter_copy(ID3D12GraphicsCommandList* cmd) {
    transition(cmd, counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(counter_readback_.Get(), 0, counters_.Get(), 0,
                          UINT64(kSlots) * 2 * 4);
    transition(cmd, counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    transition(cmd, draw_args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(args_readback_.Get(), 0, draw_args_.Get(), 0,
                          UINT64(kSlots) * 5 * 4);
    transition(cmd, draw_args_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ChunkMesher::read_draw_args(UINT first, UINT count, std::vector<UINT>& out) {
    out.clear();
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(kSlots) * 5 * 4};
    if (FAILED(args_readback_->Map(0, &r, &p))) return;
    const UINT* a = static_cast<const UINT*>(p);
    for (UINT s = first; s < first + count && s < kSlots; ++s) {
        for (int k = 0; k < 5; ++k) out.push_back(a[s * 5 + k]);
    }
    D3D12_RANGE none = {0, 0};
    args_readback_->Unmap(0, &none);
}

void ChunkMesher::read_counts(UINT& verts, UINT& inds, UINT& overflowed) {
    verts = 0; inds = 0; overflowed = 0;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(kSlots) * 2 * 4};
    if (FAILED(counter_readback_->Map(0, &r, &p))) return;
    const UINT* c = static_cast<const UINT*>(p);
    for (UINT s = 0; s < kSlots; ++s) {
        const UINT v = c[s * 2 + 0], i = c[s * 2 + 1];
        // The kernel keeps incrementing past the cap and drops the write, so a
        // counter above the cap is exactly the overflow signal.
        if (v > kSlotVertices || i > kSlotIndices) ++overflowed;
        verts += v < kSlotVertices ? v : kSlotVertices;
        inds += i < kSlotIndices ? i : kSlotIndices;
    }
    D3D12_RANGE none = {0, 0};
    counter_readback_->Unmap(0, &none);
}

UINT ChunkMesher::slot_vertex_count(UINT slot) {
    if (slot >= kSlots) return 0;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(kSlots) * 2 * 4};
    if (FAILED(counter_readback_->Map(0, &r, &p))) return 0;
    const UINT n = static_cast<const UINT*>(p)[slot * 2];
    D3D12_RANGE none = {0, 0};
    counter_readback_->Unmap(0, &none);
    return n < kSlotVertices ? n : kSlotVertices;
}

void ChunkMesher::record_vertex_copy(ID3D12GraphicsCommandList* cmd) {
    transition(cmd, vertices_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(vertex_readback_.Get(), 0, vertices_.Get(), 0,
                          UINT64(kSlotVertices) * sizeof(MeshVertex));
    transition(cmd, vertices_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool ChunkMesher::read_vertices(UINT slot, UINT count, std::vector<MeshVertex>& out) {
    out.clear();
    if (slot != 0) return false;   // only slot 0 is copied back
    if (count == 0) return true;
    if (count > kSlotVertices) count = kSlotVertices;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(count) * sizeof(MeshVertex)};
    if (FAILED(vertex_readback_->Map(0, &r, &p))) return false;
    out.assign(static_cast<const MeshVertex*>(p),
               static_cast<const MeshVertex*>(p) + count);
    D3D12_RANGE none = {0, 0};
    vertex_readback_->Unmap(0, &none);
    return true;
}

void ChunkMesher::record_height_copy(ID3D12GraphicsCommandList* cmd, UINT ring,
                                     const UINT* slots, UINT count) {
    if (count == 0) return;
    if (count > kReadbackChunksPerFrame) count = kReadbackChunksPerFrame;
    transition(cmd, heights_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < count; ++i) {
        cmd->CopyBufferRegion(height_readback_[ring].Get(),
                              UINT64(i) * kHeightSamples * 4,
                              heights_.Get(),
                              UINT64(slots[i]) * kHeightSamples * 4,
                              UINT64(kHeightSamples) * 4);
    }
    transition(cmd, heights_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ChunkMesher::record_flag_copy(ID3D12GraphicsCommandList* cmd, UINT ring,
                                   const UINT* slots, UINT count) {
    if (count == 0) return;
    if (count > kReadbackChunksPerFrame) count = kReadbackChunksPerFrame;
    transition(cmd, chunk_flags_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < count; ++i) {
        cmd->CopyBufferRegion(flag_readback_[ring].Get(), UINT64(i) * 4,
                              chunk_flags_.Get(), UINT64(slots[i]) * 4, 4);
    }
    transition(cmd, chunk_flags_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

const UINT* ChunkMesher::mapped_flags(UINT ring, UINT count) {
    if (count == 0 || ring >= kReadbackRing) return nullptr;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(count) * 4};
    if (FAILED(flag_readback_[ring]->Map(0, &r, &p))) return nullptr;
    return static_cast<const UINT*>(p);
}

void ChunkMesher::unmap_flags(UINT ring) {
    if (ring >= kReadbackRing) return;
    D3D12_RANGE none = {0, 0};
    flag_readback_[ring]->Unmap(0, &none);
}

void ChunkMesher::record_mesh_copy(ID3D12GraphicsCommandList* cmd, UINT ring,
                                   const UINT* slots, UINT count) {
    if (count == 0) return;
    if (count > kMeshChunksPerFrame) count = kMeshChunksPerFrame;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(cmd, vertices_.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cmd, indices_.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < count; ++i) {
        const UINT64 dst = UINT64(i) * kMeshChunkBytes;
        cmd->CopyBufferRegion(mesh_readback_[ring].Get(), dst, vertices_.Get(),
                              UINT64(slots[i]) * kMeshVertexBytes, kMeshVertexBytes);
        cmd->CopyBufferRegion(mesh_readback_[ring].Get(), dst + kMeshVertexBytes,
                              indices_.Get(), UINT64(slots[i]) * kMeshIndexBytes,
                              kMeshIndexBytes);
    }
    transition(cmd, vertices_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ua);
    transition(cmd, indices_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ua);
}

const unsigned char* ChunkMesher::mapped_mesh(UINT ring, UINT count) {
    if (count == 0 || ring >= kReadbackRing) return nullptr;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(count) * kMeshChunkBytes};
    if (FAILED(mesh_readback_[ring]->Map(0, &r, &p))) return nullptr;
    return static_cast<const unsigned char*>(p);
}

void ChunkMesher::unmap_mesh(UINT ring) {
    if (ring >= kReadbackRing) return;
    D3D12_RANGE none = {0, 0};
    mesh_readback_[ring]->Unmap(0, &none);
}

void ChunkMesher::slot_counts(UINT slot, UINT& verts, UINT& inds) {
    verts = 0;
    inds = 0;
    if (slot >= kSlots) return;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(kSlots) * 2 * 4};
    if (FAILED(counter_readback_->Map(0, &r, &p))) return;
    const UINT* c = static_cast<const UINT*>(p);
    verts = c[slot * 2 + 0] < kSlotVertices ? c[slot * 2 + 0] : kSlotVertices;
    inds = c[slot * 2 + 1] < kSlotIndices ? c[slot * 2 + 1] : kSlotIndices;
    D3D12_RANGE none = {0, 0};
    counter_readback_->Unmap(0, &none);
}

const float* ChunkMesher::mapped_heights(UINT ring, UINT count) {
    if (count == 0 || ring >= kReadbackRing) return nullptr;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(count) * kHeightSamples * 4};
    if (FAILED(height_readback_[ring]->Map(0, &r, &p))) return nullptr;
    return static_cast<const float*>(p);
}

void ChunkMesher::unmap_heights(UINT ring) {
    if (ring >= kReadbackRing) return;
    D3D12_RANGE none = {0, 0};
    height_readback_[ring]->Unmap(0, &none);
}

void ChunkMesher::record_timestamp(ID3D12GraphicsCommandList* cmd, UINT which) {
    cmd->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, which);
}

void ChunkMesher::resolve_timestamps(ID3D12GraphicsCommandList* cmd) {
    cmd->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                          timestamp_readback_.Get(), 0);
}

void ChunkMesher::read_timestamps() {
    if (timestamp_freq_ == 0) return;
    void* p = nullptr;
    D3D12_RANGE r = {0, 16};
    if (FAILED(timestamp_readback_->Map(0, &r, &p))) return;
    const UINT64* t = static_cast<const UINT64*>(p);
    if (t[1] > t[0]) last_mesh_ms_ = 1000.0 * double(t[1] - t[0]) / double(timestamp_freq_);
    D3D12_RANGE none = {0, 0};
    timestamp_readback_->Unmap(0, &none);
}

}  // namespace engine
