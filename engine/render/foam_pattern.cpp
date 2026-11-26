#include "engine/render/foam_pattern.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

using Microsoft::WRL::ComPtr;

namespace engine {

namespace {

std::string foam_hr(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

uint32_t cell_hash(int x, int y) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

// One point a cell, wrapped so the tile repeats: its position in cells and
// its hole's radius.
void cell_point(int cx, int cy, float& px, float& py, float& r) {
    const int K = FoamPattern::kCells;
    const int wx = ((cx % K) + K) % K, wy = ((cy % K) + K) % K;
    const uint32_t h = cell_hash(wx, wy);
    const float u0 = float(h & 1023u) / 1023.0f;
    const float u1 = float((h >> 10) & 1023u) / 1023.0f;
    const float u2 = float((h >> 20) & 1023u) / 1023.0f;
    px = float(cx) + 0.15f + 0.7f * u0;
    py = float(cy) + 0.15f + 0.7f * u1;
    r = 0.45f + 0.45f * u2;
}

uint8_t unorm8(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return uint8_t(v * 255.0f + 0.5f);
}

}  // namespace

bool FoamPattern::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap, UINT slot,
                       std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    const int N = kSize;
    const float per = float(kCells) / float(N);   // cells a texel

    // --- the bake: level 0 in float, then a box-filtered chain ---------------
    std::vector<float> lv(size_t(N) * N * 4);
    double sum = 0.0;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float px = (float(x) + 0.5f) * per, py = (float(y) + 0.5f) * per;
            const int cx = int(std::floor(px)), cy = int(std::floor(py));
            float best = 1e9f, bdx = 0.0f, bdy = 0.0f, br = 1.0f;
            for (int k = 0; k < 9; ++k) {
                float qx, qy, r;
                cell_point(cx + k % 3 - 1, cy + k / 3 - 1, qx, qy, r);
                const float dx = px - qx, dy = py - qy;
                const float dd = (dx * dx + dy * dy) / (r * r);
                if (dd < best) { best = dd; bdx = dx; bdy = dy; br = r; }
            }
            const float b = std::sqrt(best);
            const float f = (std::max)(b * br, 1e-4f);
            float* o = &lv[(size_t(y) * N + x) * 4];
            o[0] = (std::min)(b, 1.0f);
            o[1] = bdx / (f * br) / 2.5f * 0.5f + 0.5f;
            o[2] = bdy / (f * br) / 2.5f * 0.5f + 0.5f;
            o[3] = (std::min)(b * 0.5f, 1.0f);
            sum += o[0];
        }
    }
    mean_ = float(sum / double(size_t(N) * N));

    UINT mips = 0;
    for (int s = N; s >= 1; s >>= 1) ++mips;
    std::vector<std::vector<uint8_t>> levels(mips);
    {
        std::vector<float> cur = lv;
        int s = N;
        for (UINT m = 0; m < mips; ++m) {
            levels[m].resize(size_t(s) * s * 4);
            for (size_t i = 0; i < levels[m].size(); ++i) levels[m][i] = unorm8(cur[i]);
            if (s == 1) break;
            const int h = s / 2;
            std::vector<float> nxt(size_t(h) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < h; ++x)
                    for (int c = 0; c < 4; ++c)
                        nxt[(size_t(y) * h + x) * 4 + c] =
                            0.25f * (cur[((size_t(2 * y) * s) + 2 * x) * 4 + c] + cur[((size_t(2 * y) * s) + 2 * x + 1) * 4 + c] +
                                     cur[((size_t(2 * y + 1) * s) + 2 * x) * 4 + c] +
                                     cur[((size_t(2 * y + 1) * s) + 2 * x + 1) * 4 + c]);
            cur.swap(nxt);
            s = h;
        }
    }

    // --- the upload: one staging buffer, one wait -----------------------------
    D3D12_HEAP_PROPERTIES def = {}, up = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = UINT64(N);
    td.Height = UINT(N);
    td.DepthOrArraySize = 1;
    td.MipLevels = UINT16(mips);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mips);
    std::vector<UINT> rows(mips);
    std::vector<UINT64> row_bytes(mips);
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, mips, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    D3D12_RESOURCE_DESC sd = {};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    sd.Width = total;
    sd.Height = 1;
    sd.DepthOrArraySize = 1;
    sd.MipLevels = 1;
    sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    HRESULT hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&staging));
    if (FAILED(hr)) { error = foam_hr("foam pattern staging", hr); return false; }
    uint8_t* p = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT m = 0; m < mips; ++m) {
        const size_t w = size_t(N >> m) * 4;
        for (UINT y = 0; y < rows[m]; ++y)
            std::memcpy(p + fp[m].Offset + UINT64(y) * fp[m].Footprint.RowPitch, levels[m].data() + y * w, w);
    }
    staging->Unmap(0, nullptr);
    hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&tex_));
    if (FAILED(hr)) { error = foam_hr("foam pattern texture", hr); return false; }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) { error = foam_hr("foam pattern upload setup", hr); return false; }
    for (UINT m = 0; m < mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION d = {}, s = {};
        d.pResource = tex_.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = m;
        s.pResource = staging.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = fp[m];
        cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = tex_.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &br);
    cmd->Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(slot) * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.Texture2D.MipLevels = mips;
    device->CreateShaderResourceView(tex_.Get(), &sv, h);
    bake_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace engine
