#include "engine/render/renderer.h"
#include "engine/render/shader_compile.h"
#include "engine/field/trivial_field.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace engine {
namespace {

const char* kShaderPath = "engine/render/shaders/trivial_raymarch.hlsl";
const char* kMeshShaderPath = "engine/render/shaders/mesh.hlsl";

const float kNearPlane = 0.2f;
const float kFarPlane  = 2400.0f;

struct MeshConstants {
    float view_proj[16];
    float cam_pos[3];
    float far_plane;
    float wire;
    float pad1[3];
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

// Row-vector convention throughout: clip = pos * view * proj, and the matrix
// is declared row_major in the shader. HLSL packs cbuffer matrices as
// column-major by default, which transposes silently and puts the geometry
// somewhere behind the camera -- stating it in both places is cheaper than
// debugging it once.
void mat_mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
    }
}

void build_view_proj(const ViewConstants& v, float out[16]) {
    const float* R = v.cam_right;
    const float* U = v.cam_up;
    const float* F = v.cam_fwd;
    const float* P = v.cam_pos;
    const float dr = R[0] * P[0] + R[1] * P[1] + R[2] * P[2];
    const float du = U[0] * P[0] + U[1] * P[1] + U[2] * P[2];
    const float df = F[0] * P[0] + F[1] * P[1] + F[2] * P[2];
    const float view[16] = {
        R[0], U[0], F[0], 0.0f,
        R[1], U[1], F[1], 0.0f,
        R[2], U[2], F[2], 0.0f,
        -dr,  -du,  -df,  1.0f
    };
    const float h = 1.0f / v.tan_half_fov;
    const float w = h / (v.aspect > 0.0f ? v.aspect : 1.0f);
    const float n = kNearPlane, f = kFarPlane;
    const float proj[16] = {
        w,    0.0f, 0.0f,             0.0f,
        0.0f, h,    0.0f,             0.0f,
        0.0f, 0.0f, f / (f - n),      1.0f,
        0.0f, 0.0f, -n * f / (f - n), 0.0f
    };
    mat_mul(view, proj, out);
}

// 24-bit BMP, bottom-up. Same container tools/validate writes, for the same
// reason: no image library, and anything can open it.
bool write_bmp(const char* path, int w, int h, const unsigned char* bgr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int row = w * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * h;
    const int off = 54;
    unsigned char hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    const int size = off + img;
    std::memcpy(hdr + 2, &size, 4);
    std::memcpy(hdr + 10, &off, 4);
    const int ih = 40;
    std::memcpy(hdr + 14, &ih, 4);
    std::memcpy(hdr + 18, &w, 4);
    std::memcpy(hdr + 22, &h, 4);
    const short planes = 1, bpp = 24;
    std::memcpy(hdr + 26, &planes, 2);
    std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &img, 4);
    std::fwrite(hdr, 1, 54, f);
    const unsigned char zero[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0; --y) {
        std::fwrite(bgr + static_cast<size_t>(y) * row, 1, row, f);
        if (pad) std::fwrite(zero, 1, pad, f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

bool Renderer::create_device(std::string& error, bool debug_layer) {
    UINT factory_flags = 0;
#ifndef _DEBUG
    if (debug_layer)
#endif
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) { error = hr_text("CreateDXGIFactory2", hr); return false; }

    // First adapter that makes a device. Explicitly skipping WARP: a software
    // device would "work" and then quietly miss every performance budget in
    // the design, which is worse than failing here.
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory_->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)))) {
            break;
        }
    }
    if (!device_) {
        error = "no Direct3D 12 hardware adapter at feature level 11_0";
        return false;
    }

    device_.As(&info_queue_);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (FAILED(hr)) { error = hr_text("CreateCommandQueue", hr); return false; }
    return true;
}

bool Renderer::create_swapchain(HWND hwnd, std::string& error) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    width_  = static_cast<UINT>(rc.right - rc.left);
    height_ = static_cast<UINT>(rc.bottom - rc.top);
    if (width_ == 0) width_ = 1;
    if (height_ == 0) height_ = 1;

    // Tearing has to be asked for here as well as at present time, and it is
    // the only way to measure past the refresh rate -- Present(0, 0) on a
    // flip-model swapchain still waits for the vertical blank. The first
    // flight measurement read a median of 16.66 ms, which is the refresh
    // interval to three figures and says nothing about headroom.
    {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory_.As(&f5))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                  &allow, sizeof(allow)))) {
                tearing_ok_ = allow == TRUE;
            }
        }
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrameCount;
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    if (tearing_ok_) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd,
                                                  nullptr, nullptr, &sc1);
    if (FAILED(hr)) { error = hr_text("CreateSwapChainForHwnd", hr); return false; }

    // The engine handles Alt+Enter itself later; DXGI's own handling resizes
    // behind the app's back and invalidates the RTVs without telling it.
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = sc1.As(&swapchain_);
    if (FAILED(hr)) { error = hr_text("IDXGISwapChain3 query", hr); return false; }
    frame_ = swapchain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.NumDescriptors = kFrameCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_));
    if (FAILED(hr)) { error = hr_text("CreateDescriptorHeap", hr); return false; }
    rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool Renderer::create_rtvs(std::string& error) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        const HRESULT hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&targets_[i]));
        if (FAILED(hr)) { error = hr_text("GetBuffer", hr); return false; }
        device_->CreateRenderTargetView(targets_[i].Get(), nullptr, h);
        h.ptr += rtv_stride_;
    }
    return true;
}

bool Renderer::create_depth(std::string& error) {
    if (!dsv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        const HRESULT hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_));
        if (FAILED(hr)) { error = hr_text("CreateDescriptorHeap (dsv)", hr); return false; }
    }
    depth_.Reset();

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width_;
    rd.Height = height_;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;

    const HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &cv, IID_PPV_ARGS(&depth_));
    if (FAILED(hr)) { error = hr_text("CreateCommittedResource (depth)", hr); return false; }

    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depth_.Get(), &dv,
                                    dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool Renderer::create_pipeline(std::string& error) {
    if (!root_) {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = sizeof(ViewConstants) / 4;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, sig_err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &sig, &sig_err);
        if (FAILED(hr)) {
            error = "root signature: ";
            error += sig_err ? static_cast<const char*>(sig_err->GetBufferPointer())
                             : hr_text("D3D12SerializeRootSignature", hr).c_str();
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature", hr); return false; }
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kShaderPath, "vs_main", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kShaderPath, "ps_main", "ps_5_1", ps, error)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root_.Get();
    pd.VS.pShaderBytecode = vs->GetBufferPointer();
    pd.VS.BytecodeLength  = vs->GetBufferSize();
    pd.PS.pShaderBytecode = ps->GetBufferPointer();
    pd.PS.BytecodeLength  = ps->GetBufferSize();
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState", hr); return false; }
    pso_ = pso;
    return true;
}

bool Renderer::create_mesh_pipeline(std::string& error) {
    if (!mesh_root_) {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(MeshConstants) / 4;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].Descriptor.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, sig_err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &sig, &sig_err);
        if (FAILED(hr)) {
            error = "mesh root signature: ";
            error += sig_err ? static_cast<const char*>(sig_err->GetBufferPointer())
                             : hr_text("D3D12SerializeRootSignature", hr).c_str();
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&mesh_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (mesh)", hr); return false; }

        // One indirect argument per slot, so the entire resident world is one
        // ExecuteIndirect. No root argument changes between slots, so the
        // signature needs no root signature of its own.
        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC csd = {};
        csd.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs = &arg;
        hr = device_->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cmd_sig_));
        if (FAILED(hr)) { error = hr_text("CreateCommandSignature", hr); return false; }
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kMeshShaderPath, "vs_main", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kMeshShaderPath, "ps_main", "ps_5_1", ps, error)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = mesh_root_.Get();
    pd.VS.pShaderBytecode = vs->GetBufferPointer();
    pd.VS.BytecodeLength = vs->GetBufferSize();
    pd.PS.pShaderBytecode = ps->GetBufferPointer();
    pd.PS.BytecodeLength = ps->GetBufferSize();
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Back-face culling on purpose. The kernel winds its quads according to
    // which end of the edge is solid, and culling is what makes a winding
    // mistake visible as holes instead of hiding it behind a correct-looking
    // image lit from the wrong side.
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (mesh)", hr); return false; }

    // The same mesh as wireframe, for drawing over the raymarch. Depth off and
    // no culling: the point is to see where the edges land relative to the
    // marched surface, including the far side.
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D12PipelineState> wire;
    hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&wire));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (wire)", hr); return false; }

    if (ready_) wait_for_gpu();
    mesh_pso_ = pso;
    mesh_wire_pso_ = wire;
    return true;
}

bool Renderer::reload_shaders(std::string& error) {
    if (ready_) wait_for_gpu();
    if (!create_pipeline(error)) return false;
    if (!mesher_.reload_shaders(device_.Get(), error)) return false;
    if (!create_mesh_pipeline(error)) return false;
    // Everything resident was built by the old kernel, so start again.
    streamer_.init();
    return true;
}

bool Renderer::init(HWND hwnd, std::string& error, bool debug_layer) {
    if (!create_device(error, debug_layer)) return false;
    if (!create_swapchain(hwnd, error)) return false;
    if (!create_rtvs(error)) return false;

    for (UINT i = 0; i < kFrameCount; ++i) {
        const HRESULT hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (FAILED(hr)) { error = hr_text("CreateCommandAllocator", hr); return false; }
    }

    HRESULT hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            allocators_[frame_].Get(), nullptr,
                                            IID_PPV_ARGS(&cmd_));
    if (FAILED(hr)) { error = hr_text("CreateCommandList", hr); return false; }
    cmd_->Close();

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) { error = hr_text("CreateFence", hr); return false; }
    fence_values_[frame_] = 1;
    fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) { error = "CreateEvent failed"; return false; }

    if (!create_pipeline(error)) return false;
    if (!create_depth(error)) return false;

    UINT64 tsfreq = 0;
    queue_->GetTimestampFrequency(&tsfreq);
    if (!mesher_.init(device_.Get(), tsfreq, error)) return false;
    if (!create_mesh_pipeline(error)) return false;
    streamer_.init();

    // Every slot starts retired, so a slot that has never been meshed draws
    // nothing instead of whatever the allocation happened to contain.
    allocators_[frame_]->Reset();
    cmd_->Reset(allocators_[frame_].Get(), nullptr);
    for (UINT s = 0; s < kSlots; ++s) mesher_.record_retire(cmd_.Get(), s);
    // Leave the pool in the states a frame BEGINS in. Every frame transitions
    // draw -> compute -> draw, so if init left the buffers in UNORDERED_ACCESS
    // the first frame's transition would declare a StateBefore that is not the
    // state they are in. That is undefined behaviour, and on this driver it
    // silently produced an empty screen while the counters showed half a
    // million triangles meshed.
    mesher_.record_to_draw(cmd_.Get());
    cmd_->Close();
    ID3D12CommandList* lists[] = {cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();

    ready_ = true;
    return true;
}

void Renderer::wait_for_gpu() {
    if (!queue_ || !fence_) return;
    const UINT64 v = fence_values_[frame_];
    if (FAILED(queue_->Signal(fence_.Get(), v))) return;
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    ++fence_values_[frame_];
}

// Signal this frame's value, move on, and only block if the frame we are about
// to reuse has not finished. With kFrameCount buffers the CPU stays one frame
// ahead instead of serialising on every present.
void Renderer::next_frame() {
    const UINT64 current = fence_values_[frame_];
    queue_->Signal(fence_.Get(), current);
    frame_ = swapchain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fence_values_[frame_]) {
        fence_->SetEventOnCompletion(fence_values_[frame_], fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    fence_values_[frame_] = current + 1;
}

void Renderer::resize(UINT width, UINT height) {
    if (!ready_ || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    wait_for_gpu();
    for (UINT i = 0; i < kFrameCount; ++i) targets_[i].Reset();

    const HRESULT hr = swapchain_->ResizeBuffers(
        kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
        tearing_ok_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) return;

    width_ = width;
    height_ = height;
    frame_ = swapchain_->GetCurrentBackBufferIndex();

    std::string ignored;
    create_rtvs(ignored);
    create_depth(ignored);
}

void Renderer::record_scene(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &scissor);

    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    MeshConstants mc = {};
    build_view_proj(view, mc.view_proj);
    mc.cam_pos[0] = view.cam_pos[0];
    mc.cam_pos[1] = view.cam_pos[1];
    mc.cam_pos[2] = view.cam_pos[2];
    mc.far_plane = kFarPlane;

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = mesher_.indices()->GetGPUVirtualAddress();
    ibv.SizeInBytes = kSlots * kSlotIndices * 4;
    ibv.Format = DXGI_FORMAT_R32_UINT;

    if (mode_ == Mode::Raymarch || mode_ == Mode::Overlay) {
        cmd_->SetPipelineState(pso_.Get());
        cmd_->SetGraphicsRootSignature(root_.Get());
        cmd_->SetGraphicsRoot32BitConstants(0, sizeof(ViewConstants) / 4, &view, 0);
        cmd_->DrawInstanced(3, 1, 0, 0);
    } else {
        // The mesh does not cover the screen, so the sky is cleared rather
        // than drawn. Same colour the raymarch's horizon fades to, so the two
        // images differ only where the geometry does.
        const float sky[4] = {0.40f, 0.50f, 0.66f, 1.0f};
        cmd_->ClearRenderTargetView(rtv, sky, 0, nullptr);
    }

    if (mode_ == Mode::Mesh || mode_ == Mode::Overlay) {
        const bool wire = (mode_ == Mode::Overlay);
        mc.wire = wire ? 1.0f : 0.0f;
        cmd_->SetPipelineState(wire ? mesh_wire_pso_.Get() : mesh_pso_.Get());
        cmd_->SetGraphicsRootSignature(mesh_root_.Get());
        cmd_->SetGraphicsRoot32BitConstants(0, sizeof(MeshConstants) / 4, &mc, 0);
        cmd_->SetGraphicsRootShaderResourceView(
            1, mesher_.vertices()->GetGPUVirtualAddress());
        cmd_->IASetIndexBuffer(&ibv);
        // One command per slot. A retired or never-used slot has all-zero
        // arguments and the command processor skips it, which is cheaper than
        // compacting the array every time a chunk moves.
        cmd_->ExecuteIndirect(cmd_sig_.Get(), kSlots, mesher_.draw_args(), 0, nullptr, 0);
    }
}

// Record the copy of this frame's newly meshed chunks into this frame's
// staging buffer, and remember which chunk each patch belongs to.
void Renderer::stage_heights() {
    const UINT ring = static_cast<UINT>(frame_index_ % kReadbackRing);
    RingRecord& r = ring_[ring];
    const std::vector<Streamer::MeshedChunk>& meshed = streamer_.last_meshed();

    UINT slots[kReadbackChunksPerFrame];
    r.count = 0;
    r.frame = frame_index_;
    for (size_t i = 0; i < meshed.size() && r.count < kReadbackChunksPerFrame; ++i) {
        const Streamer::MeshedChunk& m = meshed[i];
        const float S = lod_chunk_size(m.key.lod);
        slots[r.count] = m.slot;
        r.keys[r.count] = m.key;
        r.origin[r.count][0] = float(m.key.x) * S;
        r.origin[r.count][1] = float(m.key.y) * S;
        r.origin[r.count][2] = float(m.key.z) * S;
        r.voxel[r.count] = lod_voxel(m.key.lod);
        ++r.count;
    }
    mesher_.record_height_copy(cmd_.Get(), ring, slots, r.count);
    mesher_.record_flag_copy(cmd_.Get(), ring, slots, r.count);
}

// Consume what was staged two frames ago. Two, because with two frames in
// flight the CPU has already waited on that frame's fence, so the map cannot
// block -- which is the entire point of the ring.
void Renderer::consume_heights() {
    if (frame_index_ < kReadbackRing - 1) return;
    const UINT ring = static_cast<UINT>((frame_index_ + 1) % kReadbackRing);
    RingRecord& r = ring_[ring];
    if (r.count == 0) return;
    if (frame_index_ < r.frame + (kReadbackRing - 1)) return;

    const float* h = mesher_.mapped_heights(ring, r.count);
    if (!h) { r.count = 0; return; }
    const UINT* flags = mesher_.mapped_flags(ring, r.count);

    stats_.readback_latency_frames = static_cast<UINT>(frame_index_ - r.frame);
    stats_.patches_this_frame = r.count;
    stats_.patches_total += r.count;

    // Every reported height should sit on the isosurface. Checked against the
    // C++ side of the dialect, the same tolerance comparison B2 used on the
    // mesh -- and the only thing that would catch a ring read one frame early,
    // which returns a previous chunk's perfectly plausible terrain.
    double sum = 0.0;
    unsigned long long n = 0;
    for (UINT p = 0; p < r.count; ++p) {
        const float* g = h + size_t(p) * kHeightSamples;
        const float v = r.voxel[p];
        for (UINT k = 0; k < kHeightGrid; k += 4) {
            for (UINT i = 0; i < kHeightGrid; i += 4) {
                const float y = g[k * kHeightGrid + i];
                if (y > 1.0e29f) continue;          // no surface in this column
                const float x = r.origin[p][0] + float(i) * v;
                const float z = r.origin[p][2] + float(k) * v;
                const float d = trivial_density(x, y, z);
                const float a = d < 0.0f ? -d : d;
                if (a > stats_.height_residual_max) stats_.height_residual_max = a;
                sum += a;
                ++n;
            }
        }
    }
    // Copied out, because the staging buffer is unmapped on the next line and
    // will be overwritten two frames from now.
    for (UINT p = 0; p < r.count; ++p) {
        PatchEvent e;
        e.key = r.keys[p];
        e.origin[0] = r.origin[p][0];
        e.origin[1] = r.origin[p][1];
        e.origin[2] = r.origin[p][2];
        e.voxel = r.voxel[p];
        e.multi_surface_columns = flags ? flags[p] : 0;
        ++stats_.chunks_classified;
        if (e.multi_surface_columns) {
            ++stats_.chunks_needing_mesh;
            mesh_wanted_.push_back(r.keys[p]);
        }
        const float* src = h + size_t(p) * kHeightSamples;
        e.h.assign(src, src + kHeightSamples);
        patches_added_.push_back(e);
    }

    if (flags) mesher_.unmap_flags(ring);
    mesher_.unmap_heights(ring);
    if (n) {
        const unsigned long long before = stats_.height_samples_checked;
        stats_.height_samples_checked = before + n;
        stats_.height_residual_mean =
            (stats_.height_residual_mean * double(before) + sum) /
            double(stats_.height_samples_checked);
    }
    r.count = 0;
}

// Take what the classification asked for and copy those chunks' triangles.
// A chunk that has since been retired is dropped rather than copied: its slot
// may now hold a different chunk, and 163 KB of the wrong terrain is worse
// than none.
void Renderer::stage_meshes() {
    const UINT ring = static_cast<UINT>(frame_index_ % kReadbackRing);
    MeshRingRecord& m = mesh_ring_[ring];
    m.count = 0;
    m.frame = frame_index_;
    if (mesh_wanted_.empty()) return;

    UINT slots[kMeshChunksPerFrame];
    size_t taken = 0;
    while (taken < mesh_wanted_.size() && m.count < kMeshChunksPerFrame) {
        const ChunkKey k = mesh_wanted_[taken];
        ++taken;
        UINT slot = 0;
        if (!streamer_.slot_of(k, slot)) continue;
        slots[m.count] = slot;
        m.keys[m.count] = k;
        m.slots[m.count] = slot;
        ++m.count;
    }
    mesh_wanted_.erase(mesh_wanted_.begin(), mesh_wanted_.begin() + taken);
    mesher_.record_mesh_copy(cmd_.Get(), ring, slots, m.count);
}

void Renderer::consume_meshes() {
    if (frame_index_ < kReadbackRing - 1) return;
    const UINT ring = static_cast<UINT>((frame_index_ + 1) % kReadbackRing);
    MeshRingRecord& m = mesh_ring_[ring];
    if (m.count == 0) return;
    if (frame_index_ < m.frame + (kReadbackRing - 1)) return;

    const unsigned char* base = mesher_.mapped_mesh(ring, m.count);
    if (!base) { m.count = 0; return; }

    for (UINT p = 0; p < m.count; ++p) {
        UINT slot = 0;
        if (!streamer_.slot_of(m.keys[p], slot) || slot != m.slots[p]) continue;
        UINT nv = 0, ni = 0;
        mesher_.slot_counts(slot, nv, ni);
        if (nv == 0 || ni < 3) continue;

        const unsigned char* chunk = base + size_t(p) * kMeshChunkBytes;
        const MeshVertex* mv = reinterpret_cast<const MeshVertex*>(chunk);
        const unsigned* mi =
            reinterpret_cast<const unsigned*>(chunk + kMeshVertexBytes);

        MeshPatchEvent e;
        e.key = m.keys[p];
        e.verts.resize(size_t(nv) * 3);
        for (UINT i = 0; i < nv; ++i) {
            e.verts[size_t(i) * 3 + 0] = mv[i].pos[0];
            e.verts[size_t(i) * 3 + 1] = mv[i].pos[1];
            e.verts[size_t(i) * 3 + 2] = mv[i].pos[2];
        }
        // Pool indices are absolute so the draw needs no rebasing; a shape
        // built from this chunk's vertices alone does.
        const unsigned vbase = slot * kSlotVertices;
        e.idx.reserve(ni);
        for (UINT i = 0; i < ni; ++i) {
            const unsigned v = mi[i] - vbase;
            e.idx.push_back(v < nv ? v : 0);
        }
        mesh_patches_added_.push_back(e);
        ++stats_.mesh_shapes_delivered;
    }
    mesher_.unmap_mesh(ring);
    m.count = 0;
}

void Renderer::record_frame(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    // Streaming first, in compute, then the draw reads what it produced.
    mesher_.record_to_compute(cmd_.Get());
    mesher_.record_timestamp(cmd_.Get(), 0);
    stats_.meshed_this_frame =
        streamer_.record_work(cmd_.Get(), mesher_, kMeshBudgetPerFrame);
    mesher_.record_timestamp(cmd_.Get(), 1);
    mesher_.resolve_timestamps(cmd_.Get());
    stage_heights();
    stage_meshes();
    mesher_.record_counter_copy(cmd_.Get());
    mesher_.record_vertex_copy(cmd_.Get());
    mesher_.record_to_draw(cmd_.Get());

    record_scene(view, rtv);
}

void Renderer::render(const ViewConstants& view) {
    if (!ready_) return;

    patches_added_.clear();
    patches_removed_.clear();
    streamer_.update(view.cam_pos[0], view.cam_pos[1], view.cam_pos[2]);
    {
        const std::vector<ChunkKey>& gone = streamer_.retired_keys();
        patches_removed_.assign(gone.begin(), gone.end());
    }
    mesh_patches_added_.clear();
    stats_.patches_this_frame = 0;
    consume_heights();
    consume_meshes();
    stats_.mesh_backlog = mesh_wanted_.size();

    allocators_[frame_]->Reset();
    cmd_->Reset(allocators_[frame_].Get(), nullptr);

    D3D12_RESOURCE_BARRIER to_rt = {};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = targets_[frame_].Get();
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame_) * rtv_stride_;
    record_frame(view, rtv);

    D3D12_RESOURCE_BARRIER to_present = to_rt;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_->ResourceBarrier(1, &to_present);

    cmd_->Close();
    ID3D12CommandList* lists[] = {cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);

    const UINT present_flags = (!vsync_ && tearing_ok_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swapchain_->Present(vsync_ ? 1 : 0, present_flags);
    next_frame();

    // Read a frame or two stale, deliberately: waiting on these would be a
    // stall in the thing being measured.
    mesher_.read_timestamps();
    mesher_.read_counts(stats_.vertices, stats_.indices, stats_.overflowed);
    stats_.mesh_ms = mesher_.last_mesh_ms();
    stats_.resident = streamer_.resident_count();
    stats_.pending = streamer_.pending_count();
    stats_.free_slots = streamer_.free_slot_count();
    ++frame_index_;
}

bool Renderer::capture(const ViewConstants& view, const char* path, std::string& error) {
    if (!ready_) { error = "renderer not initialised"; return false; }

    // Fill residency before capturing: a single frame would otherwise show
    // only the eight chunks the per-frame budget allows.
    for (int pass = 0; pass < 400; ++pass) {
        streamer_.update(view.cam_pos[0], view.cam_pos[1], view.cam_pos[2]);
        if (streamer_.pending_count() == 0 && pass > 0) break;
        allocators_[frame_]->Reset();
        cmd_->Reset(allocators_[frame_].Get(), nullptr);
        mesher_.record_to_compute(cmd_.Get());
        streamer_.record_work(cmd_.Get(), mesher_, 64);
        mesher_.record_counter_copy(cmd_.Get());
        mesher_.record_vertex_copy(cmd_.Get());
        mesher_.record_to_draw(cmd_.Get());
        cmd_->Close();
        ID3D12CommandList* ls[] = {cmd_.Get()};
        queue_->ExecuteCommandLists(1, ls);
        wait_for_gpu();
    }
    mesher_.read_counts(stats_.vertices, stats_.indices, stats_.overflowed);
    stats_.resident = streamer_.resident_count();
    stats_.pending = streamer_.pending_count();
    stats_.free_slots = streamer_.free_slot_count();

    const D3D12_RESOURCE_DESC rd = targets_[frame_]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT   rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { error = hr_text("CreateCommittedResource (readback)", hr); return false; }

    allocators_[frame_]->Reset();
    cmd_->Reset(allocators_[frame_].Get(), nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = targets_[frame_].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame_) * rtv_stride_;
    record_scene(view, rtv);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd_->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = targets_[frame_].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_->ResourceBarrier(1, &b);

    cmd_->Close();
    ID3D12CommandList* lists[] = {cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();

    void* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &range, &mapped);
    if (FAILED(hr)) { error = hr_text("Map (readback)", hr); return false; }

    // R8G8B8A8 to packed BGR. The row pitch is aligned to 256 bytes and is
    // almost never the image width, so rows are copied one at a time.
    std::vector<unsigned char> bgr(static_cast<size_t>(width_) * height_ * 3);
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    for (UINT y = 0; y < height_; ++y) {
        const unsigned char* s = base + static_cast<size_t>(y) * fp.Footprint.RowPitch;
        unsigned char* d = &bgr[static_cast<size_t>(y) * width_ * 3];
        for (UINT x = 0; x < width_; ++x) {
            d[x * 3 + 0] = s[x * 4 + 2];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 0];
        }
    }
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);

    if (!write_bmp(path, static_cast<int>(width_), static_cast<int>(height_), bgr.data())) {
        error = std::string("could not write ") + path;
        return false;
    }
    return true;
}

// Evaluated here in C++ from the same header the kernel compiles as HLSL. If
// these two ever disagree beyond tolerance, the dialect has drifted and M1 is
// in trouble -- which is the thing ADR-001 says to check by tolerance and
// never by hash.
bool Renderer::check_against_field(float& max_err, float& mean_err, UINT& checked) {
    max_err = 0.0f;
    mean_err = 0.0f;
    checked = 0;
    const UINT n = mesher_.slot_vertex_count(0);
    std::vector<MeshVertex> verts;
    if (!mesher_.read_vertices(0, n, verts)) return false;
    double sum = 0.0;
    for (size_t i = 0; i < verts.size(); ++i) {
        const float d = trivial_density(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        const float a = d < 0.0f ? -d : d;
        if (a > max_err) max_err = a;
        sum += a;
    }
    checked = static_cast<UINT>(verts.size());
    if (checked) mean_err = static_cast<float>(sum / checked);
    return true;
}

void Renderer::drain_debug_messages(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    if (!info_queue_) {
        std::fprintf(f, "no info queue: the debug layer is not enabled\n");
        std::fclose(f);
        return;
    }
    const UINT64 n = info_queue_->GetNumStoredMessages();
    std::fprintf(f, "%llu validation messages\n", static_cast<unsigned long long>(n));
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        info_queue_->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(info_queue_->GetMessage(i, m, &len))) {
            std::fprintf(f, "[%d] %s\n", int(m->Severity), m->pDescription);
        }
    }
    std::fclose(f);
}

void Renderer::dump_draw_args(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::vector<UINT> a;
    mesher_.read_draw_args(0, kSlots, a);
    UINT nonzero = 0, total = 0, biggest = 0, biggest_slot = 0;
    for (size_t s = 0; s * 5 + 4 < a.size(); ++s) {
        const UINT n = a[s * 5 + 0];
        if (!n) continue;
        ++nonzero;
        total += n;
        if (n > biggest) { biggest = n; biggest_slot = UINT(s); }
    }
    std::fprintf(f, "slots with a non-empty draw: %u of %u\n", nonzero, kSlots);
    std::fprintf(f, "indices across all draws: %u (%u triangles)\n", total, total / 3);
    std::fprintf(f, "biggest slot %u with %u indices\n", biggest_slot, biggest);
    std::fprintf(f, "\nslot  idxcount inst startidx basevtx startinst\n");
    UINT shown = 0;
    for (size_t s = 0; s * 5 + 4 < a.size() && shown < 12; ++s) {
        if (!a[s * 5 + 0]) continue;
        ++shown;
        std::fprintf(f, "%4zu  %8u %4u %8u %7u %9u\n", s,
                     a[s*5+0], a[s*5+1], a[s*5+2], a[s*5+3], a[s*5+4]);
    }
    std::fclose(f);
}

void Renderer::shutdown() {
    if (ready_) wait_for_gpu();
    ready_ = false;
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}

}  // namespace engine
