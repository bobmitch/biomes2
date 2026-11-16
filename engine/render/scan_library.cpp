#include "engine/render/scan_library.h"

#include "worldgen/assign/json_lite.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace engine {

namespace {

struct Pack {
    uint32_t size = 0, mips = 0;
    float mean[3] = {}, tile = 1.0f;
    float rough_mean = 0.8f;
    std::vector<uint8_t> a, b;   // whole mip chains, largest first
};

bool read_pack(const std::string& path, Pack& p) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    uint32_t hdr[2] = {};
    float fl[4] = {};
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SCN1", 4) == 0 &&
              std::fread(hdr, 4, 2, f) == 2 && std::fread(fl, 4, 4, f) == 4;
    if (ok) {
        p.size = hdr[0];
        p.mips = hdr[1];
        for (int k = 0; k < 3; ++k) p.mean[k] = fl[k];
        p.tile = fl[3];
        size_t bytes = 0;
        for (uint32_t m = 0, s = p.size; m < p.mips; ++m, s = s > 1 ? s / 2 : 1) bytes += size_t(s) * s * 4;
        p.a.resize(bytes);
        p.b.resize(bytes);
        ok = std::fread(p.a.data(), 1, bytes, f) == bytes && std::fread(p.b.data(), 1, bytes, f) == bytes;
    }
    std::fclose(f);
    if (ok) {
        // The scan's mean roughness, from its smallest mip: the table scales
        // the scan's roughness so its mean lands on the authored value.
        const size_t last = p.b.size() - 4;
        p.rough_mean = (std::max)(p.b[last + 2] / 255.0f, 0.05f);
    }
    return ok;
}

float srgb_to_linear(double c) {
    const float x = c < 0.0 ? 0.0f : float(c);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

std::string hr_text(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

bool ScanLibrary::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap,
                       const std::vector<uint32_t>& material_ids, const std::vector<std::string>& extra,
                       std::string& error) {
    // --- which sets, and which biome takes which --------------------------
    std::vector<std::string> names;
    std::map<std::string, int> index;
    std::vector<Pack> packs;
    struct Row { std::string ground, rock; float g[3], r[3], gr, rr; };
    std::vector<Row> rows;
    std::vector<std::string> missing;
    try {
        const jsonlite::Value root = jsonlite::parse_file("worldgen/data/archetypes.json");
        const jsonlite::Array& mats = root.array("materials");
        auto row_of = [&](const jsonlite::Value& m) {
            Row r;
            r.ground = m.has("ground_scan") ? m.string("ground_scan") : std::string();
            r.rock = m.has("rock_scan") ? m.string("rock_scan") : std::string();
            for (int k = 0; k < 3; ++k) {
                r.g[k] = srgb_to_linear(m.array("ground")[k].num);
                r.r[k] = srgb_to_linear(m.array("rock")[k].num);
            }
            r.gr = float(m.number("ground_roughness"));
            r.rr = float(m.number("rock_roughness"));
            return r;
        };
        for (uint32_t id : material_ids) {
            bool found = false;
            for (size_t m = 0; m < mats.size(); ++m)
                if (uint32_t(mats[m].number("id")) == id) { rows.push_back(row_of(mats[m])); found = true; break; }
            if (!found) throw std::runtime_error("no material with id " + std::to_string(id));
        }
        rows.push_back(row_of(root.at("seabed")));
    } catch (const std::exception& e) {
        error = std::string("scan library: archetypes.json materials: ") + e.what();
        return false;
    }
    auto want = [&](const std::string& n) {
        if (n.empty() || index.count(n)) return;
        Pack p;
        if (!read_pack("assets/textures/packed/" + n + ".bin", p)) {
            index[n] = -1;
            missing.push_back(n);
            return;
        }
        if (!packs.empty() && (p.size != packs[0].size || p.mips != packs[0].mips)) {
            index[n] = -1;
            missing.push_back(n + " (size differs)");
            return;
        }
        index[n] = int(packs.size());
        names.push_back(n);
        packs.push_back(std::move(p));
    };
    for (const Row& r : rows) { want(r.ground); want(r.rock); }
    for (const std::string& n : extra) want(n);
    for (const auto& kv : index) {
        if (kv.second < 0) continue;
        const Pack& p = packs[size_t(kv.second)];
        info_[kv.first] = Info{kv.second, {p.mean[0], p.mean[1], p.mean[2]}, p.tile};
    }

    // --- the table ---------------------------------------------------------
    std::vector<ScanMaterialGpu> table(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        ScanMaterialGpu& t = table[i];
        auto fill = [&](const std::string& n, const float auth[3], float auth_rough,
                        float& layer, float& inv_tile, float tint[3], float& rough, float& scan_rough) {
            const int l = n.empty() ? -1 : index[n];
            layer = float(l);
            rough = auth_rough;
            if (l < 0) { inv_tile = 1.0f; for (int k = 0; k < 3; ++k) tint[k] = 1.0f; scan_rough = 1.0f; return; }
            const Pack& p = packs[size_t(l)];
            inv_tile = 1.0f / (std::max)(p.tile, 0.1f);
            for (int k = 0; k < 3; ++k) tint[k] = auth[k] / (std::max)(p.mean[k], 1e-3f);
            scan_rough = p.rough_mean;
        };
        fill(rows[i].ground, rows[i].g, rows[i].gr, t.ground_layer, t.ground_inv_tile, t.ground_tint,
             t.ground_rough, t.ground_scan_rough);
        fill(rows[i].rock, rows[i].r, rows[i].rr, t.rock_layer, t.rock_inv_tile, t.rock_tint,
             t.rock_rough, t.rock_scan_rough);
    }

    // --- resources -----------------------------------------------------------
    layers_ = UINT(packs.size());
    const UINT size = layers_ ? packs[0].size : 1, mips = layers_ ? packs[0].mips : 1;
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = size;
    td.Height = size;
    td.DepthOrArraySize = UINT16(layers_ ? layers_ : 1);
    td.MipLevels = UINT16(mips);
    td.SampleDesc.Count = 1;
    HRESULT hr;
    if (layers_) {
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&albedo_));
        if (FAILED(hr)) { error = hr_text("scan albedo array", hr); return false; }
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&nra_));
        if (FAILED(hr)) { error = hr_text("scan normal array", hr); return false; }
    }
    const UINT64 table_bytes = sizeof(ScanMaterialGpu) * table.size();
    {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = table_bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // An upload-heap buffer, read in place: 64 bytes a biome, written once.
        hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&table_));
        if (FAILED(hr)) { error = hr_text("scan table", hr); return false; }
        void* p = nullptr;
        table_->Map(0, nullptr, &p);
        std::memcpy(p, table.data(), size_t(table_bytes));
        table_->Unmap(0, nullptr);
    }

    // --- upload, one staging buffer for everything, one wait ---------------
    double mb = 0.0;
    if (layers_) {
        const UINT subs = layers_ * mips;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
        std::vector<UINT> nrows(subs);
        std::vector<UINT64> row_bytes(subs);
        UINT64 total = 0;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->GetCopyableFootprints(&td, 0, subs, 0, fp.data(), nrows.data(), row_bytes.data(), &total);
        D3D12_RESOURCE_DESC sd = {};
        sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        sd.Width = total;
        sd.Height = 1;
        sd.DepthOrArraySize = 1;
        sd.MipLevels = 1;
        sd.SampleDesc.Count = 1;
        sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> staging[2];
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        ComPtr<ID3D12Fence> fence;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) { error = hr_text("scan upload setup", hr); return false; }
        ID3D12Resource* dst[2] = {albedo_.Get(), nra_.Get()};
        for (int t = 0; t < 2; ++t) {
            hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&staging[t]));
            if (FAILED(hr)) { error = hr_text("scan staging", hr); return false; }
            uint8_t* base = nullptr;
            staging[t]->Map(0, nullptr, reinterpret_cast<void**>(&base));
            for (UINT l = 0; l < layers_; ++l) {
                const std::vector<uint8_t>& src = t == 0 ? packs[l].a : packs[l].b;
                size_t off = 0;
                for (UINT m = 0; m < mips; ++m) {
                    const UINT s = m + l * mips;   // D3D12CalcSubresource, one plane
                    const UINT w = (std::max)(size >> m, 1u);
                    for (UINT y = 0; y < nrows[s]; ++y)
                        std::memcpy(base + fp[s].Offset + UINT64(y) * fp[s].Footprint.RowPitch,
                                    src.data() + off + size_t(y) * w * 4, size_t(w) * 4);
                    off += size_t(w) * w * 4;
                }
            }
            staging[t]->Unmap(0, nullptr);
            for (UINT s = 0; s < subs; ++s) {
                D3D12_TEXTURE_COPY_LOCATION d = {}, sl = {};
                d.pResource = dst[t];
                d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                d.SubresourceIndex = s;
                sl.pResource = staging[t].Get();
                sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sl.PlacedFootprint = fp[s];
                cmd->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
            }
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = dst[t];
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &b);
            mb += double(total) / 1e6;
        }
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
    }

    // --- descriptors: null views when there is nothing, so the shader can
    // still be bound and reads the table's -1 layers -----------------------
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = mips;
    sv.Texture2DArray.ArraySize = layers_ ? layers_ : 1;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    device->CreateShaderResourceView(albedo_.Get(), &sv, {h.ptr + SIZE_T(kSrvAlbedo) * inc});
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(nra_.Get(), &sv, {h.ptr + SIZE_T(kSrvNra) * inc});
    D3D12_SHADER_RESOURCE_VIEW_DESC bv = {};
    bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bv.Format = DXGI_FORMAT_UNKNOWN;
    bv.Buffer.NumElements = UINT(table.size());
    bv.Buffer.StructureByteStride = sizeof(ScanMaterialGpu);
    device->CreateShaderResourceView(table_.Get(), &bv, {h.ptr + SIZE_T(kSrvTable) * inc});

    char buf[256];
    std::snprintf(buf, sizeof buf, "%u scan sets at %u^2 (%.0f MB on the GPU), %zu missing", layers_, size,
                  mb, missing.size());
    note_ = buf;
    for (const std::string& m : missing) note_ += " " + m;
    if (!missing.empty()) note_ += " -- run tools/textures/fetch_scans.py";
    return true;
}

bool ScanLibrary::lookup(const std::string& name, const float authored_srgb[3], float& layer, float& inv_tile,
                         float tint[3]) const {
    layer = -1.0f;
    inv_tile = 1.0f;
    for (int k = 0; k < 3; ++k) tint[k] = 1.0f;
    const auto it = info_.find(name);
    if (it == info_.end()) return false;
    layer = float(it->second.layer);
    inv_tile = 1.0f / (std::max)(it->second.tile, 0.05f);
    for (int k = 0; k < 3; ++k) tint[k] = srgb_to_linear(authored_srgb[k]) / (std::max)(it->second.mean[k], 1e-3f);
    return true;
}

}  // namespace engine
