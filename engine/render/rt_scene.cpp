#ifndef NOMINMAX
#define NOMINMAX   // windows.h, via d3d12.h, would turn worldgen's std::min into a macro
#endif
#include "engine/render/rt_scene.h"
#include "engine/render/tree_sway.h"
#include "engine/rocks/rock_field.h"
#include "engine/meshing/chunk_mesher.h"
#include "engine/trees/leaf_cutout.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

namespace engine {

namespace {
float srgb_lin(float c) {
    c = c < 0.0f ? 0.0f : c;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
void uav_all(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    cmd->ResourceBarrier(1, &b);
}
}  // namespace

// Matches TreeRtInfo in gi_trace.hlsli. bark[3] is the crown volume's
// extinction, 1/m.
struct TreeRtInfo {
    UINT vbase, ibase_bark, ibase_leaf, narrow;   // narrow: weeping leaves, as the raster's leaf_kind
    float bark[4], leaf[4];
};

bool RtScene::make_buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav,
                          ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes > 0 ? bytes : 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out)));
}

bool RtScene::init(ID3D12Device* device, std::string& error) {
    ready_ = false;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev_)))) { error = "no ID3D12Device5"; return false; }
    slots_.assign(kSlots, SlotBlas());
    latest_mesh_.assign(kSlots, 0);

    // Scratch for a frame's BLAS builds: the worst chunk's scratch times the
    // budget. A full slot is 12,288 triangles.
    {
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = kSlots * kSlotVertices;
        g.Triangles.VertexBuffer.StrideInBytes = sizeof(MeshVertex);
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount = kSlotIndices;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        const UINT64 per = (pi.ScratchDataSizeInBytes + 255) & ~UINT64(255);
        scratch_size_ = per * kBuildsPerFrame;
        if (!make_buffer(scratch_size_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, scratch_)) {
            error = "rt scratch";
            return false;
        }
    }
    // The TLAS at its largest: every slot and the tree cap. Two, because the
    // frame in flight may still be tracing last frame's.
    max_instances_ = kSlots + kMaxTreeInstances;
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = max_instances_;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        tlas_size_ = pi.ResultDataMaxSizeInBytes;
        for (int k = 0; k < 2; ++k) {
            if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, tlas_[k]) ||
                !make_buffer(UINT64(max_instances_) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ, false, inst_up_[k])) {
                error = "rt tlas";
                return false;
            }
            inst_up_[k]->Map(0, nullptr, &inst_cpu_[k]);
        }
        if (!make_buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         true, tlas_scratch_)) {
            error = "rt tlas scratch";
            return false;
        }
        stats_.tlas_bytes = pi.ResultDataMaxSizeInBytes * 2;
        stats_.scratch_bytes = scratch_size_ + pi.ScratchDataSizeInBytes;
    }
    ready_ = true;
    return true;
}

void RtScene::note_meshed(const std::vector<Streamer::MeshedChunk>& meshed, bool landed) {
    const unsigned long long frame_index = landed ? tick_ - 3 : tick_;
    for (size_t i = 0; i < meshed.size(); ++i) {
        if (meshed[i].key.lod > kMaxLod) continue;
        Pending p;
        p.key = meshed[i].key;
        p.slot = meshed[i].slot;
        p.frame = frame_index;
        pending_.push_back(p);
        ++stats_.noted;
        if (meshed[i].slot < slots_.size()) slots_[meshed[i].slot].queued = meshed[i].key;
        if (meshed[i].slot < latest_mesh_.size()) latest_mesh_[meshed[i].slot] = frame_index;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS RtScene::tlas() const {
    return tlas_[tlas_cur_] ? tlas_[tlas_cur_]->GetGPUVirtualAddress() : 0;
}

bool RtScene::build_trees(ID3D12GraphicsCommandList4* cl, const std::vector<TreeModel>& models,
                          const std::vector<TreeLook>& looks) {
    // Positions and indices of every model's LOD 1, bark first then leaves,
    // in two upload buffers that stay: they are the BLAS inputs and what
    // gi_trace reads a hit's normal from.
    std::vector<float> pos, attr;   // attr: u, v, kind, seed -- what the leaf alpha test reads
    std::vector<UINT> idx;
    std::vector<TreeRtInfo> info(models.size());
    struct Range { UINT vbase, vcount, bark_first, bark_count, leaf_first, leaf_count; };
    std::vector<Range> ranges(models.size());
    for (size_t m = 0; m < models.size(); ++m) {
        const TreeMesh& mesh = models[m].lod[1];
        Range& r = ranges[m];
        r.vbase = UINT(pos.size() / 3);
        r.vcount = UINT(mesh.v.size());
        for (size_t i = 0; i < mesh.v.size(); ++i) {
            for (int k = 0; k < 3; ++k) pos.push_back(mesh.v[i].pos[k]);
            attr.push_back(mesh.v[i].uv[0]);
            attr.push_back(mesh.v[i].uv[1]);
            attr.push_back(mesh.v[i].kind);
            attr.push_back(mesh.v[i].seed);
        }
        r.bark_first = UINT(idx.size());
        for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
            const float kind = mesh.v[mesh.idx[t]].kind;
            if (kind > 0.5f && kind < 2.5f) continue;   // leaves and needles go in the second geometry
            for (int k = 0; k < 3; ++k) idx.push_back(mesh.idx[t + k]);
        }
        r.bark_count = UINT(idx.size()) - r.bark_first;
        r.leaf_first = UINT(idx.size());
        for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
            const float kind = mesh.v[mesh.idx[t]].kind;
            if (!(kind > 0.5f && kind < 2.5f)) continue;
            for (int k = 0; k < 3; ++k) idx.push_back(mesh.idx[t + k]);
        }
        r.leaf_count = UINT(idx.size()) - r.leaf_first;
        TreeRtInfo& ti = info[m];
        ti.vbase = r.vbase;
        ti.ibase_bark = r.bark_first;
        ti.ibase_leaf = r.leaf_first;
        const size_t species = m / kTreeVariants;
        if (species < looks.size()) {
            ti.narrow = looks[species].form == TreeForm::Weeping ? 1u : 0u;
            ti.bark[3] = crown_volume(models[m], looks[species]).sigma;   // the crown's extinction
            for (int k = 0; k < 3; ++k) {
                ti.bark[k] = srgb_lin(looks[species].bark[k]);
                ti.leaf[k] = srgb_lin(looks[species].leaf[k]);
            }
        }
    }
    // Rocks (layer 9): each model's far level after the trees, one opaque
    // geometry, its info at kRockInfoBase + model so a hit finds it.
    struct RockRange { UINT vbase, vcount, first, count; };
    std::vector<RockRange> rranges;
    if (rock_meshes_) {
        info.resize(kRockInfoBase + rock_meshes_->models.size());
        for (size_t m = 0; m < rock_meshes_->models.size(); ++m) {
            const RockModel& rm = rock_meshes_->models[m];
            RockRange rr;
            rr.vbase = UINT(pos.size() / 3);
            rr.vcount = rm.vcount[1];
            for (UINT i = 0; i < rm.vcount[1]; ++i) {
                const RockVertex& v = rock_meshes_->v[rm.vfirst[1] + i];
                for (int k = 0; k < 3; ++k) pos.push_back(v.pos[k]);
                for (int k = 0; k < 4; ++k) attr.push_back(0.0f);
            }
            rr.first = UINT(idx.size());
            for (UINT i = 0; i < rm.count[1]; ++i) idx.push_back(rock_meshes_->idx[rm.first[1] + i] - rm.vfirst[1]);
            rr.count = rm.count[1];
            TreeRtInfo& ti = info[kRockInfoBase + m];
            ti = TreeRtInfo();
            ti.vbase = rr.vbase;
            ti.ibase_bark = rr.first;
            ti.ibase_leaf = rr.first;
            rranges.push_back(rr);
        }
    }
    // Each model's crown as an ellipsoid: from where the crown starts to the
    // top, as wide as the leaves reach. A tree with no leaves has none.
    crowns_by_model_.assign(models.size(), Crown());
    for (size_t m = 0; m < models.size(); ++m) {
        const size_t species = m / kTreeVariants;
        Crown& c = crowns_by_model_[m];
        c.cy = c.rh = c.rv = 0.0f;
        if (species >= looks.size()) continue;
        const CrownVolume v = crown_volume(models[m], looks[species]);
        c.cy = v.cy;
        c.rh = v.rh;
        c.rv = v.rv;
    }
    if (pos.empty() || idx.empty()) return false;
    auto upload = [&](const void* src, UINT64 bytes, ComPtr<ID3D12Resource>& out) -> bool {
        if (!make_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, out)) return false;
        void* p = nullptr;
        out->Map(0, nullptr, &p);
        std::memcpy(p, src, size_t(bytes));
        out->Unmap(0, nullptr);
        return true;
    };
    if (!upload(pos.data(), pos.size() * 4, tree_pos_) || !upload(idx.data(), idx.size() * 4, tree_idx_) ||
        !upload(info.data(), info.size() * sizeof(TreeRtInfo), tree_info_) ||
        !upload(attr.data(), attr.size() * 4, tree_attr_)) return false;

    // One BLAS per model, each with its own scratch: forty-odd, once.
    tree_blas_.assign(models.size(), ComPtr<ID3D12Resource>());
    std::vector<ComPtr<ID3D12Resource> > scratch(models.size() + 1 + rranges.size());
    // The crowns' one BLAS: a unit AABB; gi_trace intersects the sphere in it.
    // OPAQUE, so a ray that culls non-opaque geometry -- the sky ray, which
    // takes crowns as a volume and so skips the cards -- still meets it.
    {
        const D3D12_RAYTRACING_AABB box = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        if (!upload(&box, sizeof box, crown_aabb_)) return false;
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.AABBs.AABBCount = 1;
        g.AABBs.AABBs.StartAddress = crown_aabb_->GetGPUVirtualAddress();
        g.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        ComPtr<ID3D12Resource>& cs = scratch[models.size()];
        if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, crown_blas_) ||
            !make_buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         true, cs)) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = crown_blas_->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = cs->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        stats_.blas_bytes += pi.ResultDataMaxSizeInBytes;
    }
    for (size_t m = 0; m < models.size(); ++m) {
        const Range& r = ranges[m];
        D3D12_RAYTRACING_GEOMETRY_DESC g[2] = {};
        UINT n = 0;
        for (int part = 0; part < 2; ++part) {
            const UINT first = part == 0 ? r.bark_first : r.leaf_first;
            const UINT count = part == 0 ? r.bark_count : r.leaf_count;
            if (count < 3) continue;
            D3D12_RAYTRACING_GEOMETRY_DESC& d = g[n++];
            d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            // Leaves are NON-opaque: each card a ray meets is alpha-tested
            // against the raster's own leaf pattern (gi_trace.hlsli), so light
            // comes through the gaps the eye sees.
            d.Flags = part == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            d.Triangles.VertexBuffer.StartAddress = tree_pos_->GetGPUVirtualAddress() + UINT64(r.vbase) * 12;
            d.Triangles.VertexBuffer.StrideInBytes = 12;
            d.Triangles.VertexCount = r.vcount;
            d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            d.Triangles.IndexBuffer = tree_idx_->GetGPUVirtualAddress() + UINT64(first) * 4;
            d.Triangles.IndexCount = count;
        }
        // gi_trace reads the geometry index as 0 bark, 1 leaves: a model with
        // no bark would shift its leaves to 0. Every model has a trunk.
        if (n == 0) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = n;
        in.pGeometryDescs = g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, tree_blas_[m]) ||
            !make_buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         true, scratch[m])) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = tree_blas_[m]->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = scratch[m]->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        stats_.blas_bytes += pi.ResultDataMaxSizeInBytes;
        ++stats_.tree_blas;
    }
    // Leaf cutouts (leaf_cutout.h): each model's LOD 1 cards cut along the
    // leaf pattern into opaque triangles, unindexed, one BLAS a model.
    cutout_blas_.clear();
    std::vector<ComPtr<ID3D12Resource> > cut_scratch;
    if (cutouts_) {
        const auto t0 = std::chrono::steady_clock::now();
        // Every model on its own core (~1 s on one), then one buffer.
        std::vector<std::vector<float> > per(models.size());
        std::vector<LeafCutoutStats> pst(models.size());
        {
            std::atomic<size_t> next(0);
            auto work = [&]() {
                for (size_t m; (m = next.fetch_add(1)) < models.size();) {
                    const size_t species = m / kTreeVariants;
                    const bool narrow = species < looks.size() && looks[species].form == TreeForm::Weeping;
                    leaf_cutouts(models[m].lod[1], narrow, per[m], pst[m]);
                }
            };
            const unsigned nt = (std::max)(1u, (std::min)(std::thread::hardware_concurrency(), 16u));
            std::vector<std::thread> pool;
            for (unsigned k = 1; k < nt; ++k) pool.emplace_back(work);
            work();
            for (size_t k = 0; k < pool.size(); ++k) pool[k].join();
        }
        std::vector<float> cpos;
        std::vector<std::pair<UINT, UINT> > crange(models.size());   // first vertex, vertex count
        for (size_t m = 0; m < models.size(); ++m) {
            crange[m].first = UINT(cpos.size() / 3);
            cpos.insert(cpos.end(), per[m].begin(), per[m].end());
            crange[m].second = UINT(cpos.size() / 3) - crange[m].first;
            stats_.cutout_cards += pst[m].cards;
            stats_.cutout_tris += pst[m].triangles;
            stats_.cutout_needle_tris += pst[m].needle_tris;
        }
        stats_.cutout_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!cpos.empty() && upload(cpos.data(), cpos.size() * 4, cutout_pos_)) {
            cutout_blas_.assign(models.size(), ComPtr<ID3D12Resource>());
            cut_scratch.resize(models.size());
            for (size_t m = 0; m < models.size(); ++m) {
                // The model's bark too, so the shadow ray never enters the
                // tree's own BLAS: it would walk every card's BVH nodes there
                // only to cull them, and the first build, cutouts alone beside
                // the cards, saved a tenth of the trace where culling the cards
                // outright had promised half.
                const Range& r = ranges[m];
                D3D12_RAYTRACING_GEOMETRY_DESC g[2] = {};
                UINT ng = 0;
                if (r.bark_count >= 3) {
                    D3D12_RAYTRACING_GEOMETRY_DESC& d = g[ng++];
                    d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                    d.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                    d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                    d.Triangles.VertexBuffer.StartAddress = tree_pos_->GetGPUVirtualAddress() + UINT64(r.vbase) * 12;
                    d.Triangles.VertexBuffer.StrideInBytes = 12;
                    d.Triangles.VertexCount = r.vcount;
                    d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                    d.Triangles.IndexBuffer = tree_idx_->GetGPUVirtualAddress() + UINT64(r.bark_first) * 4;
                    d.Triangles.IndexCount = r.bark_count;
                }
                if (crange[m].second >= 3) {
                    D3D12_RAYTRACING_GEOMETRY_DESC& d = g[ng++];
                    d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                    d.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                    d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                    d.Triangles.VertexBuffer.StartAddress = cutout_pos_->GetGPUVirtualAddress() + UINT64(crange[m].first) * 12;
                    d.Triangles.VertexBuffer.StrideInBytes = 12;
                    d.Triangles.VertexCount = crange[m].second;
                    d.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
                }
                if (ng == 0) continue;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
                in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                in.NumDescs = ng;
                in.pGeometryDescs = g;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
                dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
                if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, cutout_blas_[m]) ||
                    !make_buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 true, cut_scratch[m])) {
                    // All or none: the shadow ray culls the cards only if
                    // every model has its cutouts. Those built are in this
                    // list's builds, so they wait out the GPU like scratch.
                    for (size_t k = 0; k < cutout_blas_.size(); ++k)
                        if (cutout_blas_[k]) graveyard_.push_back(std::make_pair(tick_, cutout_blas_[k]));
                    cutout_blas_.clear();
                    break;
                }
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
                bd.Inputs = in;
                bd.DestAccelerationStructureData = cutout_blas_[m]->GetGPUVirtualAddress();
                bd.ScratchAccelerationStructureData = cut_scratch[m]->GetGPUVirtualAddress();
                cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
                stats_.blas_bytes += pi.ResultDataMaxSizeInBytes;
                stats_.cutout_bytes += pi.ResultDataMaxSizeInBytes;
            }
        }
    }
    for (size_t m = 0; m < cut_scratch.size(); ++m)
        if (cut_scratch[m]) graveyard_.push_back(std::make_pair(tick_, cut_scratch[m]));
    // The cutouts' vertices are read by the builds alone (nothing traces
    // their attributes), so they go once the builds are done.
    if (cutout_pos_) {
        graveyard_.push_back(std::make_pair(tick_, cutout_pos_));
        cutout_pos_.Reset();
    }

    rock_blas_.assign(rranges.size(), ComPtr<ID3D12Resource>());
    for (size_t m = 0; m < rranges.size(); ++m) {
        const RockRange& rr = rranges[m];
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexBuffer.StartAddress = tree_pos_->GetGPUVirtualAddress() + UINT64(rr.vbase) * 12;
        g.Triangles.VertexBuffer.StrideInBytes = 12;
        g.Triangles.VertexCount = rr.vcount;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexBuffer = tree_idx_->GetGPUVirtualAddress() + UINT64(rr.first) * 4;
        g.Triangles.IndexCount = rr.count;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        ComPtr<ID3D12Resource>& sc = scratch[models.size() + 1 + m];
        if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, rock_blas_[m]) ||
            !make_buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         true, sc)) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = rock_blas_[m]->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = sc->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        stats_.blas_bytes += pi.ResultDataMaxSizeInBytes;
    }
    // The scratch must outlive the GPU's use of it: into the graveyard,
    // stamped NOW. It was stamped 0 for "not seen yet" once, and the release
    // loop, which runs first, read 0 as ancient and freed it the next frame
    // while the build was still running: a use-after-free that hung the GPU
    // (DXGI_ERROR_DEVICE_HUNG), found by switching the parts off in turn.
    for (size_t m = 0; m < scratch.size(); ++m)
        if (scratch[m]) graveyard_.push_back(std::make_pair(tick_, scratch[m]));
    return true;
}

void RtScene::record(ID3D12GraphicsCommandList* cmd, UINT frame,
                     const Streamer& streamer, ChunkMesher& mesher,
                     const std::vector<TreeModel>& models, const std::vector<TreeLook>& looks,
                     const std::vector<TreeGpuInstance>& trees, const float cam[3], float tree_range) {
    if (!ready_) return;
    const unsigned long long frame_index = ++tick_;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cl)))) return;

    // Release what no frame in flight can still be reading.
    for (size_t i = 0; i < graveyard_.size();) {
        if (graveyard_[i].first + 4 <= frame_index) {
            graveyard_[i] = graveyard_.back();
            graveyard_.pop_back();
        } else {
            ++i;
        }
    }

    if (!(debug_ & 16u) && !trees_built_ && !models.empty()) trees_built_ = build_trees(cl.Get(), models, looks) || true;

    // --- terrain BLAS that are due: meshed two or more frames ago, the
    // counter copy behind them waited out.
    const D3D12_RESOURCE_STATES ib = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    const D3D12_RESOURCE_STATES ibr = D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER tb = {};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = mesher.indices();
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = ib;
    tb.Transition.StateAfter = ibr;
    cmd->ResourceBarrier(1, &tb);

    stats_.built_this_frame = 0;
    UINT64 scratch_used = 0;
    const UINT64 vb = mesher.vertices()->GetGPUVirtualAddress();
    const UINT64 ixb = mesher.indices()->GetGPUVirtualAddress();
    while (!(debug_ & 2u) && !pending_.empty() && stats_.built_this_frame < kBuildsPerFrame) {
        const Pending p = pending_.front();
        // Three ticks, not two: a chunk is noted BEFORE the tick its frame's
        // record() takes, so "two on" was one frame on -- before that frame's
        // counter copy landed -- and read the slot's old zero. Every chunk
        // meshed that way was judged empty for good: 834 of 858 at spawn.
        if (p.frame + 3 > frame_index) break;
        pending_.pop_front();
        if (p.slot < kSlots && slots_[p.slot].queued == p.key) slots_[p.slot].queued.lod = -1;
        UINT slot = 0;
        if (p.slot >= kSlots || p.frame < latest_mesh_[p.slot]) { ++stats_.dropped_remeshed; continue; }   // a newer entry builds it
        if (!streamer.slot_of(p.key, slot) || slot != p.slot) { ++stats_.dropped_moved; continue; }        // retired or moved on
        UINT nv = 0, ni = 0;
        mesher.slot_counts(slot, nv, ni);
        ni = (std::min)(ni, kSlotIndices);
        ni -= ni % 3;
        SlotBlas& sb = slots_[slot];
        if (sb.blas) {
            stats_.blas_bytes -= sb.bytes;
            --stats_.terrain_blas;
            graveyard_.push_back(std::make_pair(frame_index, sb.blas));
            sb.blas.Reset();
        }
        sb.key = p.key;
        sb.bytes = 0;
        if (ni < 3) { ++stats_.empty; continue; }   // meshed to nothing: nothing to trace
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexBuffer.StartAddress = vb;
        g.Triangles.VertexBuffer.StrideInBytes = sizeof(MeshVertex);
        g.Triangles.VertexCount = kSlots * kSlotVertices;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexBuffer = ixb + UINT64(slot) * kSlotIndices * 4;
        g.Triangles.IndexCount = ni;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        const UINT64 need = (pi.ScratchDataSizeInBytes + 255) & ~UINT64(255);
        if (scratch_used + need > scratch_size_) { pending_.push_front(p); break; }
        if (!make_buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, sb.blas)) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = sb.blas->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + scratch_used;
        cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        scratch_used += need;
        sb.bytes = pi.ResultDataMaxSizeInBytes;
        stats_.blas_bytes += sb.bytes;
        ++stats_.terrain_blas;
        ++stats_.built_this_frame;
        ++stats_.built_total;
    }
    stats_.pending = UINT(pending_.size());
    uav_all(cmd);
    tb.Transition.StateBefore = ibr;
    tb.Transition.StateAfter = ib;
    cmd->ResourceBarrier(1, &tb);

    // --- the TLAS: drawn chunks with a BLAS for their own key, standing trees
    D3D12_RAYTRACING_INSTANCE_DESC* inst = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(inst_cpu_[frame & 1u]);
    UINT n = 0;
    std::vector<std::pair<ChunkKey, UINT> > drawn;
    streamer.drawn(drawn);
    stats_.instances_terrain = 0;
    stats_.drawn_without_blas = 0;
    for (size_t i = 0; i < drawn.size() && n < max_instances_; ++i) {
        const ChunkKey& k = drawn[i].first;
        const UINT slot = drawn[i].second;
        if (k.lod > kMaxLod || slot >= kSlots) continue;
        SlotBlas& sb = slots_[slot];
        if (!(sb.key == k)) {
            // Drawn without a verdict for this key: queue it. Chunks meshed
            // where no frame was recorded -- a headless mode's residency
            // fill, a scene switched on late -- come in this way, so nothing
            // depends on having seen the meshing happen.
            ++stats_.drawn_without_blas;
            if (!(sb.queued == k)) {
                Pending q;
                q.key = k;
                q.slot = slot;
                q.frame = frame_index;
                pending_.push_back(q);
                sb.queued = k;
                ++stats_.noted;
            }
            continue;
        }
        if (!sb.blas) continue;   // known empty
        sb.seen = frame_index;
        D3D12_RAYTRACING_INSTANCE_DESC& d = inst[n++];
        std::memset(&d, 0, sizeof d);
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;
        d.InstanceID = slot;
        d.InstanceMask = kMaskTerrain;
        d.AccelerationStructure = sb.blas->GetGPUVirtualAddress();
        ++stats_.instances_terrain;
    }
    stats_.instances_trees = 0;
    stats_.instances_crowns = 0;
    stats_.instances_cutouts = 0;
    const float r2 = tree_range * tree_range;
    const bool cut = has_cutouts();
    // Trees, their cutouts and their crowns share one cap. A tree goes in
    // only with room for its cutouts: the shadow ray culls the cards, so a
    // tree without them would cast no leaf shadow.
    auto tree_room = [&](UINT need) {
        return n + need <= max_instances_ &&
               stats_.instances_trees + stats_.instances_crowns + stats_.instances_cutouts + need <= kMaxTreeInstances;
    };
    for (size_t i = 0; !(debug_ & 4u) && i < trees.size() && tree_room(cut ? 2u : 1u); ++i) {
        const TreeGpuInstance& t = trees[i];
        if (t.mode != 0 || t.model >= tree_blas_.size() || !tree_blas_[t.model]) continue;
        const float dx = t.pos[0] - cam[0], dz = t.pos[2] - cam[2];
        if (dx * dx + dz * dz > r2) continue;
        // Rotation from the quaternion, times the scale, then the position.
        const float x = t.rot[0], y = t.rot[1], z = t.rot[2], w = t.rot[3], s = t.scale;
        const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                               {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                               {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        // Built here and copied in: `inst` is a mapped upload heap, which is
        // write-combined, and the cutouts' copy must not read it back.
        D3D12_RAYTRACING_INSTANCE_DESC d;
        std::memset(&d, 0, sizeof d);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) d.Transform[r][c] = R[r][c] * s;
            d.Transform[r][3] = t.pos[r];
        }
        // The wind's lean, as trees.hlsl bends the mesh (tree_sway.h): the
        // crown's offset at the top, applied as a SHEAR -- each point moved by
        // TS_SHEAR_FIT x that offset x (its model height over the top), the
        // base fixed -- since an instance transform is affine and the mesh's
        // bend goes as h^2. Added to the model-y column, which is what
        // multiplies a point's own height. Without it every traced shadow
        // stood still under a swaying crown: ~0.7 m off in a gale.
        float lean_y[3] = {0.0f, 0.0f, 0.0f};
        if (sway_.on) {
            const float top = models[t.model].top > 1.0f ? models[t.model].top : 1.0f;
            const size_t species = t.model / kTreeVariants;
            const float bend = species < looks.size() ? looks[species].bend : 1.0f;
            float u_here;
            const ts_float3 o = ts_crown_offset(top, t.id, t.pos[0], t.pos[2], sway_.time,
                                                sway_.wind_v[0], sway_.wind_v[1], sway_.wind_v[2], sway_.wind_v[3],
                                                sway_.drift[0], sway_.drift[1], sway_.mult, bend, u_here);
            lean_y[0] = TS_SHEAR_FIT * o.x / top;
            lean_y[1] = TS_SHEAR_FIT * o.y / top;
            lean_y[2] = TS_SHEAR_FIT * o.z / top;
            for (int r = 0; r < 3; ++r) d.Transform[r][1] += lean_y[r];
        }
        d.InstanceID = kTreeFlag | t.model;
        d.InstanceMask = kMaskTrees;
        d.AccelerationStructure = tree_blas_[t.model]->GetGPUVirtualAddress();
        std::memcpy(&inst[n++], &d, sizeof d);
        ++stats_.instances_trees;
        // Its leaf cutouts, placed as the tree is, for the shadow ray alone.
        if (cut && t.model < cutout_blas_.size() && cutout_blas_[t.model]) {
            d.InstanceMask = kMaskCutouts;
            d.AccelerationStructure = cutout_blas_[t.model]->GetGPUVirtualAddress();
            std::memcpy(&inst[n++], &d, sizeof d);
            ++stats_.instances_cutouts;
        }
        // Its crown: the unit sphere scaled to the ellipsoid, then placed as
        // the tree is. Counts against the same cap as the trees.
        if (!crowns_ || !crown_blas_ || t.model >= crowns_by_model_.size() || !tree_room(1u)) continue;
        const Crown& c = crowns_by_model_[t.model];
        if (c.rh <= 0.0f) continue;
        D3D12_RAYTRACING_INSTANCE_DESC& e = inst[n++];
        std::memset(&e, 0, sizeof e);
        const float rad[3] = {c.rh, c.rv, c.rh};
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) e.Transform[r][k] = R[r][k] * s * rad[k];
            e.Transform[r][3] = t.pos[r] + R[r][1] * s * c.cy;
            // The same shear, on the ellipsoid: its points sit at model height
            // cy + rv * (unit y), so the centre moves and the vertical axis tilts.
            e.Transform[r][1] += lean_y[r] * c.rv;
            e.Transform[r][3] += lean_y[r] * c.cy;
        }
        e.InstanceID = kCrownFlag | t.model;
        e.InstanceMask = kMaskCrowns;
        e.AccelerationStructure = crown_blas_->GetGPUVirtualAddress();
        ++stats_.instances_crowns;
    }
    // Rocks within range, in the trees' mask: opaque surfaces for every ray.
    stats_.instances_rocks = 0;
    if (rocks_ && !rock_blas_.empty()) {
        const float rr2 = rock_range_ * rock_range_;
        for (size_t i = 0; i < rocks_->size() && n < max_instances_; ++i) {
            const worldgen::RockInstance& r = (*rocks_)[i];
            const float dx = r.x - cam[0], dz = r.z - cam[2];
            if (dx * dx + dz * dz > rr2) continue;
            const uint32_t m = rock_model(r.kind, r.variant);
            if (m >= rock_blas_.size() || !rock_blas_[m]) continue;
            float q[4];
            rock_rotation(r, q);
            const float x = q[0], y = q[1], z = q[2], w = q[3], s = r.size;
            const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                   {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                   {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
            D3D12_RAYTRACING_INSTANCE_DESC& d = inst[n++];
            std::memset(&d, 0, sizeof d);
            const float pos[3] = {r.x, r.y, r.z};
            for (int a = 0; a < 3; ++a) {
                for (int c = 0; c < 3; ++c) d.Transform[a][c] = R[a][c] * s;
                d.Transform[a][3] = pos[a];
            }
            // The biome's rock colour as sqrt(linear) in RGB565.
            UINT rgb = 0;
            if (size_t(r.biome) * 3 + 2 < rock_albedo_.size()) {
                const float* c = &rock_albedo_[size_t(r.biome) * 3];
                const UINT cr = UINT(std::sqrt((std::min)(c[0], 1.0f)) * 31.0f + 0.5f);
                const UINT cg = UINT(std::sqrt((std::min)(c[1], 1.0f)) * 63.0f + 0.5f);
                const UINT cb = UINT(std::sqrt((std::min)(c[2], 1.0f)) * 31.0f + 0.5f);
                rgb = (cr << 11) | (cg << 5) | cb;
            }
            d.InstanceID = kRockFlag | (rgb << 4) | m;
            d.InstanceMask = kMaskTrees | kMaskCutouts;   // the key-light shadow's casters too
            d.AccelerationStructure = rock_blas_[m]->GetGPUVirtualAddress();
            ++stats_.instances_rocks;
        }
    }
    // Slots nobody has drawn for a while give their BLAS back.
    for (UINT s = 0; s < kSlots; ++s) {
        SlotBlas& sb = slots_[s];
        if (sb.blas && sb.seen + 600 < frame_index && latest_mesh_[s] + 600 < frame_index) {
            stats_.blas_bytes -= sb.bytes;
            --stats_.terrain_blas;
            graveyard_.push_back(std::make_pair(frame_index, sb.blas));
            sb.blas.Reset();
            sb.key.lod = -1;
        }
    }

    tlas_cur_ = frame & 1u;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = n;
    in.InstanceDescs = inst_up_[frame & 1u]->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs = in;
    bd.DestAccelerationStructureData = tlas_[tlas_cur_]->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = tlas_scratch_->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uav_all(cmd);
}

}  // namespace engine
