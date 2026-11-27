#include "engine/ui/ui_render_d3d12.h"
#include "engine/render/shader_compile.h"

#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>

#include <cstring>

namespace engine {
namespace ui {

namespace {

const UINT kHeapSlots = 256;   // textures: the font atlases, white, images
const uint64_t kRetireFrames = 4;  // > frames in flight

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = t;
    return hp;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return rd;
}

void set_identity(float m[16]) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

}  // namespace

bool RenderD3D12::init(ID3D12Device* device, DXGI_FORMAT target_format, std::string& error) {
    device_ = device;
    set_identity(transform_);

    // Root: 20 constants (transform, translate, inverse size) and one SRV.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 20;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &smp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        error = "ui root signature: ";
        if (err) error += static_cast<const char*>(err->GetBufferPointer());
        return false;
    }
    if (FAILED(device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
        error = "ui CreateRootSignature";
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader("engine/ui/ui.hlsl", "vs_ui", "vs_5_1", vs, error)) return false;
    if (!compile_shader("engine/ui/ui.hlsl", "ps_ui", "ps_5_1", ps, error)) return false;

    const D3D12_INPUT_ELEMENT_DESC layout[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(Rml::Vertex) == 20, "the input layout above assumes RmlUi's 20-byte vertex");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {layout, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = target_format;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = 0xffffffffu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    // Premultiplied alpha, as RmlUi hands it over.
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)))) {
        error = "ui CreateGraphicsPipelineState";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
        error = "ui descriptor heap";
        return false;
    }
    heap_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = kHeapSlots; i-- > 0;) free_slots_.push_back(i);

    // Untextured geometry samples white, so one pipeline draws everything.
    const uint8_t white[4] = {255, 255, 255, 255};
    white_ = make_texture(white, 1, 1);
    if (!white_) {
        error = "ui white texture";
        return false;
    }
    return true;
}

void RenderD3D12::shutdown() {
    pending_uploads_.clear();
    delete white_;
    white_ = nullptr;
    retired_.clear();
    pso_.Reset();
    root_.Reset();
    heap_.Reset();
}

void RenderD3D12::retire(ComPtr<ID3D12Resource> r) {
    if (r) retired_.push_back({frame_, std::move(r)});
}

RenderD3D12::Texture* RenderD3D12::make_texture(const uint8_t* rgba, int w, int h) {
    if (free_slots_.empty()) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ui: out of texture slots (%u)", kHeapSlots);
        return nullptr;
    }
    Texture* t = new Texture();
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = UINT64(w);
    td.Height = UINT(h);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES dh = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&t->tex)))) {
        delete t;
        return nullptr;
    }
    UINT64 total = 0;
    UINT rows = 0;
    UINT64 row_bytes = 0;
    device_->GetCopyableFootprints(&td, 0, 1, 0, &t->footprint, &rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES uh = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bd = buffer_desc(total);
    if (FAILED(device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&t->upload)))) {
        delete t;
        return nullptr;
    }
    uint8_t* dst = nullptr;
    D3D12_RANGE none = {0, 0};
    t->upload->Map(0, &none, reinterpret_cast<void**>(&dst));
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + t->footprint.Offset + size_t(y) * t->footprint.Footprint.RowPitch,
                    rgba + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    t->upload->Unmap(0, nullptr);

    t->slot = free_slots_.back();
    free_slots_.pop_back();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h_cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    h_cpu.ptr += SIZE_T(t->slot) * heap_stride_;
    device_->CreateShaderResourceView(t->tex.Get(), &sv, h_cpu);

    if (cmd_) record_upload(t);
    else pending_uploads_.push_back(t);
    return t;
}

void RenderD3D12::record_upload(Texture* t) {
    if (t->uploaded) return;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = t->tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = t->upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = t->footprint;
    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = t->tex.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_->ResourceBarrier(1, &br);
    t->uploaded = true;
    retire(std::move(t->upload));
}

void RenderD3D12::begin_frame(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width,
                              UINT height) {
    ++frame_;
    // Free what no frame in flight can still be reading.
    size_t keep = 0;
    for (size_t i = 0; i < retired_.size(); ++i)
        if (retired_[i].frame + kRetireFrames > frame_) retired_[keep++] = std::move(retired_[i]);
    retired_.resize(keep);

    cmd_ = cmd;
    width_ = width ? width : 1;
    height_ = height ? height : 1;
    for (Texture* t : pending_uploads_) record_upload(t);
    pending_uploads_.clear();

    D3D12_VIEWPORT vp = {0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
    cmd_->RSSetViewports(1, &vp);
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    cmd_->SetGraphicsRootSignature(root_.Get());
    cmd_->SetPipelineState(pso_.Get());
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    scissor_on_ = false;
    set_identity(transform_);
    apply_scissor();
}

void RenderD3D12::end_frame() { cmd_ = nullptr; }

Rml::CompiledGeometryHandle RenderD3D12::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                         Rml::Span<const int> indices) {
    if (vertices.empty() || indices.empty()) return 0;
    const UINT64 vb = UINT64(vertices.size()) * sizeof(Rml::Vertex);
    const UINT64 ib = UINT64(indices.size()) * sizeof(int);
    Geometry* g = new Geometry();
    D3D12_HEAP_PROPERTIES uh = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bd = buffer_desc(vb + ib);
    if (FAILED(device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&g->buf)))) {
        delete g;
        return 0;
    }
    uint8_t* p = nullptr;
    D3D12_RANGE none = {0, 0};
    g->buf->Map(0, &none, reinterpret_cast<void**>(&p));
    std::memcpy(p, vertices.data(), size_t(vb));
    std::memcpy(p + vb, indices.data(), size_t(ib));
    g->buf->Unmap(0, nullptr);
    const D3D12_GPU_VIRTUAL_ADDRESS va = g->buf->GetGPUVirtualAddress();
    g->vbv.BufferLocation = va;
    g->vbv.SizeInBytes = UINT(vb);
    g->vbv.StrideInBytes = sizeof(Rml::Vertex);
    g->ibv.BufferLocation = va + vb;
    g->ibv.SizeInBytes = UINT(ib);
    g->ibv.Format = DXGI_FORMAT_R32_UINT;
    g->index_count = UINT(indices.size());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RenderD3D12::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                 Rml::TextureHandle texture) {
    Geometry* g = reinterpret_cast<Geometry*>(handle);
    if (!g || !cmd_) return;
    Texture* t = texture ? reinterpret_cast<Texture*>(texture) : white_;
    if (!t->uploaded) record_upload(t);
    float c[20];
    std::memcpy(c, transform_, sizeof transform_);
    c[16] = translation.x;
    c[17] = translation.y;
    c[18] = 1.0f / float(width_);
    c[19] = 1.0f / float(height_);
    cmd_->SetGraphicsRoot32BitConstants(0, 20, c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += UINT64(t->slot) * heap_stride_;
    cmd_->SetGraphicsRootDescriptorTable(1, gh);
    cmd_->IASetVertexBuffers(0, 1, &g->vbv);
    cmd_->IASetIndexBuffer(&g->ibv);
    cmd_->DrawIndexedInstanced(g->index_count, 1, 0, 0, 0);
}

void RenderD3D12::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    Geometry* g = reinterpret_cast<Geometry*>(handle);
    if (!g) return;
    retire(std::move(g->buf));
    delete g;
}

Rml::TextureHandle RenderD3D12::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    // "icon:<name>", wherever the document's path put it in front.
    const size_t at = source.find("icon:");
    if (at != Rml::String::npos && image_source_) {
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        if (image_source_(source.substr(at + 5), rgba, w, h) && w > 0 && h > 0 &&
            rgba.size() >= size_t(w) * size_t(h) * 4) {
            dimensions = Rml::Vector2i(w, h);
            return reinterpret_cast<Rml::TextureHandle>(make_texture(rgba.data(), w, h));
        }
    }
    // No image decoder yet: the menus are text and boxes.
    dimensions = Rml::Vector2i(0, 0);
    Rml::Log::Message(Rml::Log::LT_WARNING, "ui: image files are not supported yet (%s)", source.c_str());
    return 0;
}

Rml::TextureHandle RenderD3D12::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0 ||
        source.size() < size_t(dimensions.x) * size_t(dimensions.y) * 4)
        return 0;
    return reinterpret_cast<Rml::TextureHandle>(make_texture(source.data(), dimensions.x, dimensions.y));
}

void RenderD3D12::ReleaseTexture(Rml::TextureHandle handle) {
    Texture* t = reinterpret_cast<Texture*>(handle);
    if (!t || t == white_) return;
    for (size_t i = 0; i < pending_uploads_.size(); ++i) {
        if (pending_uploads_[i] == t) {
            pending_uploads_.erase(pending_uploads_.begin() + long(i));
            break;
        }
    }
    retire(std::move(t->tex));
    retire(std::move(t->upload));
    // The slot's descriptor may still be read by a frame in flight; the
    // heap is large and this is rare, so it is simply not reused until the
    // resource itself is gone. Approximated by returning it at the back of
    // the free list, which is popped from the back -- so push to the FRONT.
    free_slots_.insert(free_slots_.begin(), t->slot);
    delete t;
}

void RenderD3D12::apply_scissor() {
    if (!cmd_) return;
    D3D12_RECT r = {0, 0, LONG(width_), LONG(height_)};
    if (scissor_on_) {
        r.left = Rml::Math::Max(0, scissor_.Left());
        r.top = Rml::Math::Max(0, scissor_.Top());
        r.right = Rml::Math::Min(LONG(width_), LONG(scissor_.Right()));
        r.bottom = Rml::Math::Min(LONG(height_), LONG(scissor_.Bottom()));
        if (r.right < r.left) r.right = r.left;
        if (r.bottom < r.top) r.bottom = r.top;
    }
    cmd_->RSSetScissorRects(1, &r);
}

void RenderD3D12::EnableScissorRegion(bool enable) {
    scissor_on_ = enable;
    apply_scissor();
}

void RenderD3D12::SetScissorRegion(Rml::Rectanglei region) {
    scissor_ = region;
    apply_scissor();
}

void RenderD3D12::SetTransform(const Rml::Matrix4f* transform) {
    if (!transform) {
        set_identity(transform_);
        return;
    }
    // Column-major in RmlUi's default build; HLSL's cbuffer float4x4 is
    // column-major too, so the floats go across as they are.
    std::memcpy(transform_, transform->data(), sizeof transform_);
}

}  // namespace ui
}  // namespace engine
