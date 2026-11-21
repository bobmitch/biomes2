// d3d12.h (through forest_map.h) brings in windows.h, whose min and max
// macros break std::min in the worldgen headers below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/forest_map.h"
#include "engine/field/world_gpu.h"
#include "engine/trees/tree_mesh.h"
#include "worldgen/world.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <windows.h>
#include <DirectXPackedVector.h>

namespace engine {

namespace {

float srgb_lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

std::string hr_text(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

bool ForestMap::build(const WorldGpu& world, const std::vector<TreeLook>& looks, ID3D12Device* device,
                      ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    const worldgen::SeedWorld& w = world.world();
    const worldgen::WorldParams& wp = world.params();
    const worldgen::ArchetypeTable& table = world.table();
    const worldgen::TreeTable& tt = table.trees;
    extent_ = 2.0f * wp.radius;
    const size_t ns = tt.species.size();
    const int N = kSize;
    std::vector<float> a(size_t(N) * N * 4, 0.0f), b(size_t(N) * N * 4, 0.0f);
    std::vector<float> expected(size_t(N) * N, 0.0f);   // trees per texel the map predicts (the check)

    // Per species: mean height, crown radius as a share of height, leaf
    // colour (linear), conifer or not.
    std::vector<float> h_mean(ns), crown(ns), leaf(ns * 3), conifer(ns);
    for (size_t s = 0; s < ns; ++s) {
        h_mean[s] = 0.5f * (tt.species[s].height_min + tt.species[s].height_max);
        const TreeLook* lk = s < looks.size() ? &looks[s] : nullptr;
        crown[s] = lk ? lk->crown_radius : 0.35f;
        for (int k = 0; k < 3; ++k) leaf[s * 3 + k] = lk ? srgb_lin(lk->leaf[k]) : 0.05f;
        conifer[s] = lk && (lk->form == TreeForm::Conifer || lk->form == TreeForm::Pine) ? 1.0f : 0.0f;
    }

    std::atomic<int> next_row(0), forested(0), land(0);
    auto worker = [&]() {
        for (;;) {
            const int j = next_row.fetch_add(1);
            if (j >= N) return;
            for (int i = 0; i < N; ++i) {
                const float x = -0.5f * extent_ + (float(i) + 0.5f) * extent_ / N;
                const float z = -0.5f * extent_ + (float(j) + 0.5f) * extent_ / N;
                if (x * x + z * z > wp.radius * wp.radius) continue;
                int bio[4];
                float bw[4];
                const int nb = worldgen::biome_mix(w, wp, x, z, bio, bw);
                if (nb == 0) continue;
                land.fetch_add(1);
                // Layer 8's sharpening, as tree_at_cell does it.
                {
                    float lnd = 0.0f, sharp = 0.0f, sw[4];
                    for (int k = 0; k < nb; ++k) {
                        lnd += bw[k];
                        float p = 1.0f;
                        for (int e = 0; e < tt.blend_sharpen; ++e) p *= bw[k];
                        sw[k] = p;
                        sharp += p;
                    }
                    if (sharp > 0.0f) for (int k = 0; k < nb; ++k) bw[k] = sw[k] / sharp * lnd;
                }
                float density = 0.0f;
                for (int k = 0; k < nb; ++k) {
                    const worldgen::BiomeArchetype& arch = table[bio[k]];
                    if (arch.tree_density <= 0.0f) continue;
                    density += bw[k] * arch.tree_density *
                               tree_patch(x, z, tt.patch_freq, arch.tree_cover, tt.patch_edge, w.s_tree_patch);
                }
                if (density <= 0.0f) continue;
                // The ground: shore, slope (over a texel), altitude fit.
                const float r = std::sqrt(x * x + z * z) / wp.radius;
                const float h = worldgen::terrain_height(w, wp, table, x, z, r);
                if (h < table.sea_level + tt.shore) continue;
                const float e = 0.5f * extent_ / N;
                const float hx = worldgen::terrain_height(w, wp, table, x + e, z, r) -
                                 worldgen::terrain_height(w, wp, table, x - e, z, r);
                const float hz = worldgen::terrain_height(w, wp, table, x, z + e, r) -
                                 worldgen::terrain_height(w, wp, table, x, z - e, r);
                const float slope = std::sqrt(hx * hx + hz * hz) / (2.0f * e);
                // A texel's slope is an average over twenty metres; the cells
                // in it vary round it. A hard cut at the limit called whole
                // texels bare that held gentle cells with trees on them, so
                // the share kept falls off over 0.7 to 1.3 x the limit.
                const float slope_keep = std::clamp((1.3f * tt.max_slope - slope) / (0.6f * tt.max_slope), 0.0f, 1.0f);
                if (slope_keep <= 0.0f) continue;
                // Rivers: layer 8 plants nothing within a channel's width + 1 m
                // of it, and channels up to ~40 m wide run through woods.
                // Left out at first ("narrower than a texel"), the shell laid
                // canopy straight over them -- the check's main source of
                // texels the map called wood and layer 8 left bare. The share
                // of the texel inside the exclusion goes, not the whole texel.
                float river_keep = 1.0f;
                {
                    float rw = 0.0f, rd = 0.0f;
                    // Two statements: see the river exclusion in tree_at_cell.
                    const float river_d = worldgen::nearest_river(w, wp, x, z, rw, rd);
                    const float edge = rw + 1.0f;
                    river_keep = std::clamp((river_d - edge) / (2.0f * e) + 0.5f, 0.0f, 1.0f);
                    if (river_keep <= 0.0f) continue;
                }
                float total = 0.0f, best_fit = 0.0f, hsum = 0.0f, csum = 0.0f, con = 0.0f;
                float lsum[3] = {0.0f, 0.0f, 0.0f};
                for (size_t s = 0; s < ns && s < 64; ++s) {
                    float ws = 0.0f;
                    for (int k = 0; k < nb; ++k) {
                        const worldgen::BiomeArchetype& arch = table[bio[k]];
                        if (arch.tree_density <= 0.0f || s >= arch.tree_species.size()) continue;
                        ws += bw[k] * arch.tree_species[s];
                    }
                    const worldgen::TreeSpecies& sp = tt.species[s];
                    const float fit = tree_altitude_fit(h - table.sea_level, sp.alt_lo, sp.alt_hi, tt.altitude_ramp);
                    const float wsp = ws * fit;
                    if (wsp <= 0.0f) continue;
                    best_fit = (std::max)(best_fit, fit);
                    total += wsp;
                    const float ht = h_mean[s] * (0.55f + 0.45f * fit);
                    hsum += wsp * ht;
                    csum += wsp * crown[s] * ht;
                    con += wsp * conifer[s];
                    for (int k = 0; k < 3; ++k) lsum[k] += wsp * leaf[s * 3 + k];
                }
                if (total <= 0.0f) continue;
                // Trees per square metre (one candidate per cell^2, kept with
                // the density and then the fit roll), and the canopy they make:
                // crowns dropped at random cover 1 - exp(-n * area).
                const float n = density * best_fit * river_keep * slope_keep / (tt.cell * tt.cell);
                const float cr = csum / total;
                const float cover = 1.0f - std::exp(-n * 3.14159f * cr * cr);
                expected[size_t(j) * N + i] = n * (extent_ / N) * (extent_ / N);
                if (cover < 0.01f) continue;
                if (cover > 0.05f) forested.fetch_add(1);
                float* pa = &a[(size_t(j) * N + i) * 4];
                pa[0] = cover;
                pa[1] = hsum / total;
                pa[2] = con / total;
                float* pb = &b[(size_t(j) * N + i) * 4];
                for (int k = 0; k < 3; ++k) pb[k] = lsum[k] / total;
            }
        }
    };
    {
        unsigned nt = std::thread::hardware_concurrency();
        nt = nt ? nt : 4;
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nt; ++t) pool.emplace_back(worker);
        for (std::thread& t : pool) t.join();
    }
    forested_ = land.load() ? float(forested.load()) / float(land.load()) : 0.0f;

    // BIOMES_FOREST_CHECK: the map against layer 8 itself. Every 16th texel
    // each way, every tree cell in it run through tree_at_cell; the trees it
    // grew against the trees the map expected, and where the map expected a
    // wood and layer 8 grew none, what rejected the cells.
    if (std::getenv("BIOMES_FOREST_CHECK")) {
        static const char* kReason[worldgen::TREE_REJECT_COUNT] = {
            "kept", "outside", "no land", "clearing", "roll", "shore", "slope", "river", "altitude", "fit roll", "surface", "flat site"};
        double sum_exp = 0.0, sum_real = 0.0;
        int texels = 0, wrong_wood = 0, missing_wood = 0;
        long long wrong_why[worldgen::TREE_REJECT_COUNT] = {0}, all_why[worldgen::TREE_REJECT_COUNT] = {0};
        const float tc = tt.cell, tex = extent_ / N;
        for (int j = 0; j < N; j += 16) {
            for (int i = 0; i < N; i += 16) {
                const float x0 = -0.5f * extent_ + i * tex, z0 = -0.5f * extent_ + j * tex;
                const float e = expected[size_t(j) * N + i];
                int real = 0;
                long long why[worldgen::TREE_REJECT_COUNT] = {0};
                const int ci0 = int(std::floor(x0 / tc)), ci1 = int(std::floor((x0 + tex) / tc));
                const int cj0 = int(std::floor(z0 / tc)), cj1 = int(std::floor((z0 + tex) / tc));
                for (int cj = cj0; cj < cj1; ++cj)
                    for (int ci = ci0; ci < ci1; ++ci) {
                        worldgen::TreeInstance t;
                        int r = 0;
                        if (worldgen::tree_at_cell(w, wp, table, ci, cj, t, &r)) ++real;
                        ++why[r];
                    }
                if (why[worldgen::TREE_NO_LAND] + why[worldgen::TREE_OUTSIDE] > 0 && e == 0.0f && real == 0) continue;
                ++texels;
                sum_exp += e;
                sum_real += real;
                for (int r = 0; r < worldgen::TREE_REJECT_COUNT; ++r) all_why[r] += why[r];
                if (e >= 3.0f && real == 0) {
                    ++wrong_wood;
                    for (int r = 0; r < worldgen::TREE_REJECT_COUNT; ++r) wrong_why[r] += why[r];
                }
                if (e < 0.2f && real >= 3) ++missing_wood;
            }
        }
        if (FILE* f = std::fopen("build/forest_check.txt", "w")) {
            std::fprintf(f, "forest map vs layer 8, %d land texels sampled\n", texels);
            std::fprintf(f, "trees expected %.0f, grown %.0f (ratio %.2f)\n", sum_exp, sum_real, sum_real / (std::max)(sum_exp, 1.0));
            std::fprintf(f, "texels the map calls wood (>= 3 trees) where layer 8 grew none: %d\n", wrong_wood);
            std::fprintf(f, "texels the map calls bare (< 0.2) where layer 8 grew >= 3: %d\n", missing_wood);
            std::fprintf(f, "\nwhy those wrong-wood cells were rejected:\n");
            for (int r = 0; r < worldgen::TREE_REJECT_COUNT; ++r)
                if (wrong_why[r]) std::fprintf(f, "  %-9s %lld\n", kReason[r], wrong_why[r]);
            std::fprintf(f, "\nall sampled cells by outcome:\n");
            for (int r = 0; r < worldgen::TREE_REJECT_COUNT; ++r)
                if (all_why[r]) std::fprintf(f, "  %-9s %lld\n", kReason[r], all_why[r]);
            std::fclose(f);
        }
    }

    // --- upload: two RGBA16F textures, one staging buffer, one wait -----------
    D3D12_HEAP_PROPERTIES def = {}, up = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = N;
    td.Height = N;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC sd = {};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    sd.Width = total * 2;
    sd.Height = 1;
    sd.DepthOrArraySize = 1;
    sd.MipLevels = 1;
    sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    HRESULT hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&staging));
    if (FAILED(hr)) { error = hr_text("forest map staging", hr); return false; }
    uint8_t* p = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (int t = 0; t < 2; ++t) {
        const std::vector<float>& src = t == 0 ? a : b;
        for (int y = 0; y < N; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(p + UINT64(t) * total + fp.Offset + UINT64(y) * fp.Footprint.RowPitch);
            for (int q = 0; q < N * 4; ++q) row[q] = DirectX::PackedVector::XMConvertFloatToHalf(src[size_t(y) * N * 4 + q]);
        }
    }
    staging->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) { error = hr_text("forest map upload setup", hr); return false; }
    for (int t = 0; t < 2; ++t) {
        hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tex_[t]));
        if (FAILED(hr)) { error = hr_text("forest map texture", hr); return false; }
        D3D12_TEXTURE_COPY_LOCATION d = {}, s = {};
        d.pResource = tex_[t].Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = staging.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = fp;
        s.PlacedFootprint.Offset += UINT64(t) * total;
        cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        D3D12_RESOURCE_BARRIER br = {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = tex_[t].Get();
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        br.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &br);
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
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex_[0].Get(), &sv, {h0.ptr + SIZE_T(kSrvA) * inc});
    device->CreateShaderResourceView(tex_[1].Get(), &sv, {h0.ptr + SIZE_T(kSrvB) * inc});
    bake_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ready_ = true;
    return true;
}

}  // namespace engine
