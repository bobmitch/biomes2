#ifndef NOMINMAX
#define NOMINMAX   // windows.h, via d3d12.h, would turn worldgen's std::min into a macro
#endif
#include "engine/render/rt_scene.h"
#include "engine/meshing/chunk_mesher.h"

#include <algorithm>
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

// Matches TreeRtInfo in gi_trace.hlsli.
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
            for (int k = 0; k < 3; ++k) {
                ti.bark[k] = srgb_lin(looks[species].bark[k]);
                ti.leaf[k] = srgb_lin(looks[species].leaf[k]);
            }
        }
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
    std::vector<ComPtr<ID3D12Resource> > scratch(models.size());
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
        d.InstanceMask = 0x1;
        d.AccelerationStructure = sb.blas->GetGPUVirtualAddress();
        ++stats_.instances_terrain;
    }
    stats_.instances_trees = 0;
    const float r2 = tree_range * tree_range;
    for (size_t i = 0; !(debug_ & 4u) && i < trees.size() && n < max_instances_ && stats_.instances_trees < kMaxTreeInstances; ++i) {
        const TreeGpuInstance& t = trees[i];
        if (t.mode != 0 || t.model >= tree_blas_.size() || !tree_blas_[t.model]) continue;
        const float dx = t.pos[0] - cam[0], dz = t.pos[2] - cam[2];
        if (dx * dx + dz * dz > r2) continue;
        // Rotation from the quaternion, times the scale, then the position.
        const float x = t.rot[0], y = t.rot[1], z = t.rot[2], w = t.rot[3], s = t.scale;
        const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                               {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                               {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        D3D12_RAYTRACING_INSTANCE_DESC& d = inst[n++];
        std::memset(&d, 0, sizeof d);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) d.Transform[r][c] = R[r][c] * s;
            d.Transform[r][3] = t.pos[r];
        }
        d.InstanceID = kTreeFlag | t.model;
        d.InstanceMask = 0x2;
        d.AccelerationStructure = tree_blas_[t.model]->GetGPUVirtualAddress();
        ++stats_.instances_trees;
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
