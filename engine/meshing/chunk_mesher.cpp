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

}  // namespace

bool ChunkMesher::init(ID3D12Device* device, UINT64 timestamp_freq, std::string& error) {
    timestamp_freq_ = timestamp_freq;
    // One root parameter for the chunk constants and six root UAVs. No
    // descriptor heap: root descriptors are enough for plain buffers, and a
    // heap is machinery B3 can introduce when there is something to index.
    D3D12_ROOT_PARAMETER params[7] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;      // origin.xyz, voxel
    for (UINT i = 0; i < 6; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1 + i].Descriptor.ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 7;
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
    if (!make_buffer(device, UINT64(kChunkCorners) * kChunkCorners * kChunkCorners * 4,
                     ua, density_, error, "density buffer")) return false;
    if (!make_buffer(device, UINT64(kChunkCells) * kChunkCells * kChunkCells * 4,
                     ua, cell_vertex_, error, "cell-vertex buffer")) return false;
    if (!make_buffer(device, UINT64(kMaxVertices) * sizeof(MeshVertex),
                     ua, vertices_, error, "vertex buffer")) return false;
    if (!make_buffer(device, UINT64(kMaxIndices) * 4,
                     ua, indices_, error, "index buffer")) return false;
    if (!make_buffer(device, 16, ua, counters_, error, "counter buffer")) return false;
    if (!make_buffer(device, 32, ua, draw_args_, error, "draw-args buffer")) return false;
    if (!make_readback(device, 16, stats_readback_, error, "stats readback")) return false;
    if (!make_readback(device, UINT64(kMaxVertices) * sizeof(MeshVertex),
                       vertex_readback_, error, "vertex readback")) return false;

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2;
    hr = device->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestamps_));
    if (FAILED(hr)) { error = hr_text("CreateQueryHeap", hr); return false; }
    if (!make_readback(device, 16, timestamp_readback_, error, "timestamp readback")) return false;

    return reload_shaders(device, error);
}

bool ChunkMesher::reload_shaders(ID3D12Device* device, std::string& error) {
    struct Build {
        const char* entry;
        ComPtr<ID3D12PipelineState> pso;
    } builds[] = {
        {"cs_clear",    {}},
        {"cs_density",  {}},
        {"cs_vertices", {}},
        {"cs_quads",    {}},
        {"cs_args",     {}},
    };

    ComPtr<ID3D12PipelineState> clear_pso;
    for (int i = 0; i < 5; ++i) {
        ComPtr<ID3DBlob> blob;
        if (!compile_shader(kKernelPath, builds[i].entry, "cs_5_1", blob, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root_.Get();
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength = blob->GetBufferSize();
        const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&builds[i].pso));
        if (FAILED(hr)) {
            error = hr_text("CreateComputePipelineState", hr);
            error += std::string(" for ") + builds[i].entry;
            return false;
        }
    }
    // Swapped in only once every kernel compiled, so a bad edit leaves the
    // previous set intact.
    pso_clear_    = builds[0].pso;
    pso_density_  = builds[1].pso;
    pso_vertices_ = builds[2].pso;
    pso_quads_    = builds[3].pso;
    pso_args_     = builds[4].pso;
    return true;
}

void ChunkMesher::record(ID3D12GraphicsCommandList* cmd, const float origin[3], float voxel) {
    cmd->SetComputeRootSignature(root_.Get());
    float consts[4] = {origin[0], origin[1], origin[2], voxel};
    cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cmd->SetComputeRootUnorderedAccessView(1, density_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, cell_vertex_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, vertices_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, indices_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, counters_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, draw_args_->GetGPUVirtualAddress());

    cmd->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    cmd->SetPipelineState(pso_clear_.Get());
    cmd->Dispatch(1, 1, 1);
    uav_barrier(cmd, counters_.Get(), nullptr);

    // 33 corners over 4-wide groups is 9 groups with a tail; the kernel bounds
    // checks rather than padding the buffer, because the odd corner count is
    // the design's and will not change.
    const UINT cg = (kChunkCorners + 3) / 4;
    cmd->SetPipelineState(pso_density_.Get());
    cmd->Dispatch(cg, cg, cg);
    uav_barrier(cmd, density_.Get(), nullptr);

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

    cmd->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
}

void ChunkMesher::record_transition_to_draw(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b[3] = {};
    for (int i = 0; i < 3; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    // Vertices are read through a root SRV in the vertex shader rather than
    // bound as a vertex buffer: SV_VertexID already resolves through the index
    // buffer, so there is no input layout to keep in sync with a struct.
    b[0].Transition.pResource = vertices_.Get();
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = indices_.Get();
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    b[2].Transition.pResource = draw_args_.Get();
    b[2].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    cmd->ResourceBarrier(3, b);
}

void ChunkMesher::record_readback(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = counters_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &b);
    cmd->CopyBufferRegion(stats_readback_.Get(), 0, counters_.Get(), 0, 16);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &b);

    // The vertices too, while they are still in UNORDERED_ACCESS and before
    // record_transition_to_draw hands them to the vertex shader.
    D3D12_RESOURCE_BARRIER vb = {};
    vb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    vb.Transition.pResource = vertices_.Get();
    vb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    vb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    vb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &vb);
    cmd->CopyBufferRegion(vertex_readback_.Get(), 0, vertices_.Get(), 0,
                          UINT64(kMaxVertices) * sizeof(MeshVertex));
    vb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    vb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &vb);

    cmd->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                          timestamp_readback_.Get(), 0);
}

bool ChunkMesher::read_stats(ID3D12Device*, UINT& verts, UINT& inds, double& ms) {
    verts = 0; inds = 0; ms = 0.0;

    void* p = nullptr;
    D3D12_RANGE r = {0, 16};
    if (FAILED(stats_readback_->Map(0, &r, &p))) return false;
    const UINT* c = static_cast<const UINT*>(p);
    verts = c[0];
    inds = c[1];
    D3D12_RANGE none = {0, 0};
    stats_readback_->Unmap(0, &none);

    if (timestamp_freq_ != 0) {
        D3D12_RANGE tr = {0, 16};
        if (SUCCEEDED(timestamp_readback_->Map(0, &tr, &p))) {
            const UINT64* t = static_cast<const UINT64*>(p);
            if (t[1] > t[0]) {
                ms = 1000.0 * double(t[1] - t[0]) / double(timestamp_freq_);
            }
            timestamp_readback_->Unmap(0, &none);
        }
    }
    return true;
}

bool ChunkMesher::read_vertices(UINT count, std::vector<MeshVertex>& out) {
    out.clear();
    if (count == 0) return true;
    if (count > kMaxVertices) count = kMaxVertices;
    void* p = nullptr;
    D3D12_RANGE r = {0, SIZE_T(count) * sizeof(MeshVertex)};
    if (FAILED(vertex_readback_->Map(0, &r, &p))) return false;
    out.assign(static_cast<const MeshVertex*>(p),
               static_cast<const MeshVertex*>(p) + count);
    D3D12_RANGE none = {0, 0};
    vertex_readback_->Unmap(0, &none);
    return true;
}

}  // namespace engine
