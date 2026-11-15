#include "engine/render/renderer.h"
#include "engine/render/shader_compile.h"
#include "engine/render/water_wave.h"
#include "engine/render/lighting_units.h"
#include "engine/render/atmosphere.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace engine {
namespace {

const char* kMeshShaderPath = "engine/render/shaders/mesh.hlsl";
const char* kWaterShaderPath = "engine/render/shaders/water.hlsl";

// The far plane has to clear the clipmap's own corner or the outermost ring
// is streamed and then clipped away: 8 levels of 7 x 7 give a +/-7.2 km box,
// whose diagonal is 10.2 km.
const float kNearPlane = 0.2f;
const float kFarPlane  = 16000.0f;

// The sea surface grid. 192 cells a side is 37,249 vertices and 221,184
// indices -- small enough that the whole thing is one static index buffer
// uploaded once, and dense enough that the cells nearest the camera are about
// 0.4 m across. The spacing is quadratic, so the same grid reaches the far
// plane; see grid_offset in water.hlsl.
const UINT kWaterCells = 256;

struct WaterConstants {
    float view_proj[16];
    float cam_pos[3];
    float time;
    float sun_dir[3];
    float far_plane;
    float cam_fwd[3];
    float aspect;
    float cam_right[3];
    float tan_half_fov;
    float cam_up[3];
    float pad1;
    float sea_level;
    float sea_state;
    float grid_cells;
    float extent;
    float debug_mode;
    float cam_water;   // world y of the sea surface directly at the camera
    float pad0[2];
    float light[7][4]; // the frame's light (ADR-004 L2): Renderer::fill_light
};

// Matches cbuffer ShadowConstants in shadow.hlsli: 368 bytes, in a 512-byte buffer.
struct ShadowConstants {
    float light_vp[3][16];
    float split[4];        // far edge of each cascade; w = on
    float texel[4];
    float depth_range[4];
    float cam[3];
    float res;
    float light[7][4];     // the frame's light (ADR-004 L2): Renderer::fill_light
    float cluster[3][4];   // local lights' clusters (ADR-004 L3): Renderer::record_lights
};
static_assert(sizeof(ShadowConstants) == 416, "ShadowConstants must match shadow.hlsli");

// Matches cbuffer ClusterConstants in lights.hlsl.
struct ClusterConstants {
    float cam_pos[3];   UINT  light_count;
    float cam_right[3]; float tan_half_fov;
    float cam_up[3];    float aspect;
    float cam_fwd[3];   float near_z;
    float log_scale;    float far_z; UINT pad0, pad1;
};
static_assert(sizeof(ClusterConstants) == 80, "ClusterConstants must match lights.hlsl");

// Row-vector matrices, as everything here: clip = p * M.
void mat_mul4(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof r);
}

// Frustum planes of a view-projection, dot(n, p) + d >= 0 inside, as the
// tree cull reads them (and as compute_cascades builds them).
void planes_of(const float m[16], float planes[6][4]) {
    float col[4][4];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) col[c][r] = m[r * 4 + c];
    for (int k = 0; k < 4; ++k) {
        planes[0][k] = col[3][k] + col[0][k];
        planes[1][k] = col[3][k] - col[0][k];
        planes[2][k] = col[3][k] + col[1][k];
        planes[3][k] = col[3][k] - col[1][k];
        planes[4][k] = col[2][k];
        planes[5][k] = col[3][k] - col[2][k];
    }
    for (int p = 0; p < 6; ++p) {
        const float l = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
        if (l > 0.0f) for (int k = 0; k < 4; ++k) planes[p][k] /= l;
    }
}

// One shadow face of a local light: looking along `fwd` from `pos`, with a
// field of view a little over 90 degrees so a cube's faces overlap enough
// for PCF and the normal offset at their edges.
void light_face(const float pos[3], const float fwd[3], const float up_hint[3], float fov_deg, float range,
                float out_vp[16], float right[3], float up[3]) {
    float u[3] = {up_hint[0], up_hint[1], up_hint[2]};
    float r[3] = {u[1] * fwd[2] - u[2] * fwd[1], u[2] * fwd[0] - u[0] * fwd[2], u[0] * fwd[1] - u[1] * fwd[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-4f) { u[0] = 1.0f; u[1] = 0.0f; u[2] = 0.0f;
        r[0] = u[1] * fwd[2] - u[2] * fwd[1]; r[1] = u[2] * fwd[0] - u[0] * fwd[2]; r[2] = u[0] * fwd[1] - u[1] * fwd[0];
        rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); }
    for (int k = 0; k < 3; ++k) r[k] /= rl;
    // up = fwd x right, so (right, up, fwd) is a basis.
    u[0] = fwd[1] * r[2] - fwd[2] * r[1];
    u[1] = fwd[2] * r[0] - fwd[0] * r[2];
    u[2] = fwd[0] * r[1] - fwd[1] * r[0];
    float v[16] = {};
    for (int i = 0; i < 3; ++i) { v[i * 4 + 0] = r[i]; v[i * 4 + 1] = u[i]; v[i * 4 + 2] = fwd[i]; }
    v[12] = -(r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2]);
    v[13] = -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]);
    v[14] = -(fwd[0] * pos[0] + fwd[1] * pos[1] + fwd[2] * pos[2]);
    v[15] = 1.0f;
    const float n = 0.05f, f = range > 0.2f ? range : 0.2f;
    const float s = 1.0f / std::tan(0.5f * fov_deg * 3.14159265f / 180.0f);
    float p[16] = {};
    p[0] = s;
    p[5] = s;
    p[10] = f / (f - n);
    p[11] = 1.0f;
    p[14] = -n * f / (f - n);
    mat_mul4(v, p, out_vp);
    for (int k = 0; k < 3; ++k) { right[k] = r[k]; up[k] = u[k]; }
}

// The scene is drawn into two HDR targets -- direct sunlight and everything
// else -- which the post chain combines (scene_out.hlsli, post.hlsl).
const DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Matches cbuffer PostConstants in post.hlsl: 416 bytes.
struct PostConstants {
    float inv_view_proj[16];
    float view_proj[16];
    float prev_view_proj[16];
    float cam_pos[3];  float time;
    float sun_dir[3];  float near_plane;
    float far_plane;   float sharpen;
    float inv_size[2];
    float jitter_ndc[2];
    UINT  width, height;
    UINT  hist_cur, flags;
    UINT  frame;       float ao_radius;
    float contact_len; float debug;
    float dt;          float ev_comp;
    float ev100_fixed; UINT  tonemap;
    float meter_lo;    float meter_hi;
    UINT  expo_flags;  float agx_saturation;
    float agx_power;   float cam_alt_km;
    float sky_sun_dir[3];  float pad2;
    float sky_moon_dir[3]; float pad3;
    float sun_disk[3];     float pad4;
    float moon_disk[3];    float pad5;
    float cam_fwd[3];      float fog_on;
    float fog_near;        float fog_log_ratio; float rt_shadows; float pad6;
};
static_assert(sizeof(PostConstants) == 416, "PostConstants must match post.hlsl");

// Matches cbuffer FogConstants in fog.hlsl.
struct FogConstants {
    float prev_view_proj[16];
    float cam_pos[3];   float time;
    float cam_right[3]; float tan_half_fov;
    float cam_up[3];    float aspect;
    float cam_fwd[3];   float jitter;
    float near_z;       float log_ratio; UINT history_valid; float time_factor;
    float map_extent;   float haze;      float sea_level;    float patch;
    float wind[2];      float phase_g;   float albedo;
    float screen_w;     float screen_h;  float density_scale; float pad0;
};
static_assert(sizeof(FogConstants) == 192, "FogConstants must match fog.hlsl");
// Matches cbuffer RtConstants in rt_shadow.hlsl.
struct RtConstants {
    float inv_view_proj[16];
    float cam_pos[3];  UINT width;
    float key_dir[3];  UINT height;
    float near_plane;  float far_plane; float csm_reach; UINT frame;
    UINT  checks;      float light_radius; float pad1[2];
};
static_assert(sizeof(RtConstants) == 128, "RtConstants must match rt_shadow.hlsl");
// The mist's froxels: 160 x 90 x 128, log-spaced from 0.5 m to 6 km.
const UINT  kFogX = 160, kFogY = 90, kFogZ = 128;
const float kFogNear = 0.5f, kFogFar = 6000.0f;

// Matches cbuffer AtmosphereConstants in atmosphere.hlsl.
struct AtmosphereConstants {
    float inv_view_proj[16];
    float cam_pos[3];  float cam_alt_km;
    float sun_dir[3];  float far_plane;
    float sun_lux[3];  float pad0;
    float moon_dir[3]; float pad1;
    float moon_lux[3]; float pad2;
};
static_assert(sizeof(AtmosphereConstants) == 144, "AtmosphereConstants must match atmosphere.hlsl");

// 4x4 inverse (row-major), by cofactors.
bool invert4(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
    if (det == 0.0) return false;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
    return true;
}

// Halton (2, 3), the usual TAA sequence: eight sub-pixel offsets that fill
// the pixel evenly before repeating.
float halton(int i, int b) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

// Cascade far edges, metres from the camera. The last one is just past the
// default tree draw distance; beyond it the shadow fades out under the fog.
const float kCascadeSplit[4] = {0.2f, 35.0f, 140.0f, 520.0f};
// How far towards the sun a caster may be and still shadow a cascade: the
// tallest relief between here and the slice. Depth precision is not the
// constraint at D32.
const float kShadowBack = 400.0f;

struct MeshConstants {
    float view_proj[16];
    float cam_pos[3];
    float far_plane;
    float wire;
    float slot_vertices;   // so the vertex shader can tell which slot it is in
    float mesh_debug;      // 0 off, 1 skirt curtain, 2 creases
    float texture_mode;    // 0 flat, 1 noise detail, 2 metric grid, 3 both
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
            adapter_vram_ = desc.DedicatedVideoMemory;
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
    // Typeless, so the post chain can read it as R32_FLOAT.
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
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

// The sea's own pipeline. It needs the WORLD buffers, not just a camera: the
// vertex shader asks the shared field function how deep the water is, so the
// shoreline is the real one. Those are the same bindings the mesher's compute
// root signature uses -- b1 plus u11..u20 -- because it is literally the same
// header being included.
bool Renderer::create_water_pipeline(std::string& error) {
    if (!water_root_) {
        // A root CBV, not root constants. WaterConstants grew to 44 DWORDs
        // when the camera basis went in for the projected grid, and 44 plus a
        // CBV plus ten root UAVs is 66 against a 64 DWORD root signature --
        // CreateRootSignature returned E_INVALIDARG, the renderer failed to
        // initialise, and because that path ends in a modal message box the
        // headless run simply hung. A descriptor costs 2 DWORDs however big
        // the struct behind it is.
        D3D12_ROOT_PARAMETER params[12] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].Descriptor.ShaderRegister = 1;
        for (UINT i = 0; i < 10; ++i) {
            params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[2 + i].Descriptor.ShaderRegister = 11 + i;
        }

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 12;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, sig_err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &sig, &sig_err);
        if (FAILED(hr)) {
            error = "water root signature: ";
            error += sig_err ? static_cast<const char*>(sig_err->GetBufferPointer())
                             : hr_text("D3D12SerializeRootSignature", hr).c_str();
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&water_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (water)", hr); return false; }
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kWaterShaderPath, "vs_main", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kWaterShaderPath, "ps_main", "ps_5_1", ps, error)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = water_root_.Get();
    pd.VS.pShaderBytecode = vs->GetBufferPointer();
    pd.VS.BytecodeLength  = vs->GetBufferSize();
    pd.PS.pShaderBytecode = ps->GetBufferPointer();
    pd.PS.BytecodeLength  = ps->GetBufferSize();
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Both faces. A camera under the surface -- in a flooded cave mouth, or
    // simply swimming -- has to see the underside, and culling it would show
    // the sky through the sea.
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    // Straight alpha over the terrain already drawn. Depth WRITE is off: the
    // sea is transparent in the shallows, and a transparent surface that
    // writes depth hides whatever is drawn after it.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // The ambient target: the sea only removes what is under it there, so
    // the seabed's occlusion does not show through (water.hlsl).
    pd.BlendState.IndependentBlendEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC& rt1 = pd.BlendState.RenderTarget[1];
    rt1 = rt;
    rt1.SrcBlend = D3D12_BLEND_ZERO;
    rt1.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;

    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = kSceneFormat;
    pd.RTVFormats[1] = kSceneFormat;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (water)", hr); return false; }
    water_pso_ = pso;

    // The LOD fade state, one per frame in flight for the same reason as the
    // constant buffer below. kSlots float2s, rewritten every frame.
    if (!fade_buf_[0]) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kSlots * 2 * sizeof(float);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < kFrameCount; ++i) {
            const HRESULT fh = device_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&fade_buf_[i]));
            if (FAILED(fh)) { error = hr_text("CreateCommittedResource (fade)", fh); return false; }
            D3D12_RANGE none = {0, 0};
            fade_buf_[i]->Map(0, &none, &fade_cpu_[i]);
        }
    }

    // One constant buffer per frame in flight. A single buffer would be
    // rewritten by the CPU while the GPU was still reading the previous
    // frame's copy of it.
    if (!water_cb_[0]) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 512;                 // CBVs are 256-byte aligned; 288 bytes since L2's light
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < kFrameCount; ++i) {
            const HRESULT ch = device_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&water_cb_[i]));
            if (FAILED(ch)) { error = hr_text("CreateCommittedResource (water cb)", ch); return false; }
            D3D12_RANGE none = {0, 0};
            water_cb_[i]->Map(0, &none, &water_cb_cpu_[i]);
        }
    }

    // The grid's indices. The POSITIONS come from SV_VertexID -- a vertex of a
    // regular grid knows where it is from its own id and needs no buffer --
    // but the triangles do not follow from an id, so they are uploaded once.
    if (!water_indices_) {
        const UINT n = kWaterCells + 1;
        std::vector<UINT> idx;
        idx.reserve(size_t(kWaterCells) * kWaterCells * 6);
        for (UINT z = 0; z < kWaterCells; ++z) {
            for (UINT x = 0; x < kWaterCells; ++x) {
                const UINT a = z * n + x, b = a + 1, c = a + n, d = c + 1;
                idx.push_back(a); idx.push_back(c); idx.push_back(b);
                idx.push_back(b); idx.push_back(c); idx.push_back(d);
            }
        }
        water_index_count_ = static_cast<UINT>(idx.size());
        const UINT64 bytes = UINT64(idx.size()) * 4;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // An upload heap read straight by the input assembler. It is written
        // once and never again, so the extra copy into a default heap would
        // buy nothing.
        const HRESULT ih = device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&water_indices_));
        if (FAILED(ih)) { error = hr_text("CreateCommittedResource (water indices)", ih); return false; }

        void* mapped = nullptr;
        D3D12_RANGE none = {0, 0};
        water_indices_->Map(0, &none, &mapped);
        std::memcpy(mapped, idx.data(), size_t(bytes));
        water_indices_->Unmap(0, nullptr);

        water_ibv_.BufferLocation = water_indices_->GetGPUVirtualAddress();
        water_ibv_.SizeInBytes = static_cast<UINT>(bytes);
        water_ibv_.Format = DXGI_FORMAT_R32_UINT;
    }
    return true;
}

bool Renderer::create_mesh_pipeline(std::string& error) {
    if (!mesh_root_) {
        // 24 DWORDs of constants + three root SRVs at 2 each + the shadow CBV
        // (2) and its table (1) = 33 of the 64.
        D3D12_ROOT_PARAMETER params[6] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(MeshConstants) / 4;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].Descriptor.ShaderRegister = 0;
        // Per-slot LOD cross-fade state, (mode, v) as Streamer::fill_visibility.
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].Descriptor.ShaderRegister = 1;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].Descriptor.ShaderRegister = 2;
        // The shadow table: the sun's map at heap [0], and since ADR-004 L3
        // the local lights' buffers and atlas at [24..28] (t20..t24).
        D3D12_DESCRIPTOR_RANGE sr[2] = {};
        sr[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr[0].NumDescriptors = 1;
        sr[0].BaseShaderRegister = 8;
        sr[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr[1].NumDescriptors = 5;
        sr[1].BaseShaderRegister = 20;
        sr[1].OffsetInDescriptorsFromTableStart = 24;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[4].DescriptorTable.NumDescriptorRanges = 2;
        params[4].DescriptorTable.pDescriptorRanges = sr;
        // Each vertex's surface material (ADR-004 L1), parallel to t0.
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[5].Descriptor.ShaderRegister = 2;
        D3D12_STATIC_SAMPLER_DESC cmp = {};
        cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cmp.AddressU = cmp.AddressV = cmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        cmp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;   // outside: lit
        cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        cmp.MaxLOD = D3D12_FLOAT32_MAX;
        cmp.ShaderRegister = 1;
        cmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 6;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &cmp;
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
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = kSceneFormat;
    pd.RTVFormats[1] = kSceneFormat;
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

    // The terrain as a shadow caster: the same vertex shader under the
    // light's view, depth only. No culling -- a thin ridge seen edge-on from
    // the sun must still cast -- and a slope-scaled bias for acne.
    pd.PS.pShaderBytecode = nullptr;
    pd.PS.BytecodeLength = 0;
    pd.NumRenderTargets = 0;
    pd.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.RasterizerState.DepthBiasClamp = 0.002f;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ComPtr<ID3D12PipelineState> shadow;
    hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadow));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (terrain shadow)", hr); return false; }

    if (ready_) wait_for_gpu();
    mesh_pso_ = pso;
    mesh_wire_pso_ = wire;
    mesh_shadow_pso_ = shadow;
    return true;
}

bool Renderer::create_shadow_resources(std::string& error) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kShadowRes;
    rd.Height = kShadowRes;
    rd.DepthOrArraySize = kCascades;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    // Starts readable: every frame begins with it a shader resource, so a
    // frame that skips the shadow pass still binds a valid (cleared) map.
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                  IID_PPV_ARGS(&shadow_map_));
    if (FAILED(hr)) { error = hr_text("shadow map", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kCascades;
    hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&shadow_dsv_heap_));
    if (FAILED(hr)) { error = hr_text("shadow dsv heap", hr); return false; }
    const UINT dinc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT c = 0; c < kCascades; ++c) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = c;
        dv.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = shadow_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(c) * dinc;
        device_->CreateDepthStencilView(shadow_map_.Get(), &dv, h);
    }

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // [0] shadow, [1..2] impostor atlases, [3..11] post SRVs (the last two the
    // sky-view table and the aerial-perspective volume), [12..15] post UAVs,
    // [16..17] the atmosphere's static tables, [18..19] its per-frame UAVs,
    // [24..28] the local lights (L3): lights, cluster counts and items, the
    // shadow atlas and its faces -- reached by every lit pipeline through a
    // second range of the table that starts at the sun's map, [0];
    // [29..35] the mist (L4): its map, two inject volumes (this frame's and
    // last's, swapping), the integrated volume, and their UAVs.
    hd.NumDescriptors = 48;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_));
    if (FAILED(hr)) { error = hr_text("shared srv heap", hr); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = kCascades;
    device_->CreateShaderResourceView(shadow_map_.Get(), &sd, srv_heap_->GetCPUDescriptorHandleForHeapStart());

    for (UINT f = 0; f < kFrameCount; ++f) {
        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&shadow_cb_[f]));
        if (FAILED(hr)) { error = hr_text("shadow cb", hr); return false; }
        shadow_cb_[f]->Map(0, nullptr, &shadow_cb_cpu_[f]);
        std::memset(shadow_cb_cpu_[f], 0, 512);
    }
    return true;
}

// One orthographic view down the sun per cascade, fitted to a sphere round
// that cascade's slice of the view frustum. The sphere's radius depends only
// on the split distances and the lens, so it never changes size as the camera
// turns; its centre is snapped to whole shadow texels in light space, so a
// static shadow edge stays on the same texels instead of crawling as the
// camera moves.
void Renderer::compute_cascades(const ViewConstants& view) {
    // The key light: the sun, or the moon at night (sky.cpp).
    float L[3] = {sky_frame_.key_dir[0], sky_frame_.key_dir[1], sky_frame_.key_dir[2]};
    {
        const float l = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (int k = 0; k < 3; ++k) L[k] /= l;
    }
    // right = normalize(cross(Y, L)), up = cross(L, right)
    float R[3] = {L[2], 0.0f, -L[0]};
    {
        const float l = std::sqrt(R[0] * R[0] + R[2] * R[2]);
        R[0] /= l; R[2] /= l;
    }
    const float U[3] = {L[1] * R[2] - L[2] * R[1], L[2] * R[0] - L[0] * R[2], L[0] * R[1] - L[1] * R[0]};
    const float diag = view.tan_half_fov * std::sqrt(1.0f + view.aspect * view.aspect);
    for (UINT c = 0; c < kCascades; ++c) {
        const float n = kCascadeSplit[c], f = kCascadeSplit[c + 1];
        const float rn = n * diag, rf = f * diag;
        float zc = ((f * f + rf * rf) - (n * n + rn * rn)) / (2.0f * (f - n));
        if (zc > f) zc = f;
        if (zc < n) zc = n;
        float r = std::sqrt((zc - n) * (zc - n) + rn * rn);
        const float rf2 = std::sqrt((f - zc) * (f - zc) + rf * rf);
        if (rf2 > r) r = rf2;
        r = std::ceil(r * 1.02f);
        float ctr[3];
        for (int k = 0; k < 3; ++k) ctr[k] = view.cam_pos[k] + view.cam_fwd[k] * zc;
        const float texel = 2.0f * r / float(kShadowRes);
        float cx = ctr[0] * R[0] + ctr[1] * R[1] + ctr[2] * R[2];
        float cy = ctr[0] * U[0] + ctr[1] * U[1] + ctr[2] * U[2];
        cx = std::floor(cx / texel) * texel;
        cy = std::floor(cy / texel) * texel;
        const float cz = -(ctr[0] * L[0] + ctr[1] * L[1] + ctr[2] * L[2]);
        const float zn = cz - r - kShadowBack, zf = cz + r + 20.0f;
        const float range = zf - zn;
        Cascade& cs = cascades_[c];
        float* m = cs.vp;
        for (int k = 0; k < 16; ++k) m[k] = 0.0f;
        for (int i = 0; i < 3; ++i) {
            m[i * 4 + 0] = R[i] / r;
            m[i * 4 + 1] = U[i] / r;
            m[i * 4 + 2] = -L[i] / range;
        }
        m[12] = -cx / r;
        m[13] = -cy / r;
        m[14] = -zn / range;
        m[15] = 1.0f;
        cs.texel = texel;
        cs.depth_range = range;
        for (int k = 0; k < 3; ++k) { cs.right[k] = R[k]; cs.up[k] = U[k]; }
        // Planes, as in tree culling: dot(n, p) + d >= 0 inside.
        float col[4][4];
        for (int cc = 0; cc < 4; ++cc) for (int rr = 0; rr < 4; ++rr) col[cc][rr] = m[rr * 4 + cc];
        for (int k = 0; k < 4; ++k) {
            cs.planes[0][k] = col[3][k] + col[0][k];
            cs.planes[1][k] = col[3][k] - col[0][k];
            cs.planes[2][k] = col[3][k] + col[1][k];
            cs.planes[3][k] = col[3][k] - col[1][k];
            cs.planes[4][k] = col[2][k];
            cs.planes[5][k] = col[3][k] - col[2][k];
        }
        for (int p = 0; p < 6; ++p) {
            const float l = std::sqrt(cs.planes[p][0] * cs.planes[p][0] + cs.planes[p][1] * cs.planes[p][1] +
                                      cs.planes[p][2] * cs.planes[p][2]);
            if (l > 0.0f) for (int k = 0; k < 4; ++k) cs.planes[p][k] /= l;
        }
    }
}

void Renderer::record_shadows(const ViewConstants& view) {
    ShadowConstants sc = {};
    sc.res = float(kShadowRes);
    for (int k = 0; k < 3; ++k) sc.cam[k] = view.cam_pos[k];
    for (UINT c = 0; c < kCascades; ++c) sc.split[c] = kCascadeSplit[c + 1];
    const bool on = shadows_on_ && mode_ != Mode::Wireframe;
    sc.split[3] = on ? 1.0f : 0.0f;
    fill_light(sc.light);
    std::memcpy(sc.cluster, cluster_consts_, sizeof sc.cluster);
    sc.cluster[2][3] = (rt_shadows_ && rt_ok_ && mode_ != Mode::Wireframe) ? 1.0f : 0.0f;
    if (on) {
        compute_cascades(view);
        for (UINT c = 0; c < kCascades; ++c) {
            std::memcpy(sc.light_vp[c], cascades_[c].vp, sizeof sc.light_vp[c]);
            sc.texel[c] = cascades_[c].texel;
            sc.depth_range[c] = cascades_[c].depth_range;
        }
    }
    std::memcpy(shadow_cb_cpu_[frame_], &sc, sizeof sc);
    if (!on) return;

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = shadow_map_.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_->ResourceBarrier(1, &b);

    D3D12_VIEWPORT vp = {};
    vp.Width = float(kShadowRes);
    vp.Height = float(kShadowRes);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr = {0, 0, LONG(kShadowRes), LONG(kShadowRes)};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = mesher_.indices()->GetGPUVirtualAddress();
    ibv.SizeInBytes = kSlots * kSlotIndices * 4;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    const UINT dinc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT c = 0; c < kCascades; ++c) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadow_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(c) * dinc;
        cmd_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd_->RSSetViewports(1, &vp);
        cmd_->RSSetScissorRects(1, &sr);
        cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Terrain: every resident chunk, under the light.
        MeshConstants mc = {};
        std::memcpy(mc.view_proj, cascades_[c].vp, sizeof mc.view_proj);
        mc.slot_vertices = static_cast<float>(kSlotVertices);
        cmd_->SetDescriptorHeaps(1, heaps);
        cmd_->SetPipelineState(mesh_shadow_pso_.Get());
        cmd_->SetGraphicsRootSignature(mesh_root_.Get());
        cmd_->SetGraphicsRoot32BitConstants(0, sizeof(MeshConstants) / 4, &mc, 0);
        cmd_->SetGraphicsRootShaderResourceView(1, mesher_.vertices()->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(2, fade_buf_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(5, mesher_.vertex_materials()->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootConstantBufferView(3, shadow_cb_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootDescriptorTable(4, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        cmd_->IASetIndexBuffer(&ibv);
        cmd_->ExecuteIndirect(cmd_sig_.Get(), kSlots, mesher_.draw_args(), 0, nullptr, 0);

        // Trees, culled against this cascade but levelled by the camera's
        // distance, so a shadow always matches the tree that casts it.
        if (trees_on_ && trees_.ready()) {
            TreeShadowInputs si = {};
            std::memcpy(si.view_proj, cascades_[c].vp, sizeof si.view_proj);
            std::memcpy(si.planes, cascades_[c].planes, sizeof si.planes);
            for (int k = 0; k < 3; ++k) {
                si.cam_pos[k] = view.cam_pos[k];
                si.light_right[k] = cascades_[c].right[k];
                si.light_up[k] = cascades_[c].up[k];
            }
            si.time = view.time;
            si.far_plane = kFarPlane;
            trees_.record_shadow(cmd_.Get(), frame_, c, si, tree_settings_);
        }
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------
// The ray-tracing scene (ADR-004 L5): rt_scene.h for the acceleration
// structures, gi_trace.hlsli for the tracer, rt_shadow.hlsl for the trace.
// ---------------------------------------------------------------------------

void Renderer::set_rt_shadows(bool on) {
    rt_shadows_ = on;
    if (on) set_rt_active(true);
}

void Renderer::set_rt_active(bool on) {
    if (on && !rt_active_) rt_seed_ = true;
    rt_active_ = on;
}

bool Renderer::create_rt(std::string& error) {
    rt_ok_ = false;
    // Absent hardware or DXC is not an error: the engine runs on cascades.
    if (caps_.raytracing < D3D12_RAYTRACING_TIER_1_1 || !dxc().ok()) return true;
    std::string why;
    if (const char* dbg = std::getenv("BIOMES_RT_DBG")) rt_.debug_ = unsigned(std::atoi(dbg));
    if (!rt_.init(device_.Get(), why)) {
        startup_log().push_back(std::make_pair(std::string("ray tracing off: ") + why, 0.0));
        return true;
    }
    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width_;
    rd.Height = height_;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                  nullptr, IID_PPV_ARGS(&rt_vis_));
    if (FAILED(hr)) { error = hr_text("rt visibility", hr); return false; }
    auto buffer = [&](ComPtr<ID3D12Resource>& out, D3D12_HEAP_TYPE heap, UINT64 bytes, bool uav, D3D12_RESOURCE_STATES st) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!buffer(rt_stats_buf_, D3D12_HEAP_TYPE_DEFAULT, 64, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) { error = "rt stats"; return false; }
    for (UINT f = 0; f < kFrameCount; ++f) {
        if (!buffer(rt_stats_rb_[f], D3D12_HEAP_TYPE_READBACK, 64, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
            !buffer(rt_cb_[f], D3D12_HEAP_TYPE_UPLOAD, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ)) { error = "rt buffers"; return false; }
        rt_cb_[f]->Map(0, nullptr, &rt_cb_cpu_[f]);
    }
    {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 4 * kFrameCount;
        if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&rt_queries_)))) { error = "rt queries"; return false; }
        if (!buffer(rt_query_rb_, D3D12_HEAP_TYPE_READBACK, 4 * kFrameCount * 8, false, D3D12_RESOURCE_STATE_COPY_DEST)) { error = "rt query readback"; return false; }
    }
    // [36] the visibility SRV (post t10), [37] its UAV.
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c;
        h.ptr += SIZE_T(36) * inc;
        device_->CreateShaderResourceView(rt_vis_.Get(), &sd, h);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        h = c;
        h.ptr += SIZE_T(37) * inc;
        device_->CreateUnorderedAccessView(rt_vis_.Get(), nullptr, &ud, h);
    }
    // Root signature: constants, the shadow constants, the TLAS, the sun's
    // map, depth, the six geometry buffers gi_trace reads, the visibility
    // UAV, the stats.
    {
        D3D12_DESCRIPTOR_RANGE r_shadow = {}, r_depth = {}, r_vis = {};
        r_shadow.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r_shadow.NumDescriptors = 1; r_shadow.BaseShaderRegister = 8;
        r_depth.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;  r_depth.NumDescriptors = 1;  r_depth.BaseShaderRegister = 0;
        r_vis.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;    r_vis.NumDescriptors = 1;    r_vis.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER p[14] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].Descriptor.ShaderRegister = 30;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1; p[3].DescriptorTable.pDescriptorRanges = &r_shadow;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[4].DescriptorTable.NumDescriptorRanges = 1; p[4].DescriptorTable.pDescriptorRanges = &r_depth;
        for (int k = 0; k < 6; ++k) {
            p[5 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[5 + k].Descriptor.ShaderRegister = 31 + k;
        }
        p[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[11].DescriptorTable.NumDescriptorRanges = 1; p[11].DescriptorTable.pDescriptorRanges = &r_vis;
        p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[12].Descriptor.ShaderRegister = 1;
        p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[13].Descriptor.ShaderRegister = 37;   // tree uv, kind, seed
        for (int k = 0; k < 14; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC cmp = {};
        cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cmp.AddressU = cmp.AddressV = cmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        cmp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        cmp.MaxLOD = D3D12_FLOAT32_MAX;
        cmp.ShaderRegister = 1;
        cmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 14;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &cmp;
        ComPtr<ID3DBlob> sig, err;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            error = std::string("rt root signature: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rt_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (rt)", hr); return false; }
    }
    const char* entries[2] = {"cs_rt_shadow", "cs_rt_clear"};
    ComPtr<ID3D12PipelineState>* outs[2] = {&rt_pso_, &rt_clear_pso_};
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/rt_shadow.hlsl", entries[k], "cs_6_5", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = rt_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "rt compute pso";
            return false;
        }
    }
    rt_ok_ = true;
    return true;
}

void Renderer::record_rt_trace() {
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES nps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ib = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    const UINT q = UINT(frame_) * 4;

    // Last time this frame index ran: its stats and stamps have landed.
    if (rt_stats_pending_[frame_]) {
        UINT* s = nullptr;
        D3D12_RANGE rr = {0, 64};
        if (SUCCEEDED(rt_stats_rb_[frame_]->Map(0, &rr, reinterpret_cast<void**>(&s)))) {
            rt_stats_.lit_lit = s[0];
            rt_stats_.shadow_shadow = s[1];
            rt_stats_.rt_only = s[2];
            rt_stats_.csm_only = s[3];
            rt_stats_.primary_agree = s[4];
            rt_stats_.primary_nearer = s[5];
            rt_stats_.primary_farther = s[6];
            rt_stats_.normal_err_deg = s[8] ? float(double(s[7]) / 100.0 / double(s[8])) : 0.0f;
            D3D12_RANGE nr = {0, 0};
            rt_stats_rb_[frame_]->Unmap(0, &nr);
        }
        rt_stats_pending_[frame_] = false;
    }
    if (rt_query_pending_[frame_]) {
        UINT64* t = nullptr;
        D3D12_RANGE rr = {q * 8, (q + 4) * 8};
        if (SUCCEEDED(rt_query_rb_->Map(0, &rr, reinterpret_cast<void**>(&t)))) {
            UINT64 freq = 0;
            queue_->GetTimestampFrequency(&freq);
            const UINT64* s = t + q;
            const double k = freq ? 1000.0 / double(freq) : 0.0;
            auto ema = [](float& v, double x) { v = v == 0.0f ? float(x) : float(0.95 * v + 0.05 * x); };
            ema(rt_stats_.build_ms, double(s[1] - s[0]) * k);
            ema(rt_stats_.trace_ms, double(s[3] - s[2]) * k);
            D3D12_RANGE nr = {0, 0};
            rt_query_rb_->Unmap(0, &nr);
        }
        rt_query_pending_[frame_] = false;
    }

    RtConstants rc = {};
    invert4(view_proj_, rc.inv_view_proj);
    for (int k = 0; k < 3; ++k) {
        rc.cam_pos[k] = cur_cam_[k];
        rc.key_dir[k] = sky_frame_.key_dir[k];
    }
    rc.width = width_;
    rc.height = height_;
    rc.near_plane = kNearPlane;
    rc.far_plane = kFarPlane;
    rc.csm_reach = kCascadeSplit[kCascades] * 0.9f;
    rc.frame = UINT(post_frame_);
    rc.checks = rt_checks_ ? 1u : 0u;
    // The sun's angular radius, or the moon's: 0.2667 and 0.259 degrees.
    rc.light_radius = (sky_frame_.key_is_moon ? 0.259f : 0.2667f) * 0.0174533f;
    std::memcpy(rt_cb_cpu_[frame_], &rc, sizeof rc);

    tr(mesher_.indices(), ib, ib | nps);
    tr(shadow_map_.Get(), ps, ps | nps);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = [&](UINT i) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64(i) * inc;
        return g;
    };
    const D3D12_GPU_VIRTUAL_ADDRESS any = mesher_.vertices()->GetGPUVirtualAddress();
    cmd_->SetComputeRootSignature(rt_root_.Get());
    cmd_->SetComputeRootConstantBufferView(0, rt_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootConstantBufferView(1, shadow_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(2, rt_.tlas());
    cmd_->SetComputeRootDescriptorTable(3, at(0));
    cmd_->SetComputeRootDescriptorTable(4, at(3));
    cmd_->SetComputeRootShaderResourceView(5, mesher_.vertices()->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(6, mesher_.indices()->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(7, mesher_.vertex_materials()->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(8, rt_.tree_positions() ? rt_.tree_positions()->GetGPUVirtualAddress() : any);
    cmd_->SetComputeRootShaderResourceView(9, rt_.tree_indices() ? rt_.tree_indices()->GetGPUVirtualAddress() : any);
    cmd_->SetComputeRootShaderResourceView(10, rt_.tree_info() ? rt_.tree_info()->GetGPUVirtualAddress() : any);
    cmd_->SetComputeRootDescriptorTable(11, at(37));
    cmd_->SetComputeRootUnorderedAccessView(12, rt_stats_buf_->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(13, rt_.tree_attributes() ? rt_.tree_attributes()->GetGPUVirtualAddress() : any);
    cmd_->SetPipelineState(rt_clear_pso_.Get());
    cmd_->Dispatch(2, 1, 1);
    {
        D3D12_RESOURCE_BARRIER u = {};
        u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        u.UAV.pResource = rt_stats_buf_.Get();
        cmd_->ResourceBarrier(1, &u);
    }
    cmd_->EndQuery(rt_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 2);
    cmd_->SetPipelineState(rt_pso_.Get());
    cmd_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    tr(rt_vis_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nps);
    cmd_->EndQuery(rt_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 3);
    cmd_->ResolveQueryData(rt_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 4, rt_query_rb_.Get(), UINT64(q) * 8);
    rt_query_pending_[frame_] = true;
    tr(rt_stats_buf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd_->CopyBufferRegion(rt_stats_rb_[frame_].Get(), 0, rt_stats_buf_.Get(), 0, 64);
    tr(rt_stats_buf_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rt_stats_pending_[frame_] = true;
    tr(shadow_map_.Get(), ps | nps, ps);
    tr(mesher_.indices(), ib | nps, ib);
}

// ---------------------------------------------------------------------------
// Volumetric mist (ADR-004 L4): fog.h for where the mist is, fog.hlsl for the
// froxels, post.hlsl's composite for applying it.
// ---------------------------------------------------------------------------

bool Renderer::create_fog(std::string& error) {
    if (!fog_map_.build(world_, "worldgen/data/archetypes.json", error)) return false;

    // Timestamps for the post chain: five a frame per frame in flight.
    {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 5 * kFrameCount;
        HRESULT hr = device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&post_queries_));
        if (FAILED(hr)) { error = hr_text("post query heap", hr); return false; }
        D3D12_HEAP_PROPERTIES rh = {};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 5 * kFrameCount * 8;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&post_query_rb_));
        if (FAILED(hr)) { error = hr_text("post query readback", hr); return false; }
        queue_->GetTimestampFrequency(&post_query_freq_);
    }

    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto tex = [&](ComPtr<ID3D12Resource>& out, bool vol, UINT w, UINT h, UINT d, DXGI_FORMAT fmt, bool uav,
                   D3D12_RESOURCE_STATES state, const char* what) -> bool {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = UINT16(d);
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hr)) { error = hr_text(what, hr); return false; }
        return true;
    };
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT, f32 = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!tex(fog_map_tex_, false, FogMap::kSize, FogMap::kSize, 1, f32, false, D3D12_RESOURCE_STATE_COPY_DEST, "mist map")) return false;
    // Inject volume 0 starts as the one written, 1 as last frame's (read).
    if (!tex(fog_inject_[0], true, kFogX, kFogY, kFogZ, f16, true, ua, "mist inject 0")) return false;
    if (!tex(fog_inject_[1], true, kFogX, kFogY, kFogZ, f16, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "mist inject 1")) return false;
    if (!tex(fog_integrated_, true, kFogX, kFogY, kFogZ, f16, true, ua, "mist integrated")) return false;
    fog_cur_ = 0;
    fog_hist_valid_ = false;

    // The map's texels, staged for the first frame.
    {
        const D3D12_RESOURCE_DESC dd = fog_map_tex_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&dd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES uh = {};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&fog_map_up_));
        if (FAILED(hr)) { error = hr_text("mist map upload", hr); return false; }
        unsigned char* p = nullptr;
        fog_map_up_->Map(0, nullptr, reinterpret_cast<void**>(&p));
        const std::vector<float>& t = fog_map_.texels();
        for (int y = 0; y < FogMap::kSize; ++y)
            std::memcpy(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &t[size_t(y) * FogMap::kSize * 4],
                        size_t(FogMap::kSize) * 16);
        fog_map_up_->Unmap(0, nullptr);
        fog_uploaded_ = false;
    }

    // [29] map, [30..31] inject SRVs, [32] integrated SRV, [33..34] inject
    // UAVs, [35] integrated UAV.
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        c.ptr += SIZE_T(i) * inc;
        return c;
    };
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = f32;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(fog_map_tex_.Get(), &sd, slot(29));
        sd.Format = f16;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        device_->CreateShaderResourceView(fog_inject_[0].Get(), &sd, slot(30));
        device_->CreateShaderResourceView(fog_inject_[1].Get(), &sd, slot(31));
        device_->CreateShaderResourceView(fog_integrated_.Get(), &sd, slot(32));
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = f16;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = kFogZ;
        device_->CreateUnorderedAccessView(fog_inject_[0].Get(), nullptr, &ud, slot(33));
        device_->CreateUnorderedAccessView(fog_inject_[1].Get(), nullptr, &ud, slot(34));
        device_->CreateUnorderedAccessView(fog_integrated_.Get(), nullptr, &ud, slot(35));
    }

    for (UINT f = 0; f < kFrameCount; ++f) {
        D3D12_HEAP_PROPERTIES uh = {};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr, IID_PPV_ARGS(&fog_cb_[f]));
        if (FAILED(hr)) { error = hr_text("mist cb", hr); return false; }
        fog_cb_[f]->Map(0, nullptr, &fog_cb_cpu_[f]);
    }

    // Root signature: the mist's constants, the frame's shadow-and-light
    // constants, the lit pipelines' shadow table (sun map + local lights),
    // and one-descriptor tables for the map, the history, this frame's
    // inject volume read and written, and the integrated volume.
    {
        D3D12_DESCRIPTOR_RANGE shadow[2] = {};
        shadow[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadow[0].NumDescriptors = 1;
        shadow[0].BaseShaderRegister = 8;
        shadow[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadow[1].NumDescriptors = 5;
        shadow[1].BaseShaderRegister = 20;
        shadow[1].OffsetInDescriptorsFromTableStart = 24;
        D3D12_DESCRIPTOR_RANGE one[5] = {};
        const D3D12_DESCRIPTOR_RANGE_TYPE kinds[5] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                                      D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                                      D3D12_DESCRIPTOR_RANGE_TYPE_UAV};
        const UINT regs[5] = {0, 1, 2, 0, 1};
        D3D12_ROOT_PARAMETER p[8] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].Descriptor.ShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 2;
        p[2].DescriptorTable.pDescriptorRanges = shadow;
        for (int k = 0; k < 5; ++k) {
            one[k].RangeType = kinds[k];
            one[k].NumDescriptors = 1;
            one[k].BaseShaderRegister = regs[k];
            p[3 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[3 + k].DescriptorTable.NumDescriptorRanges = 1;
            p[3 + k].DescriptorTable.pDescriptorRanges = &one[k];
        }
        for (int k = 0; k < 8; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[2] = {};
        ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[0].MaxLOD = D3D12_FLOAT32_MAX;
        ss[0].ShaderRegister = 0;
        ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ss[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        ss[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        ss[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ss[1].MaxLOD = D3D12_FLOAT32_MAX;
        ss[1].ShaderRegister = 1;
        ss[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 8;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            error = std::string("mist root signature: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&fog_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (mist)", hr); return false; }
    }
    return create_fog_pipelines(error);
}

bool Renderer::create_fog_pipelines(std::string& error) {
    const char* entries[2] = {"cs_fog_inject", "cs_fog_integrate"};
    ComPtr<ID3D12PipelineState>* outs[2] = {&fog_inject_pso_, &fog_integrate_pso_};
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/fog.hlsl", entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = fog_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "mist compute pso";
            return false;
        }
    }
    return true;
}

void Renderer::record_fog() {
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES nps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!fog_uploaded_) {
        const D3D12_RESOURCE_DESC dd = fog_map_tex_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        device_->GetCopyableFootprints(&dd, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION d = {}, s = {};
        d.pResource = fog_map_tex_.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = fog_map_up_.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = fp;
        cmd_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        tr(fog_map_tex_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, nps);
        fog_uploaded_ = true;
    }

    // A camera that jumped starts the mist's history over, as TAA's does.
    const float jx = cur_cam_[0] - prev_cam_[0], jy = cur_cam_[1] - prev_cam_[1], jz = cur_cam_[2] - prev_cam_[2];
    if (jx * jx + jy * jy + jz * jz > 400.0f) fog_hist_valid_ = false;

    FogConstants fc = {};
    std::memcpy(fc.prev_view_proj, prev_vp_, sizeof fc.prev_view_proj);
    for (int k = 0; k < 3; ++k) {
        fc.cam_pos[k] = cur_cam_[k];
        fc.cam_right[k] = cur_right_[k];
        fc.cam_up[k] = cur_up_[k];
        fc.cam_fwd[k] = cur_fwd_[k];
    }
    fc.time = cur_time_;
    fc.tan_half_fov = cur_tan_;
    fc.aspect = cur_aspect_;
    // Where in its slice each froxel samples this frame: a Halton walk, which
    // the temporal blend turns into an average over the slice.
    fc.jitter = halton(int(post_frame_ % 16) + 1, 2);
    fc.near_z = kFogNear;
    fc.log_ratio = std::log(kFogFar / kFogNear);
    fc.history_valid = fog_hist_valid_ ? 1u : 0u;
    fc.time_factor = fog_map_.time_factor(time_hours_);
    const FogSettings& fs = fog_map_.settings();
    fc.map_extent = fog_map_.extent();
    fc.haze = fs.haze;
    fc.sea_level = world_.params().sea_level;
    fc.patch = fs.patch;
    fc.wind[0] = fs.wind[0];
    fc.wind[1] = fs.wind[1];
    fc.phase_g = fs.phase_g;
    fc.albedo = fs.albedo;
    fc.screen_w = float(width_);
    fc.screen_h = float(height_);
    fc.density_scale = fog_density_;
    std::memcpy(fog_cb_cpu_[frame_], &fc, sizeof fc);

    // The sun's shadow map and the light atlas are read by pixel shaders
    // elsewhere; this pass reads them from compute too.
    tr(shadow_map_.Get(), ps, ps | nps);
    tr(light_atlas_.Get(), ps, ps | nps);

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = [&](UINT i) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64(i) * inc;
        return g;
    };
    const UINT cur = fog_cur_, prev = fog_cur_ ^ 1u;
    cmd_->SetComputeRootSignature(fog_root_.Get());
    cmd_->SetComputeRootConstantBufferView(0, fog_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootConstantBufferView(1, shadow_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootDescriptorTable(2, at(0));
    cmd_->SetComputeRootDescriptorTable(3, at(29));
    cmd_->SetComputeRootDescriptorTable(4, at(30 + prev));      // last frame's inject volume
    cmd_->SetComputeRootDescriptorTable(5, at(30 + cur));       // this frame's, read by the integrate pass
    cmd_->SetComputeRootDescriptorTable(6, at(33 + cur));       // this frame's, written
    cmd_->SetComputeRootDescriptorTable(7, at(35));
    cmd_->SetPipelineState(fog_inject_pso_.Get());
    cmd_->Dispatch((kFogX + 7) / 8, (kFogY + 7) / 8, kFogZ);
    tr(fog_inject_[cur].Get(), ua, nps);
    if (post_queries_) cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(frame_) * 5 + 2);
    cmd_->SetPipelineState(fog_integrate_pso_.Get());
    cmd_->Dispatch((kFogX + 7) / 8, (kFogY + 7) / 8, 1);
    if (post_queries_) cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(frame_) * 5 + 3);
    tr(fog_integrated_.Get(), ua, nps);

    tr(shadow_map_.Get(), ps | nps, ps);
    tr(light_atlas_.Get(), ps | nps, ps);

    // Last frame's volume becomes the one written next frame.
    tr(fog_inject_[prev].Get(), nps, ua);
    fog_cur_ = prev;
    fog_hist_valid_ = fog_on_;
}

// ---------------------------------------------------------------------------
// Local lights (ADR-004 L3): local_lights.h for the model, lights.hlsl for
// the clusters, local_lights.hlsli for the shading.
// ---------------------------------------------------------------------------

bool Renderer::create_lights(std::string& error) {
    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uh = {};
    uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto buffer = [&](ComPtr<ID3D12Resource>& out, D3D12_HEAP_PROPERTIES& hp, UINT64 bytes, bool uav,
                      D3D12_RESOURCE_STATES state, const char* what) -> bool {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hr)) { error = hr_text(what, hr); return false; }
        return true;
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!buffer(lights_gpu_, dh, UINT64(kMaxLights) * sizeof(LocalLightGpu), false, read, "light list")) return false;
    if (!buffer(faces_gpu_, dh, UINT64(kMaxShadowLights) * 6 * sizeof(LightFaceGpu), false, read, "light faces")) return false;
    // Root UAVs: ALLOW_UNORDERED_ACCESS is not optional (CLAUDE.md).
    if (!buffer(cluster_n_, dh, UINT64(kClusterCount) * 4, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "cluster counts")) return false;
    if (!buffer(cluster_ix_, dh, UINT64(kClusterCount) * kClusterMaxItems * 4, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "cluster items")) return false;
    if (!buffer(cluster_stats_, dh, 16, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "cluster stats")) return false;
    cluster_readable_ = false;
    D3D12_HEAP_PROPERTIES rh = {};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    for (UINT f = 0; f < kFrameCount; ++f) {
        if (!buffer(lights_up_[f], uh, UINT64(kMaxLights) * sizeof(LocalLightGpu), false, D3D12_RESOURCE_STATE_GENERIC_READ, "light upload")) return false;
        if (!buffer(faces_up_[f], uh, UINT64(kMaxShadowLights) * 6 * sizeof(LightFaceGpu), false, D3D12_RESOURCE_STATE_GENERIC_READ, "face upload")) return false;
        if (!buffer(cluster_cb_[f], uh, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ, "cluster cb")) return false;
        if (!buffer(stats_rb_[f], rh, 16, false, D3D12_RESOURCE_STATE_COPY_DEST, "cluster stats readback")) return false;
        lights_up_[f]->Map(0, nullptr, &lights_up_cpu_[f]);
        faces_up_[f]->Map(0, nullptr, &faces_up_cpu_[f]);
        cluster_cb_[f]->Map(0, nullptr, &cluster_cb_cpu_[f]);
        stats_pending_[f] = false;
    }

    // The shadow atlas: 4096^2 of depth, 64 tiles of 512^2.
    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kLightAtlas;
        rd.Height = kLightAtlas;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        HRESULT hr = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      &cv, IID_PPV_ARGS(&light_atlas_));
        if (FAILED(hr)) { error = hr_text("light shadow atlas", hr); return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&light_dsv_heap_));
        if (FAILED(hr)) { error = hr_text("light atlas dsv heap", hr); return false; }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(light_atlas_.Get(), &dv, light_dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    }

    // Descriptors [24..28]: t20 lights, t21 counts, t22 items, t23 atlas, t24 faces.
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        c.ptr += SIZE_T(i) * inc;
        return c;
    };
    auto sbuf = [&](ID3D12Resource* r, UINT n, UINT stride, UINT i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = n;
        sd.Buffer.StructureByteStride = stride;
        device_->CreateShaderResourceView(r, &sd, slot(i));
    };
    sbuf(lights_gpu_.Get(), kMaxLights, sizeof(LocalLightGpu), 24);
    sbuf(cluster_n_.Get(), kClusterCount, 4, 25);
    sbuf(cluster_ix_.Get(), kClusterCount * kClusterMaxItems, 4, 26);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(light_atlas_.Get(), &sd, slot(27));
    }
    sbuf(faces_gpu_.Get(), kMaxShadowLights * 6, sizeof(LightFaceGpu), 28);

    // The cluster pass: constants, the light list, three UAVs.
    {
        D3D12_ROOT_PARAMETER p[5] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        for (int k = 0; k < 3; ++k) {
            p[2 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[2 + k].Descriptor.ShaderRegister = k;
        }
        for (int k = 0; k < 5; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) { error = "cluster root signature"; return false; }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&cluster_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (clusters)", hr); return false; }
    }
    for (UINT i = 0; i < kMaxShadowLights; ++i) shadow_slots_[i] = ShadowSlot();
    return create_lights_pipelines(error);
}

bool Renderer::create_lights_pipelines(std::string& error) {
    const char* entries[2] = {"cs_cluster", "cs_cluster_clear"};
    ComPtr<ID3D12PipelineState>* outs[2] = {&cluster_pso_, &cluster_clear_pso_};
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/lights.hlsl", entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = cluster_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "cluster compute pso";
            return false;
        }
    }
    return true;
}

namespace {
// A flame's flicker: three incommensurate sines, about +-20% at worst. A
// light with flicker 0 is steady.
float flame(float t, uint32_t seed, float amount) {
    const float s = float(seed % 997u) * 0.37f;
    return 1.0f + amount * (0.10f * std::sin(t * 8.3f + s) + 0.06f * std::sin(t * 13.7f + s * 1.7f) +
                            0.04f * std::sin(t * 23.1f + s * 2.3f));
}
}  // namespace

void Renderer::record_lights(const ViewConstants& view) {
    // The stats this frame index last wrote have landed: the frame's fence
    // was waited for before its allocator came back.
    if (stats_pending_[frame_]) {
        void* p = nullptr;
        D3D12_RANGE rr = {0, 16};
        if (SUCCEEDED(stats_rb_[frame_]->Map(0, &rr, &p))) {
            const UINT* s = static_cast<const UINT*>(p);
            light_stats_.overflowed = s[0];
            light_stats_.max_in_cluster = s[1];
            light_stats_.clusters_lit = s[2];
            light_stats_.cluster_items = s[3];
            D3D12_RANGE nr = {0, 0};
            stats_rb_[frame_]->Unmap(0, &nr);
        }
        stats_pending_[frame_] = false;
    }

    // --- this frame's lights: the torch at the camera, then the placed ones
    struct Active { LocalLight l; float importance; };
    std::vector<Active> act;
    act.reserve(lights_.size() + 1);
    const float* cam = view.cam_pos;
    if (torch_on_) {
        Active a;
        a.l = make_torch();
        a.l.id = 0;
        // In the right hand, a little low and ahead of the eye. The flame
        // wanders a centimetre or two, which the shadows follow: they are
        // redrawn every frame for a light that moves anyway.
        const float t = view.time;
        for (int k = 0; k < 3; ++k)
            a.l.pos[k] = cam[k] + view.cam_right[k] * 0.35f - view.cam_up[k] * 0.30f + view.cam_fwd[k] * 0.45f;
        a.l.pos[0] += 0.012f * std::sin(t * 9.1f);
        a.l.pos[1] += 0.015f * std::sin(t * 11.3f + 1.0f);
        a.l.pos[2] += 0.012f * std::sin(t * 7.7f + 2.0f);
        a.importance = 1e30f;   // the light in your hand is always the one that matters
        act.push_back(a);
    }
    for (size_t i = 0; i < lights_.size(); ++i) {
        Active a;
        a.l = lights_[i];
        const float dx = a.l.pos[0] - cam[0], dy = a.l.pos[1] - cam[1], dz = a.l.pos[2] - cam[2];
        a.importance = a.l.lumens / (std::max)(dx * dx + dy * dy + dz * dz, 1.0f);
        act.push_back(a);
    }
    std::stable_sort(act.begin(), act.end(), [](const Active& x, const Active& y) { return x.importance > y.importance; });
    if (act.size() > kMaxLights) act.resize(kMaxLights);

    // --- shadows: the most important shadowed lights keep or take a slot
    light_stats_.lights = UINT(act.size());
    light_stats_.shadowed = 0;
    light_stats_.faces_drawn = 0;
    light_stats_.chunk_draws = 0;
    bool wanted[kMaxShadowLights] = {};
    std::vector<int> slot_of(act.size(), -1);
    if (light_shadows_on_) {
        std::vector<size_t> pick;
        for (size_t i = 0; i < act.size() && pick.size() < kMaxShadowLights; ++i)
            if (act[i].l.shadow) pick.push_back(i);
        // Keep the slots of lights still picked: their faces are cached there.
        for (size_t q = 0; q < pick.size(); ++q)
            for (UINT s = 0; s < kMaxShadowLights; ++s)
                if (shadow_slots_[s].light == act[pick[q]].l.id) { slot_of[pick[q]] = int(s); wanted[s] = true; }
        for (size_t q = 0; q < pick.size(); ++q) {
            if (slot_of[pick[q]] >= 0) continue;
            for (UINT s = 0; s < kMaxShadowLights; ++s) {
                if (wanted[s]) continue;
                shadow_slots_[s] = ShadowSlot();
                shadow_slots_[s].light = act[pick[q]].l.id;
                slot_of[pick[q]] = int(s);
                wanted[s] = true;
                break;
            }
        }
    }
    for (UINT s = 0; s < kMaxShadowLights; ++s) if (!wanted[s]) shadow_slots_[s] = ShadowSlot();

    // Which slots draw their faces this frame: never drawn or moved first
    // (most important first, as `act` is sorted), then the stalest -- a
    // static light's faces are redrawn every few seconds so a dug hole or a
    // felled tree reaches its shadow -- within twelve faces.
    std::vector<size_t> redraw;
    {
        UINT budget = kMaxFacesPerFrame;
        std::vector<size_t> stale;
        for (size_t i = 0; i < act.size(); ++i) {
            if (slot_of[i] < 0) continue;
            ShadowSlot& sl = shadow_slots_[slot_of[i]];
            const float dx = act[i].l.pos[0] - sl.pos[0], dy = act[i].l.pos[1] - sl.pos[1], dz = act[i].l.pos[2] - sl.pos[2];
            const bool moved = dx * dx + dy * dy + dz * dz > 1e-4f;
            const int faces = act[i].l.type == 1 ? 1 : 6;
            if (!sl.ready || moved) {
                if (UINT(faces) <= budget) { redraw.push_back(i); budget -= faces; }
            } else if (post_frame_ - sl.drawn > 240) {
                stale.push_back(i);
            }
        }
        std::sort(stale.begin(), stale.end(), [&](size_t x, size_t y) {
            return shadow_slots_[slot_of[x]].drawn < shadow_slots_[slot_of[y]].drawn;
        });
        for (size_t q = 0; q < stale.size(); ++q) {
            const int faces = act[stale[q]].l.type == 1 ? 1 : 6;
            if (UINT(faces) <= budget) { redraw.push_back(stale[q]); budget -= faces; }
        }
    }

    // --- the faces' matrices, for what is redrawn
    static const float kAxis[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float kUp[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
    struct FaceDraw { UINT tile; float vp[16]; float right[3], up[3]; float pos[3]; float range; };
    std::vector<FaceDraw> draws;
    for (size_t q = 0; q < redraw.size(); ++q) {
        const LocalLight& L = act[redraw[q]].l;
        const int s = slot_of[redraw[q]];
        const int faces = L.type == 1 ? 1 : 6;
        for (int fc = 0; fc < faces; ++fc) {
            FaceDraw d;
            d.tile = UINT(s) * 6 + UINT(fc);
            const float* fwd = L.type == 1 ? L.spot_dir : kAxis[fc];
            const float* uph = L.type == 1 ? kUp[0] : kUp[fc];
            const float fov = L.type == 1 ? (std::min)(2.0f * L.spot_outer_deg + 10.0f, 150.0f) : 95.0f;
            light_face(L.pos, fwd, uph, fov, L.range, d.vp, d.right, d.up);
            for (int k = 0; k < 3; ++k) d.pos[k] = L.pos[k];
            d.range = L.range;
            LightFaceGpu& g = faces_cpu_[d.tile];
            std::memcpy(g.view_proj, d.vp, sizeof g.view_proj);
            const float sz = float(kLightTile) / float(kLightAtlas);
            g.rect[0] = float(d.tile % (kLightAtlas / kLightTile)) * sz;
            g.rect[1] = float(d.tile / (kLightAtlas / kLightTile)) * sz;
            g.rect[2] = sz;
            g.rect[3] = 1.0f / float(kLightAtlas);
            draws.push_back(d);
        }
        ShadowSlot& sl = shadow_slots_[s];
        sl.ready = true;
        for (int k = 0; k < 3; ++k) sl.pos[k] = L.pos[k];
        sl.drawn = post_frame_;
        sl.faces = faces;
    }

    // --- the GPU records
    LocalLightGpu* gl = static_cast<LocalLightGpu*>(lights_up_cpu_[frame_]);
    for (size_t i = 0; i < act.size(); ++i) {
        const LocalLight& L = act[i].l;
        LocalLightGpu g = {};
        const float cd = L.lumens / (4.0f * 3.14159265f) * flame(view.time, L.id * 2654435761u + 17u, L.flicker);
        for (int k = 0; k < 3; ++k) {
            g.pos[k] = L.pos[k];
            g.intensity[k] = L.color[k] * cd;
            g.spot_dir[k] = L.spot_dir[k];
        }
        g.range = L.range;
        g.type = float(L.type);
        g.cos_outer = std::cos(L.spot_outer_deg * 3.14159265f / 180.0f);
        g.cos_inner = std::cos(L.spot_inner_deg * 3.14159265f / 180.0f);
        g.shadow = (slot_of[i] >= 0 && shadow_slots_[slot_of[i]].ready) ? slot_of[i] * 6 : -1;
        if (g.shadow >= 0) ++light_stats_.shadowed;
        gl[i] = g;
    }
    std::memcpy(faces_up_cpu_[frame_], faces_cpu_, sizeof faces_cpu_);

    // --- the clusters' constants, for the cluster pass and every lit shader
    const float lscale = float(kClusterZ) / std::log(kClusterFar / kClusterNear);
    ClusterConstants cc = {};
    for (int k = 0; k < 3; ++k) {
        cc.cam_pos[k] = cam[k];
        cc.cam_right[k] = view.cam_right[k];
        cc.cam_up[k] = view.cam_up[k];
        cc.cam_fwd[k] = view.cam_fwd[k];
    }
    cc.light_count = UINT(act.size());
    cc.tan_half_fov = view.tan_half_fov;
    cc.aspect = view.aspect;
    cc.near_z = kClusterNear;
    cc.far_z = kClusterFar;
    cc.log_scale = lscale;
    std::memcpy(cluster_cb_cpu_[frame_], &cc, sizeof cc);
    cluster_consts_[0][0] = float(kClusterX); cluster_consts_[0][1] = float(kClusterY);
    cluster_consts_[0][2] = float(kClusterZ); cluster_consts_[0][3] = float(act.size());
    cluster_consts_[1][0] = kClusterNear;     cluster_consts_[1][1] = lscale;
    cluster_consts_[1][2] = float(width_);    cluster_consts_[1][3] = float(height_);
    for (int k = 0; k < 3; ++k) cluster_consts_[2][k] = view.cam_fwd[k];
    cluster_consts_[2][3] = 0.0f;

    // --- copies, the cluster pass, its stats
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_->ResourceBarrier(1, &rb);
    };
    auto uavb = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = r;
        cmd_->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    tr(lights_gpu_.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
    tr(faces_gpu_.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!act.empty())
        cmd_->CopyBufferRegion(lights_gpu_.Get(), 0, lights_up_[frame_].Get(), 0, UINT64(act.size()) * sizeof(LocalLightGpu));
    cmd_->CopyBufferRegion(faces_gpu_.Get(), 0, faces_up_[frame_].Get(), 0, sizeof faces_cpu_);
    tr(lights_gpu_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    tr(faces_gpu_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    if (cluster_readable_) {
        tr(cluster_n_.Get(), read, ua);
        tr(cluster_ix_.Get(), read, ua);
    }
    cmd_->SetComputeRootSignature(cluster_root_.Get());
    cmd_->SetComputeRootConstantBufferView(0, cluster_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootShaderResourceView(1, lights_gpu_->GetGPUVirtualAddress());
    cmd_->SetComputeRootUnorderedAccessView(2, cluster_n_->GetGPUVirtualAddress());
    cmd_->SetComputeRootUnorderedAccessView(3, cluster_ix_->GetGPUVirtualAddress());
    cmd_->SetComputeRootUnorderedAccessView(4, cluster_stats_->GetGPUVirtualAddress());
    cmd_->SetPipelineState(cluster_clear_pso_.Get());
    cmd_->Dispatch(1, 1, 1);
    uavb(cluster_stats_.Get());
    cmd_->SetPipelineState(cluster_pso_.Get());
    cmd_->Dispatch((kClusterCount + 63) / 64, 1, 1);
    uavb(cluster_stats_.Get());
    tr(cluster_n_.Get(), ua, read);
    tr(cluster_ix_.Get(), ua, read);
    cluster_readable_ = true;
    tr(cluster_stats_.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd_->CopyBufferRegion(stats_rb_[frame_].Get(), 0, cluster_stats_.Get(), 0, 16);
    tr(cluster_stats_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ua);
    stats_pending_[frame_] = true;

    if (draws.empty()) return;

    // --- the shadow faces: the chunks in each light's reach, and the trees
    tr(light_atlas_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = light_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    cmd_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = mesher_.indices()->GetGPUVirtualAddress();
    ibv.SizeInBytes = kSlots * kSlotIndices * 4;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    std::vector<UINT> slots;
    for (size_t q = 0; q < draws.size(); ++q) {
        const FaceDraw& d = draws[q];
        const UINT per_row = kLightAtlas / kLightTile;
        const LONG x0 = LONG((d.tile % per_row) * kLightTile), y0 = LONG((d.tile / per_row) * kLightTile);
        D3D12_RECT rc = {x0, y0, x0 + LONG(kLightTile), y0 + LONG(kLightTile)};
        D3D12_VIEWPORT vp = {};
        vp.TopLeftX = float(x0);
        vp.TopLeftY = float(y0);
        vp.Width = float(kLightTile);
        vp.Height = float(kLightTile);
        vp.MaxDepth = 1.0f;
        cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &rc);
        cmd_->RSSetViewports(1, &vp);
        cmd_->RSSetScissorRects(1, &rc);
        cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        MeshConstants mc = {};
        std::memcpy(mc.view_proj, d.vp, sizeof mc.view_proj);
        mc.slot_vertices = static_cast<float>(kSlotVertices);
        cmd_->SetDescriptorHeaps(1, heaps);
        cmd_->SetPipelineState(mesh_shadow_pso_.Get());
        cmd_->SetGraphicsRootSignature(mesh_root_.Get());
        cmd_->SetGraphicsRoot32BitConstants(0, sizeof(MeshConstants) / 4, &mc, 0);
        cmd_->SetGraphicsRootShaderResourceView(1, mesher_.vertices()->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(2, fade_buf_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(5, mesher_.vertex_materials()->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootConstantBufferView(3, shadow_cb_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootDescriptorTable(4, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        cmd_->IASetIndexBuffer(&ibv);
        streamer_.slots_near(d.pos, d.range, slots);
        for (size_t k = 0; k < slots.size(); ++k)
            cmd_->ExecuteIndirect(cmd_sig_.Get(), 1, mesher_.draw_args(),
                                  UINT64(slots[k]) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
        light_stats_.chunk_draws += UINT(slots.size());
        if (trees_on_ && trees_.ready()) {
            TreeShadowInputs si = {};
            std::memcpy(si.view_proj, d.vp, sizeof si.view_proj);
            planes_of(d.vp, si.planes);
            for (int k = 0; k < 3; ++k) {
                si.cam_pos[k] = cam[k];
                si.light_right[k] = d.right[k];
                si.light_up[k] = d.up[k];
            }
            si.time = view.time;
            si.far_plane = kFarPlane;
            trees_.record_light_shadow(cmd_.Get(), frame_, UINT(q), si, tree_settings_);
            // The tree pass sets its own targets' state; put ours back.
            cmd_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        }
        ++light_stats_.faces_drawn;
    }
    tr(light_atlas_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ---------------------------------------------------------------------------
// The atmosphere (ADR-004 L2). The static tables are built on the CPU by the
// same code the GPU samples them with (sky.cpp, atmosphere_march.h) and
// uploaded on the first frame; the sky-view table and the aerial-perspective
// volume are rebuilt every frame (atmosphere.hlsl).
// ---------------------------------------------------------------------------

void Renderer::set_time_of_day(float hours) {
    hours = std::fmod(hours, 24.0f);
    if (hours < 0.0f) hours += 24.0f;
    time_hours_ = hours;
}

void Renderer::update_sky(float cam_y) {
    // Re-evaluate when the hour or the camera's height moved enough to show:
    // the evaluation integrates the sky over the hemisphere (~0.3 ms).
    if (std::fabs(time_hours_ - sky_hours_) < 1e-4f && std::fabs(cam_y - sky_cam_y_) < 5.0f) return;
    sky_frame_ = sky_.evaluate(time_hours_, cam_y);
    sky_hours_ = time_hours_;
    sky_cam_y_ = cam_y;
}

void Renderer::fill_light(float out[7][4]) const {
    const SkyFrame& f = sky_frame_;
    const float* src[7] = {f.key_dir, f.key_lux, f.sky_mean, f.ground, f.zenith, f.horizon_sun, f.horizon_away};
    for (int i = 0; i < 7; ++i) {
        for (int k = 0; k < 3; ++k) out[i][k] = src[i][k];
        out[i][3] = 0.0f;
    }
}

bool Renderer::create_atmosphere(std::string& error) {
    const double t0 = startup_now_ms();
    if (!sky_.built()) sky_.build();
    startup_log().push_back(std::make_pair(std::string("phase: atmosphere tables (CPU)"), startup_now_ms() - t0));
    update_sky(0.0f);

    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto tex = [&](ComPtr<ID3D12Resource>& out, bool vol, UINT w, UINT h, UINT d, DXGI_FORMAT fmt, bool uav,
                   D3D12_RESOURCE_STATES state, const char* what) -> bool {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = UINT16(d);
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                            IID_PPV_ARGS(&out));
        if (FAILED(hr)) { error = hr_text(what, hr); return false; }
        return true;
    };
    const DXGI_FORMAT f32 = DXGI_FORMAT_R32G32B32A32_FLOAT, f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!tex(trans_tex_, false, kAtmTransW, kAtmTransH, 1, f32, false, D3D12_RESOURCE_STATE_COPY_DEST, "transmittance table")) return false;
    if (!tex(ms_tex_, false, kAtmMsSize, kAtmMsSize, 1, f32, false, D3D12_RESOURCE_STATE_COPY_DEST, "multiple-scattering table")) return false;
    if (!tex(sky_view_tex_, false, kAtmSkyW, kAtmSkyH, 1, f16, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "sky-view table")) return false;
    if (!tex(ap_tex_, true, 32, 32, 32, f16, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "aerial-perspective volume")) return false;

    // Upload buffers for the two static tables, filled now, copied on the
    // first frame (record_atmosphere).
    auto stage = [&](ComPtr<ID3D12Resource>& up, ID3D12Resource* dst, const std::vector<float>& src, UINT w, UINT h) -> bool {
        const D3D12_RESOURCE_DESC dd = dst->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&dd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES uh = {};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr, IID_PPV_ARGS(&up));
        if (FAILED(hr)) { error = hr_text("atmosphere upload", hr); return false; }
        unsigned char* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < h; ++y)
            std::memcpy(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &src[size_t(y) * w * 4], size_t(w) * 16);
        up->Unmap(0, nullptr);
        return true;
    };
    if (!stage(trans_up_, trans_tex_.Get(), sky_.transmittance(), kAtmTransW, kAtmTransH)) return false;
    if (!stage(ms_up_, ms_tex_.Get(), sky_.multiple_scattering(), kAtmMsSize, kAtmMsSize)) return false;
    atm_uploaded_ = false;

    // Descriptors: [10] sky-view and [11] the volume for the post chain,
    // [16..17] the static tables and [18..19] the per-frame UAVs for ours.
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        c.ptr += SIZE_T(i) * inc;
        return c;
    };
    auto srv2 = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &sd, slot(i));
    };
    srv2(sky_view_tex_.Get(), f16, 10);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = f16;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture3D.MipLevels = 1;
        device_->CreateShaderResourceView(ap_tex_.Get(), &sd, slot(11));
    }
    srv2(trans_tex_.Get(), f32, 16);
    srv2(ms_tex_.Get(), f32, 17);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = f16;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(sky_view_tex_.Get(), nullptr, &ud, slot(18));
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = 32;
        device_->CreateUnorderedAccessView(ap_tex_.Get(), nullptr, &ud, slot(19));
    }

    for (UINT f = 0; f < kFrameCount; ++f) {
        D3D12_HEAP_PROPERTIES uh = {};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr, IID_PPV_ARGS(&atm_cb_[f]));
        if (FAILED(hr)) { error = hr_text("atmosphere cb", hr); return false; }
        atm_cb_[f]->Map(0, nullptr, &atm_cb_cpu_[f]);
    }

    {
        D3D12_DESCRIPTOR_RANGE sr = {};
        sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr.NumDescriptors = 2;
        D3D12_DESCRIPTOR_RANGE ur = {};
        ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ur.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &sr;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ur;
        for (int k = 0; k < 3; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &ss;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) { error = "atmosphere root signature"; return false; }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&atm_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (atmosphere)", hr); return false; }
    }
    return create_atmosphere_pipelines(error);
}

bool Renderer::create_atmosphere_pipelines(std::string& error) {
    const char* kAtm = "engine/render/shaders/atmosphere.hlsl";
    const char* entries[2] = {"cs_sky_view", "cs_aerial"};
    ComPtr<ID3D12PipelineState>* outs[2] = {&sky_view_pso_, &aerial_pso_};
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kAtm, entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = atm_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "atmosphere compute pso";
            return false;
        }
    }
    return true;
}

void Renderer::record_atmosphere() {
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_->ResourceBarrier(1, &rb);
    };
    if (!atm_uploaded_) {
        ID3D12Resource* dst[2] = {trans_tex_.Get(), ms_tex_.Get()};
        ID3D12Resource* src[2] = {trans_up_.Get(), ms_up_.Get()};
        for (int k = 0; k < 2; ++k) {
            const D3D12_RESOURCE_DESC dd = dst[k]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            device_->GetCopyableFootprints(&dd, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
            D3D12_TEXTURE_COPY_LOCATION d = {}, s = {};
            d.pResource = dst[k];
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            s.pResource = src[k];
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp;
            cmd_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            tr(dst[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        atm_uploaded_ = true;
    }

    AtmosphereConstants ac = {};
    invert4(view_proj_, ac.inv_view_proj);
    for (int k = 0; k < 3; ++k) {
        ac.cam_pos[k] = cur_cam_[k];
        ac.sun_dir[k] = sky_frame_.sun_dir[k];
        ac.sun_lux[k] = sky_frame_.sun_lux_top[k];
        ac.moon_dir[k] = sky_frame_.moon_dir[k];
        ac.moon_lux[k] = sky_frame_.moon_lux_top[k];
    }
    ac.cam_alt_km = sky_frame_.cam_alt_km;
    ac.far_plane = kFarPlane;
    std::memcpy(atm_cb_cpu_[frame_], &ac, sizeof ac);

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE srvs = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    srvs.ptr += UINT64(16) * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE uavs = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    uavs.ptr += UINT64(18) * inc;
    cmd_->SetComputeRootSignature(atm_root_.Get());
    cmd_->SetComputeRootConstantBufferView(0, atm_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootDescriptorTable(1, srvs);
    cmd_->SetComputeRootDescriptorTable(2, uavs);
    cmd_->SetPipelineState(sky_view_pso_.Get());
    cmd_->Dispatch((kAtmSkyW + 7) / 8, (kAtmSkyH + 7) / 8, 1);
    cmd_->SetPipelineState(aerial_pso_.Get());
    cmd_->Dispatch(4, 4, 1);
}

bool Renderer::reload_shaders(std::string& error) {
    if (ready_) wait_for_gpu();
    if (!mesher_.reload_shaders(device_.Get(), error)) return false;
    if (!create_mesh_pipeline(error)) return false;
    if (!create_water_pipeline(error)) return false;
    if (!create_post_pipelines(error)) return false;
    if (!create_atmosphere_pipelines(error)) return false;
    if (!create_lights_pipelines(error)) return false;
    if (fog_root_ && !create_fog_pipelines(error)) return false;
    // Everything resident was built by the old kernel, so start again.
    streamer_.init();
    return true;
}

bool Renderer::init(HWND hwnd, std::string& error, bool debug_layer) {
    double t0 = startup_now_ms();
    if (!create_device(error, debug_layer)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: device"), startup_now_ms() - t0));
    caps_ = query_caps(device_.Get());
    startup_log().push_back(std::make_pair(std::string("device: ") + describe(caps_), 0.0));
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

    if (!create_depth(error)) return false;
    if (!create_shadow_resources(error)) return false;
    if (!create_atmosphere(error)) return false;
    if (!create_lights(error)) return false;
    if (!create_post_targets(error)) return false;
    if (!create_post_pipelines(error)) return false;

    UINT64 tsfreq = 0;
    queue_->GetTimestampFrequency(&tsfreq);
    t0 = startup_now_ms();
    if (!mesher_.init(device_.Get(), tsfreq, error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: meshing kernels + buffers"), startup_now_ms() - t0));
    t0 = startup_now_ms();
    if (!create_mesh_pipeline(error)) return false;
    if (!create_water_pipeline(error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: draw pipelines"), startup_now_ms() - t0));
    streamer_.init();
    pager_.init(kMaxBricks);

    // M1: build the seed and upload it before anything is meshed. The world
    // is static for the run, so this is the only place it happens.
    allocators_[frame_]->Reset();
    cmd_->Reset(allocators_[frame_].Get(), nullptr);
    t0 = startup_now_ms();
    if (!world_.build(device_.Get(), cmd_.Get(), 0, error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: world build (CPU seed + upload)"), startup_now_ms() - t0));
    mesher_.set_world(&world_);
    {
        float lo = 0.0f, hi = 0.0f;
        world_.band(lo, hi);
        streamer_.set_band(lo, hi);
        streamer_.set_bounds(&world_);
    }

    // Every slot starts retired, so a slot that has never been meshed draws
    // nothing instead of whatever the allocation happened to contain.
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
    world_.release_staging();

    // Trees: models, the impostor bake, and the CPU placement workers.
    t0 = startup_now_ms();
    {
        std::vector<TreeLook> looks;
        if (!load_tree_looks(world_.table(), "worldgen/data/archetypes.json", looks, error)) return false;
        if (!trees_.init(device_.Get(), queue_.Get(), looks, srv_heap_.Get(), 1, error)) return false;
        // Three workers: the first fill of a 500 m radius is ~300 tiles, and
        // the frame loop never waits on them.
        tree_field_.init(&world_, 3);
    }
    startup_log().push_back(std::make_pair(std::string("phase: tree models + impostor bake"), startup_now_ms() - t0));
    t0 = startup_now_ms();
    if (!foliage_.init(device_.Get(), &world_, "worldgen/data/archetypes.json", error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: foliage pipelines"), startup_now_ms() - t0));
    t0 = startup_now_ms();
    if (!create_fog(error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: mist map + volumes"), startup_now_ms() - t0));
    t0 = startup_now_ms();
    if (!create_rt(error)) return false;
    startup_log().push_back(std::make_pair(std::string("phase: ray-tracing scene"), startup_now_ms() - t0));

    ready_ = true;
    return true;
}

void Renderer::update_trees(const ViewConstants& view, bool settle) {
    if (!trees_.ready()) return;
    // A tile beyond the draw distance is never drawn, but one tile of margin
    // keeps a turn of the head from waiting on placement.
    const float radius = tree_settings_.max_dist + 64.0f;
    tree_field_.update(view.cam_pos[0], view.cam_pos[2], radius);
    if (settle) {
        // A still waits for every tile, the way it waits for every chunk.
        for (int i = 0; i < 4000 && !tree_field_.settled(); ++i) {
            Sleep(2);
            tree_field_.update(view.cam_pos[0], view.cam_pos[2], radius);
        }
    }
    if (tree_field_.version() == tree_field_seen_ && !tree_dirty_) return;
    tree_field_seen_ = tree_field_.version();
    tree_dirty_ = false;
    const std::vector<worldgen::TreeInstance>& st = tree_field_.standing();
    tree_all_.clear();
    tree_all_.reserve(st.size() + tree_extras_.size());
    for (size_t i = 0; i < st.size(); ++i) {
        const worldgen::TreeInstance& t = st[i];
        TreeGpuInstance g = {};
        g.pos[0] = t.x; g.pos[1] = t.y; g.pos[2] = t.z;
        g.scale = t.height / trees_.look(t.species).nominal_height;
        tree_standing_rotation(t, g.rot);
        g.model = t.species * kTreeVariants + (t.variant % kTreeVariants);
        g.mode = 0;
        std::unordered_map<uint32_t, float>::const_iterator h = tree_hits_.find(t.id);
        g.hit_time = h == tree_hits_.end() ? -100.0f : h->second;
        g.id = t.id;
        tree_all_.push_back(g);
    }
    tree_all_.insert(tree_all_.end(), tree_extras_.begin(), tree_extras_.end());
    trees_.set_instances(tree_all_);
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
    create_post_targets(ignored);
    hist_valid_ = false;
}


// --- the post chain ------------------------------------------------------------

bool Renderer::create_post_targets(std::string& error) {
    if (!post_rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&post_rtv_heap_)))) {
            error = "post rtv heap";
            return false;
        }
    }
    const UINT w = width_ ? width_ : 1, h = height_ ? height_ : 1;
    auto tex = [&](ComPtr<ID3D12Resource>& out, UINT tw, UINT th, DXGI_FORMAT fmt,
                   D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const float* clear) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = tw;
        rd.Height = th;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = flags;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = fmt;
        if (clear) for (int k = 0; k < 4; ++k) cv.Color[k] = clear[k];
        out.Reset();
        return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                          clear ? &cv : nullptr, IID_PPV_ARGS(&out)));
    };
    const float zero[4] = {0, 0, 0, 1};
    const float sky[4] = {kSkyClearR, kSkyClearG, kSkyClearB, 1.0f};
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!tex(hdr_direct_, w, h, kSceneFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
             D3D12_RESOURCE_STATE_RENDER_TARGET, zero) ||
        !tex(hdr_ambient_, w, h, kSceneFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
             D3D12_RESOURCE_STATE_RENDER_TARGET, sky) ||
        !tex(scene_, w, h, kSceneFormat, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr) ||
        !tex(ao_, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R8_UNORM, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr) ||
        !tex(history_[0], w, h, kSceneFormat, uav, read, nullptr) ||
        !tex(history_[1], w, h, kSceneFormat, uav, read, nullptr)) {
        error = "post targets";
        return false;
    }
    const UINT rinc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE r = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(hdr_direct_.Get(), nullptr, r);
    r.ptr += rinc;
    device_->CreateRenderTargetView(hdr_ambient_.Get(), nullptr, r);

    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        c.ptr += SIZE_T(i) * inc;
        return c;
    };
    auto srv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(res, &sd, slot(i));
    };
    auto uavd = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(res, nullptr, &ud, slot(i));
    };
    srv(depth_.Get(), DXGI_FORMAT_R32_FLOAT, 3);
    srv(hdr_direct_.Get(), kSceneFormat, 4);
    srv(hdr_ambient_.Get(), kSceneFormat, 5);
    srv(ao_.Get(), DXGI_FORMAT_R8_UNORM, 6);
    srv(scene_.Get(), kSceneFormat, 7);
    srv(history_[0].Get(), kSceneFormat, 8);
    srv(history_[1].Get(), kSceneFormat, 9);
    uavd(ao_.Get(), DXGI_FORMAT_R8_UNORM, 12);
    uavd(scene_.Get(), kSceneFormat, 13);
    uavd(history_[0].Get(), kSceneFormat, 14);
    uavd(history_[1].Get(), kSceneFormat, 15);
    hist_valid_ = false;
    return true;
}

bool Renderer::create_post_pipelines(std::string& error) {
    if (!post_root_) {
        D3D12_DESCRIPTOR_RANGE sr = {};
        sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr.NumDescriptors = 9;
        // t9, the mist's integrated volume, sits at heap [32]: 29 past the
        // table's start at [3].
        D3D12_DESCRIPTOR_RANGE sr2[3] = {sr, {}, {}};
        sr2[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr2[1].NumDescriptors = 1;
        sr2[1].BaseShaderRegister = 9;
        sr2[1].OffsetInDescriptorsFromTableStart = 29;
        // t10, L5's traced visibility, at heap [36].
        sr2[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr2[2].NumDescriptors = 1;
        sr2[2].BaseShaderRegister = 10;
        sr2[2].OffsetInDescriptorsFromTableStart = 33;
        D3D12_DESCRIPTOR_RANGE ur = {};
        ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ur.NumDescriptors = 4;
        D3D12_ROOT_PARAMETER p[5] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 3;
        p[1].DescriptorTable.pDescriptorRanges = sr2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ur;
        // The exposure meter (ADR-004 L1): the luminance histogram (u4) and
        // the exposure state (u5), which ps_final reads too.
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[3].Descriptor.ShaderRegister = 4;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[4].Descriptor.ShaderRegister = 5;
        for (int k = 0; k < 5; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[3] = {};
        ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        for (int k = 0; k < 3; ++k) {
            ss[k].AddressU = ss[k].AddressV = ss[k].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[k].MaxLOD = D3D12_FLOAT32_MAX;
            ss[k].ShaderRegister = k;
            ss[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // The sky-view table is world azimuth in u: it wraps.
        ss[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 3;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) { error = "post root signature"; return false; }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&post_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (post)", hr); return false; }
        for (UINT f = 0; f < kFrameCount; ++f) {
            D3D12_HEAP_PROPERTIES up = {};
            up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 512;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&post_cb_[f]));
            if (FAILED(hr)) { error = hr_text("post cb", hr); return false; }
            post_cb_[f]->Map(0, nullptr, &post_cb_cpu_[f]);
        }
        // The meter's buffers. Root UAVs, so ALLOW_UNORDERED_ACCESS is not
        // optional (CLAUDE.md: the M1 hang). A committed default-heap buffer
        // starts zeroed, which the exposure kernel reads as "no history yet".
        for (int k = 0; k < 2; ++k) {
            D3D12_HEAP_PROPERTIES dh = {};
            dh.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = k == 0 ? 256 * 4 : 2 * 16;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            hr = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(k == 0 ? &lumhist_ : &expo_));
            if (FAILED(hr)) { error = hr_text("exposure meter buffer", hr); return false; }
        }
    }
    const char* kPost = "engine/render/shaders/post.hlsl";
    const char* cs_e[6] = {"cs_ao", "cs_composite", "cs_taa", "cs_hist_clear", "cs_histogram", "cs_exposure"};
    ComPtr<ID3D12PipelineState>* cs_o[6] = {&ao_pso_, &composite_pso_, &taa_pso_,
                                            &hist_clear_pso_, &hist_pso_, &expo_pso_};
    for (int k = 0; k < 6; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kPost, cs_e[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = post_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(cs_o[k]->GetAddressOf())))) {
            error = "post compute pso";
            return false;
        }
    }
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kPost, "vs_full", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kPost, "ps_final", "ps_5_1", ps, error)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = post_root_.Get();
    pd.VS.pShaderBytecode = vs->GetBufferPointer();
    pd.VS.BytecodeLength = vs->GetBufferSize();
    pd.PS.pShaderBytecode = ps->GetBufferPointer();
    pd.PS.BytecodeLength = ps->GetBufferSize();
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&final_pso_)))) {
        error = "post final pso";
        return false;
    }
    return true;
}

void Renderer::record_post(D3D12_CPU_DESCRIPTOR_HANDLE backbuffer) {
    const bool wire = mode_ == Mode::Wireframe;
    // Last time this frame index ran, its stamps were resolved; its fence
    // has been waited for, so they can be read now.
    const UINT q0 = UINT(frame_) * 5;
    if (post_queries_ && post_query_pending_[frame_] && post_query_freq_) {
        UINT64* t = nullptr;
        D3D12_RANGE rr = {q0 * 8, (q0 + 5) * 8};
        if (SUCCEEDED(post_query_rb_->Map(0, &rr, reinterpret_cast<void**>(&t)))) {
            const UINT64* s = t + q0;
            const double k = 1000.0 / double(post_query_freq_);
            auto ema = [](float& v, double x) { v = v == 0.0f ? float(x) : float(0.95 * v + 0.05 * x); };
            ema(post_times_.atmosphere, double(s[1] - s[0]) * k);
            ema(post_times_.mist_inject, double(s[2] - s[1]) * k);
            ema(post_times_.mist_integrate, double(s[3] - s[2]) * k);
            ema(post_times_.post_total, double(s[4] - s[0]) * k);
            D3D12_RANGE nr = {0, 0};
            post_query_rb_->Unmap(0, &nr);
        }
        post_query_pending_[frame_] = false;
    }
    if (post_queries_) cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 0);
    // TAA only reprojects static surfaces by depth; a camera that jumped
    // (Tab, a teleport, the first frame) starts the history over.
    const float jx = cur_cam_[0] - prev_cam_[0], jy = cur_cam_[1] - prev_cam_[1], jz = cur_cam_[2] - prev_cam_[2];
    if (jx * jx + jy * jy + jz * jz > 400.0f) hist_valid_ = false;

    PostConstants pc = {};
    invert4(view_proj_, pc.inv_view_proj);
    std::memcpy(pc.view_proj, view_proj_, sizeof pc.view_proj);
    std::memcpy(pc.prev_view_proj, prev_vp_, sizeof pc.prev_view_proj);
    for (int k = 0; k < 3; ++k) pc.cam_pos[k] = cur_cam_[k];
    for (int k = 0; k < 3; ++k) {
        pc.sun_dir[k] = sky_frame_.key_dir[k];          // contact shadows march towards the key light
        pc.sky_sun_dir[k] = sky_frame_.sun_dir[k];
        pc.sky_moon_dir[k] = sky_frame_.moon_dir[k];
        pc.sun_disk[k] = sky_frame_.sun_disk[k];
        pc.moon_disk[k] = sky_frame_.moon_disk[k];
    }
    pc.cam_alt_km = sky_frame_.cam_alt_km;
    for (int k = 0; k < 3; ++k) pc.cam_fwd[k] = cur_fwd_[k];
    pc.fog_on = (fog_on_ && !wire) ? 1.0f : 0.0f;
    pc.fog_near = kFogNear;
    pc.fog_log_ratio = std::log(kFogFar / kFogNear);
    pc.rt_shadows = (rt_shadows_ && rt_ok_ && !wire) ? 1.0f : 0.0f;
    pc.near_plane = kNearPlane;
    pc.far_plane = kFarPlane;
    pc.sharpen = taa_on_ && !wire ? 0.5f : 0.0f;
    pc.inv_size[0] = 1.0f / float(width_);
    pc.inv_size[1] = 1.0f / float(height_);
    pc.jitter_ndc[0] = jitter_[0];
    pc.jitter_ndc[1] = jitter_[1];
    pc.width = width_;
    pc.height = height_;
    pc.hist_cur = hist_cur_;
    pc.flags = (taa_on_ && !wire ? 1u : 0u) | (ao_on_ && !wire ? 2u : 0u) |
               (contact_on_ && !wire ? 4u : 0u) | (hist_valid_ ? 8u : 0u);
    pc.frame = static_cast<UINT>(post_frame_);
    pc.ao_radius = 1.2f;
    pc.contact_len = 0.5f;
    pc.debug = post_debug_;
    // Exposure (ADR-004 L1). The time since the last post pass drives the
    // eye's adaptation; a still adapts instantly instead, so the picture
    // does not depend on how many warm-up frames it took.
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const unsigned long long t = static_cast<unsigned long long>(now.QuadPart);
        const double dt = last_post_tick_ ? double(t - last_post_tick_) / double(freq.QuadPart) : 0.0;
        last_post_tick_ = t;
        pc.dt = static_cast<float>(dt < 0.0 ? 0.0 : (dt > 0.25 ? 0.25 : dt));
    }
    pc.ev_comp = ev_comp_;
    pc.ev100_fixed = ev100_fixed_;
    pc.tonemap = wire ? 0u : tonemap_;
    pc.meter_lo = 0.10f;
    pc.meter_hi = 0.90f;
    pc.expo_flags = exposure_snap_ ? 1u : 0u;
    pc.agx_saturation = agx_saturation_;
    pc.agx_power = agx_power_;
    std::memcpy(post_cb_cpu_[frame_], &pc, sizeof pc);

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_->ResourceBarrier(1, &rb);
    };
    auto uavb = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = r;
        cmd_->ResourceBarrier(1, &rb);
    };
    // The sky-view table and the aerial-perspective volume for this frame.
    record_atmosphere();
    // The transitions first: a timestamp with no barrier in front of it is
    // written while the dispatches before it still run, and the
    // atmosphere's time showed up as 0 with the mist's taking it.
    tr(sky_view_tex_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tr(ap_tex_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (post_queries_) cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1);
    // The mist's froxels (L4), injected and integrated -- or nothing at all.
    if (fog_on_ && !wire) {
        record_fog();
    } else if (post_queries_) {
        cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 2);
        cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 3);
    }
    tr(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
    tr(hdr_direct_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    tr(hdr_ambient_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, read);

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE srvs = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    srvs.ptr += UINT64(3) * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE uavs = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    uavs.ptr += UINT64(12) * inc;
    cmd_->SetComputeRootSignature(post_root_.Get());
    cmd_->SetComputeRootConstantBufferView(0, post_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetComputeRootDescriptorTable(1, srvs);
    cmd_->SetComputeRootDescriptorTable(2, uavs);
    cmd_->SetComputeRootUnorderedAccessView(3, lumhist_->GetGPUVirtualAddress());
    cmd_->SetComputeRootUnorderedAccessView(4, expo_->GetGPUVirtualAddress());

    // The traced key-light shadow and the RT instruments (L5).
    if (rt_ok_ && rt_active_ && !(rt_.debug_ & 1u)) {
        record_rt_trace();
        cmd_->SetComputeRootSignature(post_root_.Get());
        cmd_->SetComputeRootConstantBufferView(0, post_cb_[frame_]->GetGPUVirtualAddress());
        cmd_->SetComputeRootDescriptorTable(1, srvs);
        cmd_->SetComputeRootDescriptorTable(2, uavs);
        cmd_->SetComputeRootUnorderedAccessView(3, lumhist_->GetGPUVirtualAddress());
        cmd_->SetComputeRootUnorderedAccessView(4, expo_->GetGPUVirtualAddress());
    }
    // AO at half resolution.
    cmd_->SetPipelineState(ao_pso_.Get());
    cmd_->Dispatch(((width_ + 1) / 2 + 7) / 8, ((height_ + 1) / 2 + 7) / 8, 1);
    tr(ao_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    // Direct x contact + ambient x AO.
    cmd_->SetPipelineState(composite_pso_.Get());
    cmd_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    tr(scene_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    // TAA into this frame's history.
    ID3D12Resource* cur = history_[hist_cur_].Get();
    tr(cur, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd_->SetPipelineState(taa_pso_.Get());
    cmd_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    uavb(cur);
    tr(cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);

    // Meter what TAA produced, then choose this frame's exposure from it.
    cmd_->SetPipelineState(hist_clear_pso_.Get());
    cmd_->Dispatch(1, 1, 1);
    uavb(lumhist_.Get());
    cmd_->SetPipelineState(hist_pso_.Get());
    cmd_->Dispatch(((width_ + 1) / 2 + 15) / 16, ((height_ + 1) / 2 + 15) / 16, 1);
    uavb(lumhist_.Get());
    cmd_->SetPipelineState(expo_pso_.Get());
    cmd_->Dispatch(1, 1, 1);
    uavb(expo_.Get());

    // Sharpen and tonemap into the back buffer.
    D3D12_VIEWPORT vp = {};
    vp.Width = float(width_);
    vp.Height = float(height_);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc = {0, 0, LONG(width_), LONG(height_)};
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &sc);
    cmd_->OMSetRenderTargets(1, &backbuffer, FALSE, nullptr);
    cmd_->SetGraphicsRootSignature(post_root_.Get());
    cmd_->SetGraphicsRootConstantBufferView(0, post_cb_[frame_]->GetGPUVirtualAddress());
    cmd_->SetGraphicsRootDescriptorTable(1, srvs);
    cmd_->SetGraphicsRootDescriptorTable(2, uavs);
    cmd_->SetGraphicsRootUnorderedAccessView(3, lumhist_->GetGPUVirtualAddress());
    cmd_->SetGraphicsRootUnorderedAccessView(4, expo_->GetGPUVirtualAddress());
    cmd_->SetPipelineState(final_pso_.Get());
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_->DrawInstanced(3, 1, 0, 0);

    // Back to where the next frame starts.
    if (rt_ok_ && rt_active_ && !(rt_.debug_ & 1u)) tr(rt_vis_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(sky_view_tex_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(ap_tex_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(ao_.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(scene_.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(depth_.Get(), read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    tr(hdr_direct_.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    tr(hdr_ambient_.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET);

    hist_valid_ = taa_on_ && !wire;
    hist_cur_ ^= 1u;
    std::memcpy(prev_vp_, view_proj_nj_, sizeof prev_vp_);
    for (int k = 0; k < 3; ++k) prev_cam_[k] = cur_cam_[k];
    if (post_queries_) {
        cmd_->EndQuery(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 4);
        cmd_->ResolveQueryData(post_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0, 5, post_query_rb_.Get(), UINT64(q0) * 8);
        post_query_pending_[frame_] = true;
    }
    ++post_frame_;
}

void Renderer::record_scene(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    // This frame's sun, moon and sky (ADR-004 L2), before anything uses them.
    update_sky(view.cam_pos[1]);
    for (int k = 0; k < 3; ++k) tree_settings_.sun_dir[k] = sky_frame_.key_dir[k];
    // The ray-tracing scene (ADR-004 L5): due BLAS and this frame's TLAS.
    if (rt_ok_ && rt_active_) {
        if (rt_seed_) {
            // Switched on with the world already meshed: everything drawn at
            // LOD 0-3 is due, as if meshed two frames ago.
            std::vector<std::pair<ChunkKey, UINT> > drawn;
            streamer_.drawn(drawn);
            std::vector<Streamer::MeshedChunk> seed;
            for (size_t i = 0; i < drawn.size(); ++i) {
                Streamer::MeshedChunk m;
                m.key = drawn[i].first;
                m.slot = drawn[i].second;
                seed.push_back(m);
            }
            rt_.note_meshed(seed, true);
            rt_seed_ = false;
        }
        rt_.note_meshed(streamer_.last_meshed());
        const UINT q = UINT(frame_) * 4;
        if (rt_queries_) cmd_->EndQuery(rt_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 0);
        if (!(rt_.debug_ & 8u)) rt_.record(cmd_.Get(), UINT(frame_), streamer_, mesher_, trees_.models(), trees_.looks(),
                   trees_.instance_list(), view.cam_pos, tree_settings_.max_dist);
        if (rt_queries_) cmd_->EndQuery(rt_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
    }
    // Local lights (ADR-004 L3): their list, their clusters and their shadow
    // faces, before anything is shaded with them.
    record_lights(view);
    // The sun's view first: the shadow map has to be finished before anything
    // that samples it is drawn.
    record_shadows(view);

    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &scissor);

    // The scene goes into the two HDR targets; the back buffer only receives
    // the post chain's result (record_post).
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE hdr[2];
    hdr[0] = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    hdr[1] = hdr[0];
    hdr[1].ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cmd_->OMSetRenderTargets(2, hdr, FALSE, &dsv);
    cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    MeshConstants mc = {};
    build_view_proj(view, mc.view_proj);
    // TAA's sub-pixel jitter: Halton (2, 3) over 8 frames, as an NDC offset
    // added to clip x and y in proportion to w. The unjittered matrix is what
    // the next frame reprojects with.
    std::memcpy(view_proj_nj_, mc.view_proj, sizeof view_proj_nj_);
    jitter_[0] = jitter_[1] = 0.0f;
    if (taa_on_ && mode_ != Mode::Wireframe) {
        const int ji = int(post_frame_ % 8) + 1;
        jitter_[0] = (halton(ji, 2) - 0.5f) * 2.0f / float(width_);
        jitter_[1] = (halton(ji, 3) - 0.5f) * 2.0f / float(height_);
        for (int r = 0; r < 4; ++r) {
            mc.view_proj[r * 4 + 0] += jitter_[0] * mc.view_proj[r * 4 + 3];
            mc.view_proj[r * 4 + 1] += jitter_[1] * mc.view_proj[r * 4 + 3];
        }
    }
    std::memcpy(view_proj_, mc.view_proj, sizeof view_proj_);
    for (int k = 0; k < 3; ++k) {
        cur_cam_[k] = view.cam_pos[k];
        cur_fwd_[k] = view.cam_fwd[k];
        cur_right_[k] = view.cam_right[k];
        cur_up_[k] = view.cam_up[k];
    }
    cur_tan_ = view.tan_half_fov;
    cur_aspect_ = view.aspect;
    cur_time_ = view.time;
    mc.cam_pos[0] = view.cam_pos[0];
    mc.cam_pos[1] = view.cam_pos[1];
    mc.cam_pos[2] = view.cam_pos[2];
    mc.far_plane = kFarPlane;

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = mesher_.indices()->GetGPUVirtualAddress();
    ibv.SizeInBytes = kSlots * kSlotIndices * 4;
    ibv.Format = DXGI_FORMAT_R32_UINT;

    {
        // The mesh does not cover the screen, so the sky is cleared rather
        // than drawn -- since L2 by the post chain, from the atmosphere. The
        // ambient target is cleared to a MARKER (lighting_units.h) that
        // capture()'s crack test compares against exactly and the composite
        // reads the sea's coverage from.
        const float sky[4] = {kSkyClearR, kSkyClearG, kSkyClearB, 1.0f};
        const float none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd_->ClearRenderTargetView(hdr[0], none, 0, nullptr);
        cmd_->ClearRenderTargetView(hdr[1], sky, 0, nullptr);
    }

    {
        const bool wire = (mode_ == Mode::Wireframe);
        mc.wire = wire ? 1.0f : 0.0f;
        mc.slot_vertices = static_cast<float>(kSlotVertices);
        mc.mesh_debug = mesh_debug_;
        mc.texture_mode = texture_mode_;
        streamer_.fill_visibility(static_cast<float*>(fade_cpu_[frame_]));
        cmd_->SetPipelineState(wire ? mesh_wire_pso_.Get() : mesh_pso_.Get());
        cmd_->SetGraphicsRootSignature(mesh_root_.Get());
        cmd_->SetGraphicsRoot32BitConstants(0, sizeof(MeshConstants) / 4, &mc, 0);
        cmd_->SetGraphicsRootShaderResourceView(
            1, mesher_.vertices()->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(2, fade_buf_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootShaderResourceView(5, mesher_.vertex_materials()->GetGPUVirtualAddress());
        {
            ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
            cmd_->SetDescriptorHeaps(1, heaps);
            cmd_->SetGraphicsRootConstantBufferView(3, shadow_cb_[frame_]->GetGPUVirtualAddress());
            cmd_->SetGraphicsRootDescriptorTable(4, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        }
        cmd_->IASetIndexBuffer(&ibv);
        // One command per slot. A retired or never-used slot has all-zero
        // arguments and the command processor skips it, which is cheaper than
        // compacting the array every time a chunk moves.
        cmd_->ExecuteIndirect(cmd_sig_.Get(), kSlots, mesher_.draw_args(), 0, nullptr, 0);
    }

    // Trees after the ground, before the sea. Not in wireframe: the wire view
    // is for reading the terrain mesh, and a forest over it hides the mesh.
    if (trees_on_ && trees_.ready() && mode_ != Mode::Wireframe) {
        TreeFrameInputs ti = {};
        std::memcpy(ti.view_proj, mc.view_proj, sizeof ti.view_proj);
        for (int k = 0; k < 3; ++k) {
            ti.cam_pos[k] = view.cam_pos[k];
            ti.cam_fwd[k] = view.cam_fwd[k];
            ti.cam_right[k] = view.cam_right[k];
            ti.cam_up[k] = view.cam_up[k];
        }
        ti.time = view.time;
        ti.far_plane = kFarPlane;
        ti.shadow_cb = shadow_cb_[frame_]->GetGPUVirtualAddress();
        trees_.record(cmd_.Get(), frame_, ti, tree_settings_);
    }

    // Ground cover after the trees, before the sea.
    if (foliage_on_ && foliage_.ready() && mode_ != Mode::Wireframe) {
        FoliageFrameInputs fi = {};
        std::memcpy(fi.view_proj, mc.view_proj, sizeof fi.view_proj);
        for (int k = 0; k < 3; ++k) {
            fi.cam_pos[k] = view.cam_pos[k];
            fi.cam_right[k] = view.cam_right[k];
            fi.cam_up[k] = view.cam_up[k];
        }
        fi.time = view.time;
        fi.far_plane = kFarPlane;
        fi.shadow_cb = shadow_cb_[frame_]->GetGPUVirtualAddress();
        fi.shadow_table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        cmd_->SetDescriptorHeaps(1, heaps);
        foliage_.record(cmd_.Get(), frame_, fi, foliage_settings_);
    }

    // The sea goes last: it is transparent, so it has to blend over whatever
    // is behind it, and the depth buffer the terrain just wrote is what tells
    // it where the land is in front.
    if (water_ && water_pso_ && mode_ != Mode::Wireframe) {
        WaterConstants wc = {};
        std::memcpy(wc.view_proj, mc.view_proj, sizeof wc.view_proj);
        wc.cam_pos[0] = view.cam_pos[0];
        wc.cam_pos[1] = view.cam_pos[1];
        wc.cam_pos[2] = view.cam_pos[2];
        // The caller's clock, the same one the raymarch used. A renderer
        // keeping its own would drift from the simulation the moment a frame
        // was dropped or a headless mode skipped the loop.
        wc.time = view.time;
        // The same sun the terrain is lit by, or the two surfaces disagree
        // about where the light is and the sea looks pasted on.
        for (int k = 0; k < 3; ++k) wc.sun_dir[k] = sky_frame_.key_dir[k];
        fill_light(wc.light);
        wc.far_plane = kFarPlane;
        // The camera basis, because the grid is built on the SCREEN and needs
        // to know which way each pixel looks.
        for (int k = 0; k < 3; ++k) {
            wc.cam_fwd[k] = view.cam_fwd[k];
            wc.cam_right[k] = view.cam_right[k];
            wc.cam_up[k] = view.cam_up[k];
        }
        wc.aspect = view.aspect;
        wc.tan_half_fov = view.tan_half_fov;
        wc.sea_level = world_.params().sea_level;
        wc.sea_state = sea_state_;
        wc.grid_cells = static_cast<float>(kWaterCells);
        wc.extent = kFarPlane;
        wc.debug_mode = water_debug_;
        // The same question physics asks of the same function, so the
        // shader's idea of "is the eye above the water here" and the
        // swimmer's are one answer rather than two that usually agree.
        wc.cam_water = wc.sea_level + ww_height_at(view.cam_pos[0], view.cam_pos[2],
                                                   view.time, sea_state_);

        cmd_->SetPipelineState(water_pso_.Get());
        cmd_->SetGraphicsRootSignature(water_root_.Get());
        std::memcpy(water_cb_cpu_[frame_], &wc, sizeof wc);
        cmd_->SetGraphicsRootConstantBufferView(
            0, water_cb_[frame_]->GetGPUVirtualAddress());
        cmd_->SetGraphicsRootConstantBufferView(1, world_.world_cb()->GetGPUVirtualAddress());
        ID3D12Resource* wb[10] = {
            world_.cells(), world_.cell_offsets(), world_.cell_items(),
            world_.archetypes(), world_.rivers(), world_.river_offsets(),
            world_.river_items(), world_.entrances(), world_.entrance_offsets(),
            world_.entrance_items()};
        for (UINT i = 0; i < 10; ++i) {
            cmd_->SetGraphicsRootUnorderedAccessView(2 + i, wb[i]->GetGPUVirtualAddress());
        }
        cmd_->IASetIndexBuffer(&water_ibv_);
        cmd_->DrawIndexedInstanced(water_index_count_, 1, 0, 0, 0);
    }

    record_post(rtv);
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
        r.slot[r.count] = m.slot;
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

    // A chunk that meshed to nothing gives its slot back. The counter the
    // decision rests on was copied before this frame's work, so it is the
    // count for THIS chunk and not a successor's: the slot cannot have been
    // reused yet, because it is still recorded as this chunk's.
    for (UINT p = 0; p < r.count; ++p) {
        UINT v = 0, ix = 0;
        mesher_.slot_counts(r.slot[p], v, ix);
        // No retire needed: a slot with no vertices already has all-zero
        // indirect arguments, and the command processor skips those.
        if (v == 0 && ix == 0) streamer_.release_empty(r.keys[p]);
    }

    // Every reported height should sit on the isosurface. Checked against the
    // C++ side of the dialect, the same tolerance comparison B2 used on the
    // mesh -- and the only thing that would catch a ring read one frame early,
    // which returns a previous chunk's perfectly plausible terrain.
    //
    // At most kMeshBudgetPerFrame patches a frame. It is 81 full CPU field
    // evaluations per patch, and once the budget adapted upward it was
    // checking three times as many patches: the 99th-percentile frame went
    // from 20 to 27 ms with the GPU's meshing time unchanged. A ring read early
    // is wrong for EVERY patch in it, so a sample catches it as surely as all.
    double sum = 0.0;
    unsigned long long n = 0;
    const UINT checked = r.count < kMeshBudgetPerFrame ? r.count : kMeshBudgetPerFrame;
    for (UINT p = 0; p < checked; ++p) {
        const float* g = h + size_t(p) * kHeightSamples;
        const float v = r.voxel[p];
        for (UINT k = 0; k < kHeightGrid; k += 4) {
            for (UINT i = 0; i < kHeightGrid; i += 4) {
                const float y = g[k * kHeightGrid + i];
                if (y > 1.0e29f) continue;          // no surface in this column
                const float x = r.origin[p][0] + float(i) * v;
                const float z = r.origin[p][2] + float(k) * v;
                const float d = world_.density(x, y, z, v);
                const float a = d < 0.0f ? -d : d;
                if (a > stats_.height_residual_max) {
                    stats_.height_residual_max = a;
                    stats_.height_residual_voxel = v;
                }
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
        // A collision mesh only for chunks physics keeps. The readback is
        // rationed (kMeshChunksPerFrame, 163 KB a chunk), and until a chunk's
        // mesh lands its heightfield is SOLID up to its top surface -- the void
        // under an overhang filled in. Coarse chunks nobody can stand on were
        // queueing ahead of the fine ones: --overhang's backlog peaked at 271.
        if (e.multi_surface_columns) {
            ++stats_.chunks_needing_mesh;
            if (r.keys[p].lod <= kCollisionMaxLod) mesh_wanted_.push_back(r.keys[p]);
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

        // Surface vertices are true now -- nothing to undo. The skirt
        // curtain is left out of the collision mesh entirely: any triangle
        // touching a skirt vertex is dropped. It sits two voxels inside the
        // solid, so a character could only ever meet it through a crack it
        // exists to cover, and a shape is cheaper without it.
        //
        // The lip this replaced was BAKED into surface vertices and had to be
        // undone here by guessing from positions which ones it had moved;
        // getting that wrong put a character two voxels low wherever a chunk
        // edge fell underfoot, and cost a day as a fake persistence failure.
        // Compacted: skirt vertices are not in the patch at all, so nothing
        // downstream -- physics, or the residual check in main.cpp, which
        // sees positions only -- needs a predicate to leave them out.
        std::vector<unsigned> remap(nv, 0xFFFFFFFFu);
        e.verts.reserve(size_t(nv) * 3);
        for (UINT i = 0; i < nv; ++i) {
            if (is_skirt_vertex(mv[i].nrm)) continue;
            remap[i] = unsigned(e.verts.size() / 3);
            e.verts.push_back(mv[i].pos[0]);
            e.verts.push_back(mv[i].pos[1]);
            e.verts.push_back(mv[i].pos[2]);
        }
        // Pool indices are absolute so the draw needs no rebasing; a shape
        // built from this chunk's vertices alone does.
        const unsigned vbase = slot * kSlotVertices;
        e.idx.reserve(ni);
        for (UINT t = 0; t + 2 < ni; t += 3) {
            unsigned tri[3];
            bool keep = true;
            for (int c = 0; c < 3; ++c) {
                const unsigned v = mi[t + c] - vbase;
                if (v >= nv || remap[v] == 0xFFFFFFFFu) { keep = false; break; }
                tri[c] = remap[v];
            }
            if (!keep) continue;
            e.idx.push_back(tri[0]); e.idx.push_back(tri[1]); e.idx.push_back(tri[2]);
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
    // Page, upload, then mesh. The order is the whole point: a dispatch that
    // runs before its bricks are in the pool meshes against whatever was
    // there, which looks correct for any brick that happened to be resident
    // already and so fails only for new edits.
    mesher_.begin_frame_bricks();
    edits_.take_dirty(dirty_bricks_);
    for (size_t i = 0; i < dirty_bricks_.size(); ++i) pager_.invalidate(dirty_bricks_[i]);

    brick_uploads_.clear();
    Streamer::EditContext ctx;
    ctx.store = &edits_;
    ctx.pager = &pager_;
    ctx.frame = frame_index_;
    ctx.uploads = &brick_uploads_;
    stats_.mesh_budget = mesh_budget_;
    stats_.meshed_this_frame = streamer_.plan_work(mesher_, mesh_budget_, ctx);

    for (size_t i = 0; i < brick_uploads_.size(); ++i) {
        const Brick* b = edits_.find(brick_uploads_[i]);
        if (!b) continue;
        std::vector<BrickKey> one(1, brick_uploads_[i]);
        std::vector<uint32_t> slots;
        std::vector<BrickKey> ignored;
        pager_.acquire(one, frame_index_, slots, ignored);
        if (slots.empty()) continue;
        if (mesher_.record_brick_upload(cmd_.Get(), slots[0], b->density, b->edited)) {
            pager_.mark_uploaded(brick_uploads_[i]);
        } else {
            // No staging room this frame. The brick stays un-uploaded, and the
            // chunks over it are thrown away again so they are re-meshed once
            // it lands -- otherwise they keep the geometry they were meshed
            // with, which is the terrain WITHOUT the edit, for good.
            //
            // Marking it uploaded anyway is what the first version did, and
            // three holes dug in one frame needed exactly one brick more than
            // the per-frame limit: the holes never appeared, and only showed
            // up after a restart, when loading spread the uploads out.
            const float s = float(kBrickSide) * kEditVoxel;
            const float lo[3] = {float(brick_uploads_[i].x) * s,
                                 float(brick_uploads_[i].y) * s,
                                 float(brick_uploads_[i].z) * s};
            const float hi[3] = {lo[0] + s, lo[1] + s, lo[2] + s};
            streamer_.invalidate_box(lo, hi);
            ++stats_.brick_upload_deferrals;
        }
    }
    stats_.bricks_uploaded = mesher_.bricks_uploaded_this_frame();

    streamer_.record_planned(cmd_.Get(), mesher_);
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
    update_trees(view, false);
    // Only when it will draw this frame: a tile handed a slot is baked by the
    // same frame's record, and a frame that skipped it would leave the slot
    // marked valid over cells nobody wrote.
    if (foliage_on_ && mode_ != Mode::Wireframe)
        foliage_.update(view.cam_pos[0], view.cam_pos[2], foliage_settings_, false);
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
    stats_.edit_bricks = edits_.brick_count();
    stats_.edit_bytes = edits_.bytes();
    stats_.bricks_resident = pager_.resident();
    stats_.brick_evictions = pager_.evictions();
    stats_.brick_misses = pager_.misses();

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
    mesher_.read_counts(stats_.vertices, stats_.indices, stats_.overflowed,
                        stats_.peak_slot_vertices, stats_.peak_slot_indices);
    stats_.mesh_ms = mesher_.last_mesh_ms();
    // Grow the budget while meshing is cheap and there is work waiting;
    // shrink it the moment the GPU time goes over. The timestamp is a frame
    // or two old, which is why the steps are small.
    if (adaptive_budget_) {
        if (stats_.mesh_ms > kMeshTargetMs) {
            mesh_budget_ = mesh_budget_ > kMeshBudgetPerFrame + 2 ? mesh_budget_ - 2
                                                                  : kMeshBudgetPerFrame;
        } else if (streamer_.pending_count() > mesh_budget_ &&
                   stats_.mesh_ms < kMeshTargetMs * 0.8) {
            mesh_budget_ = mesh_budget_ + 2 < kMeshBudgetMax ? mesh_budget_ + 2 : kMeshBudgetMax;
        }
    }
    stats_.resident = streamer_.resident_count();
    stats_.pending = streamer_.pending_count();
    stats_.free_slots = streamer_.free_slot_count();
    stats_.empty_resident = streamer_.empty_resident();
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
        mesher_.begin_frame_bricks();
        {
            brick_uploads_.clear();
            Streamer::EditContext cctx;
            cctx.store = &edits_;
            cctx.pager = &pager_;
            cctx.frame = frame_index_;
            cctx.uploads = &brick_uploads_;
            streamer_.plan_work(mesher_, 64, cctx);
            for (size_t i = 0; i < brick_uploads_.size(); ++i) {
                const Brick* b = edits_.find(brick_uploads_[i]);
                if (!b) continue;
                std::vector<BrickKey> one(1, brick_uploads_[i]);
                std::vector<uint32_t> slots;
                std::vector<BrickKey> ignored;
                pager_.acquire(one, frame_index_, slots, ignored);
                    if (slots.empty()) continue;
                if (mesher_.record_brick_upload(cmd_.Get(), slots[0], b->density,
                                                b->edited)) {
                    pager_.mark_uploaded(brick_uploads_[i]);
                }
            }
            streamer_.record_planned(cmd_.Get(), mesher_);
        }
        mesher_.record_counter_copy(cmd_.Get());
        mesher_.record_vertex_copy(cmd_.Get());
        mesher_.record_to_draw(cmd_.Get());
        cmd_->Close();
        ID3D12CommandList* ls[] = {cmd_.Get()};
        queue_->ExecuteCommandLists(1, ls);
        wait_for_gpu();

        // Give back the slots of chunks that meshed to nothing. The frame
        // loop does this off the readback ring two frames later; this loop
        // has its own pacing and never touched the ring, so it never
        // reclaimed anything -- the pool filled with air and the fill stalled
        // with hundreds of chunks still queued.
        //
        // It did not show from the ground, where the nearest chunks are
        // ground. From 1400 m up, nearest-first spends the entire pool on the
        // air around the camera and the capture came back as an empty sky
        // over 483,590 triangles that were never reached.
        //
        // No latency to wait out here: wait_for_gpu() above means the
        // counters just copied are this pass's own.
        const std::vector<Streamer::MeshedChunk>& done = streamer_.last_meshed();
        for (size_t i = 0; i < done.size(); ++i) {
            UINT v = 0, ix = 0;
            mesher_.slot_counts(done[i].slot, v, ix);
            if (v == 0 && ix == 0) streamer_.release_empty(done[i].key);
        }
    }
    mesher_.read_counts(stats_.vertices, stats_.indices, stats_.overflowed,
                        stats_.peak_slot_vertices, stats_.peak_slot_indices);
    stats_.resident = streamer_.resident_count();
    stats_.pending = streamer_.pending_count();
    stats_.free_slots = streamer_.free_slot_count();
    stats_.empty_resident = streamer_.empty_resident();
    // A still of a finished world: no chunk mid-fade in a screenshot.
    streamer_.settle_fades();
    update_trees(view, true);
    if (foliage_on_ && mode_ != Mode::Wireframe)
        foliage_.update(view.cam_pos[0], view.cam_pos[2], foliage_settings_, true);

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

    // A still adapts its exposure instantly (ADR-004 L1), so the picture is
    // the scene's and not an artefact of how many frames it took.
    struct SnapGuard {
        bool& f;
        explicit SnapGuard(bool& flag) : f(flag) { f = true; }
        ~SnapGuard() { f = false; }
    } snap_guard(exposure_snap_);

    // TAA needs frames to converge: a still renders sixteen with the same
    // camera and the jitter walking, and captures the last. The ray-tracing
    // scene (L5) needs frames too, for its BLAS backlog: a still waits for
    // it, up to 400 frames, before counting down the sixteen.
    const bool taa_warm = taa_on_ && mode_ != Mode::Wireframe;
    auto rt_behind = [&]() {
        return rt_ok_ && rt_active_ && (rt_.stats().pending > 0 || rt_.stats().drawn_without_blas > 0);
    };
    if (taa_warm || rt_behind()) {
        hist_valid_ = false;
        int rt_frames = 0;
        for (int warm = 0; warm < 16 || (rt_behind() && rt_frames < 400); ++warm) {
            if (rt_behind() && rt_frames < 400) { ++rt_frames; warm = 0; }
            if (!taa_warm && !rt_behind()) break;
            allocators_[frame_]->Reset();
            cmd_->Reset(allocators_[frame_].Get(), nullptr);
            D3D12_RESOURCE_BARRIER wb = {};
            wb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            wb.Transition.pResource = targets_[frame_].Get();
            wb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            wb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            wb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd_->ResourceBarrier(1, &wb);
            D3D12_CPU_DESCRIPTOR_HANDLE wrtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            wrtv.ptr += static_cast<SIZE_T>(frame_) * rtv_stride_;
            record_scene(view, wrtv);
            wb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            wb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cmd_->ResourceBarrier(1, &wb);
            cmd_->Close();
            ID3D12CommandList* wl[] = {cmd_.Get()};
            queue_->ExecuteCommandLists(1, wl);
            wait_for_gpu();
        }
    }

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

    // The two HDR scene targets, for the "nothing drawn here" crack count.
    ComPtr<ID3D12Resource> hdr_rb[2];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT hfp = {};
    {
        const D3D12_RESOURCE_DESC hd = hdr_direct_->GetDesc();
        UINT64 htotal = 0;
        device_->GetCopyableFootprints(&hd, 0, 1, 0, &hfp, nullptr, nullptr, &htotal);
        ID3D12Resource* src_t[2] = {hdr_direct_.Get(), hdr_ambient_.Get()};
        for (int k = 0; k < 2; ++k) {
            D3D12_RESOURCE_DESC hb = bd;
            hb.Width = htotal;
            hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hb, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&hdr_rb[k]));
            if (FAILED(hr)) { error = hr_text("CreateCommittedResource (hdr readback)", hr); return false; }
            D3D12_RESOURCE_BARRIER t = {};
            t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            t.Transition.pResource = src_t[k];
            t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            t.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd_->ResourceBarrier(1, &t);
            D3D12_TEXTURE_COPY_LOCATION hd2 = {}, hs = {};
            hd2.pResource = hdr_rb[k].Get();
            hd2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            hd2.PlacedFootprint = hfp;
            hs.pResource = src_t[k];
            hs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd_->CopyTextureRegion(&hd2, 0, 0, 0, &hs, nullptr);
            t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            t.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd_->ResourceBarrier(1, &t);
        }
    }

    // Depth too, for the depth-based crack count.
    const D3D12_RESOURCE_DESC dd = depth_->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dfp = {};
    UINT64 dtotal = 0;
    device_->GetCopyableFootprints(&dd, 0, 1, 0, &dfp, nullptr, nullptr, &dtotal);
    ComPtr<ID3D12Resource> depth_rb;
    {
        D3D12_RESOURCE_DESC db = bd;
        db.Width = dtotal;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &db, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&depth_rb));
        if (FAILED(hr)) { error = hr_text("CreateCommittedResource (depth readback)", hr); return false; }
        D3D12_RESOURCE_BARRIER db2 = {};
        db2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        db2.Transition.pResource = depth_.Get();
        db2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        db2.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        db2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd_->ResourceBarrier(1, &db2);
        D3D12_TEXTURE_COPY_LOCATION ddst = {}, dsrc = {};
        ddst.pResource = depth_rb.Get();
        ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        ddst.PlacedFootprint = dfp;
        dsrc.pResource = depth_.Get();
        dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd_->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
        db2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        db2.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        cmd_->ResourceBarrier(1, &db2);
    }

    // The sky-view table, for the CPU-to-GPU agreement check.
    ComPtr<ID3D12Resource> sky_rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sfp = {};
    {
        const D3D12_RESOURCE_DESC sdsc = sky_view_tex_->GetDesc();
        UINT64 stotal = 0;
        device_->GetCopyableFootprints(&sdsc, 0, 1, 0, &sfp, nullptr, nullptr, &stotal);
        D3D12_RESOURCE_DESC sb = bd;
        sb.Width = stotal;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sb, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&sky_rb));
        if (FAILED(hr)) { error = hr_text("CreateCommittedResource (sky readback)", hr); return false; }
        D3D12_RESOURCE_BARRIER t = {};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = sky_view_tex_.Get();
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd_->ResourceBarrier(1, &t);
        D3D12_TEXTURE_COPY_LOCATION sd = {}, ss = {};
        sd.pResource = sky_rb.Get();
        sd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sd.PlacedFootprint = sfp;
        ss.pResource = sky_view_tex_.Get();
        ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd_->CopyTextureRegion(&sd, 0, 0, 0, &ss, nullptr);
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd_->ResourceBarrier(1, &t);
    }

    // The exposure meter: what the histogram saw and what it chose.
    ComPtr<ID3D12Resource> meter_rb;
    {
        D3D12_RESOURCE_DESC mb = bd;
        mb.Width = 256 * 4 + 2 * 16;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &mb, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&meter_rb));
        if (FAILED(hr)) { error = hr_text("CreateCommittedResource (meter readback)", hr); return false; }
        ID3D12Resource* src_m[2] = {lumhist_.Get(), expo_.Get()};
        const UINT64 off[2] = {0, 256 * 4};
        const UINT64 len[2] = {256 * 4, 2 * 16};
        for (int k = 0; k < 2; ++k) {
            D3D12_RESOURCE_BARRIER t = {};
            t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            t.Transition.pResource = src_m[k];
            t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            t.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd_->ResourceBarrier(1, &t);
            cmd_->CopyBufferRegion(meter_rb.Get(), off[k], src_m[k], 0, len[k]);
            t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            t.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cmd_->ResourceBarrier(1, &t);
        }
    }

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

    // The sky table against the CPU's own march of the same texels: a grid
    // of 16 x 12, at the GPU's 30 steps. Half floats read back, so anything
    // under 0.1% is the storage, not the model.
    sky_check_ = SkyCheck();
    {
        unsigned char* sp = nullptr;
        if (SUCCEEDED(sky_rb->Map(0, nullptr, reinterpret_cast<void**>(&sp)))) {
            auto h2f = [](unsigned short h) {
                const unsigned e = (h >> 10) & 31u, m = h & 1023u;
                if (e == 0) return float(m) / 16777216.0f;
                return std::ldexp(float(m | 1024u), int(e) - 25);
            };
            double sum = 0.0;
            for (int gy = 0; gy < 12; ++gy) {
                for (int gx = 0; gx < 16; ++gx) {
                    const int x = (gx * kAtmSkyW) / 16 + 5, y = (gy * kAtmSkyH) / 12 + 4;
                    const unsigned short* px = reinterpret_cast<const unsigned short*>(
                        sp + size_t(sfp.Offset) + size_t(y) * sfp.Footprint.RowPitch) + size_t(x) * 4;
                    const float g = 0.2126f * h2f(px[0]) + 0.7152f * h2f(px[1]) + 0.0722f * h2f(px[2]);
                    float dir[3], c[3];
                    Sky::sky_texel_dir(x, y, sky_frame_.cam_alt_km, dir);
                    sky_.radiance(sky_frame_, sky_frame_.cam_alt_km, dir, c, 30);
                    const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                    if (l <= 1e-6f) continue;
                    const float rel = std::fabs(g - l) / l;
                    sum += rel;
                    ++sky_check_.texels;
                    if (rel > sky_check_.worst_rel) {
                        sky_check_.worst_rel = rel;
                        sky_check_.worst_at[0] = float(x);
                        sky_check_.worst_at[1] = float(y);
                    }
                }
            }
            sky_check_.mean_rel = sky_check_.texels ? float(sum / sky_check_.texels) : 0.0f;
            sky_check_.valid = sky_check_.texels > 0;
            D3D12_RANGE nr = {0, 0};
            sky_rb->Unmap(0, &nr);
        }
    }

    exposure_report_ = ExposureReport();
    {
        void* mp = nullptr;
        if (SUCCEEDED(meter_rb->Map(0, nullptr, &mp))) {
            const unsigned* bins = static_cast<const unsigned*>(mp);
            const float* st = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mp) + 256 * 4);
            exposure_report_.bins.assign(bins, bins + 256);
            exposure_report_.adapted_log2 = st[0];
            exposure_report_.exposure = st[1];
            exposure_report_.ev100 = st[2];
            exposure_report_.target_log2 = st[3];
            exposure_report_.lo_nits = st[4];
            exposure_report_.hi_nits = st[5];
            exposure_report_.metered = st[6];
            exposure_report_.valid = st[7] > 0.5f;
            meter_rb->Unmap(0, &none);
        }
    }

    // Holes below the horizon, both ways. A pixel is below the horizon if
    // its view ray points more than ~1 degree under it: rays nearer the
    // horizon can pass over the rim of the world legitimately.
    {
        cracks_ = CrackCount();
        const float* dbase = nullptr;
        const unsigned char* hb0 = nullptr;
        const unsigned char* hb1 = nullptr;
        D3D12_RANGE drange = {0, static_cast<SIZE_T>(dtotal)};
        hdr_rb[0]->Map(0, nullptr, reinterpret_cast<void**>(const_cast<unsigned char**>(&hb0)));
        hdr_rb[1]->Map(0, nullptr, reinterpret_cast<void**>(const_cast<unsigned char**>(&hb1)));
        // Half floats: the clear values, as the targets hold them.
        auto half = [](unsigned short h) {
            const unsigned e = (h >> 10) & 31u, m = h & 1023u;
            const float s = (h & 0x8000u) ? -1.0f : 1.0f;
            if (e == 0) return s * float(m) / 16777216.0f;
            return s * std::ldexp(float(m | 1024u), int(e) - 25);
        };
        if (hb0 && hb1 &&
            SUCCEEDED(depth_rb->Map(0, &drange, reinterpret_cast<void**>(const_cast<float**>(&dbase))))) {
            for (UINT y = 0; y < height_; ++y) {
                const float ny = 1.0f - (float(y) + 0.5f) / float(height_) * 2.0f;
                const float* drow = reinterpret_cast<const float*>(
                    reinterpret_cast<const unsigned char*>(dbase) + size_t(y) * dfp.Footprint.RowPitch);
                const unsigned char* crow = &bgr[static_cast<size_t>(y) * width_ * 3];
                const unsigned short* drow_h = reinterpret_cast<const unsigned short*>(hb0 + size_t(y) * hfp.Footprint.RowPitch);
                const unsigned short* arow_h = reinterpret_cast<const unsigned short*>(hb1 + size_t(y) * hfp.Footprint.RowPitch);
                for (UINT x = 0; x < width_; ++x) {
                    const float nx = (float(x) + 0.5f) / float(width_) * 2.0f - 1.0f;
                    float dir[3];
                    for (int k = 0; k < 3; ++k) {
                        dir[k] = view.cam_fwd[k] + view.cam_right[k] * nx * view.tan_half_fov * view.aspect +
                                 view.cam_up[k] * ny * view.tan_half_fov;
                    }
                    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    if (dir[1] / dl > -0.0175f) continue;
                    ++cracks_.below_horizon;
                    const bool d = drow[x] >= 1.0f;
                    cracks_.by_depth += d;
                    // Nothing drawn: far depth, no direct light, ambient still the sky clear.
                    const unsigned short* dp = drow_h + x * 4;
                    const unsigned short* ap = arow_h + x * 4;
                    const bool clear = d && dp[0] == 0 && dp[1] == 0 && dp[2] == 0 &&
                                       half(ap[0]) == kSkyClearR &&
                                       half(ap[1]) == kSkyClearG &&
                                       half(ap[2]) == kSkyClearB;
                    cracks_.undrawn += clear;
                    if (clear && cracks_.where.size() < 200) cracks_.where.push_back(std::make_pair(int(x), int(y)));
                }
            }
            D3D12_RANGE dnone = {0, 0};
            depth_rb->Unmap(0, &dnone);
        }
        if (hb0) hdr_rb[0]->Unmap(0, nullptr);
        if (hb1) hdr_rb[1]->Unmap(0, nullptr);
    }

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
    ChunkKey key;
    const bool have_key = streamer_.key_of_slot(0, key);
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float voxel = 0.0f;
    if (have_key) {
        const float S = lod_chunk_size(key.lod);
        origin[0] = float(key.x) * S;
        origin[1] = float(key.y) * S;
        origin[2] = float(key.z) * S;
        voxel = lod_voxel(key.lod);
    }

    double sum = 0.0;
    for (size_t i = 0; i < verts.size(); ++i) {
        // The skirt is off the isosurface on purpose; measuring it here would
        // report the skirt depth and hide everything smaller.
        if (is_skirt_vertex(verts[i].nrm)) continue;
        const float d = world_.density(verts[i].pos[0], verts[i].pos[1],
                                       verts[i].pos[2]);
        const float a = d < 0.0f ? -d : d;
        if (a > max_err) max_err = a;
        sum += a;
        ++checked;
    }
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

void Renderer::band_report(const char* path) const {
    FILE* f = std::fopen(path, "w");
    if (!f) return;

    std::fprintf(f, "Residency cost per column, and whether the band actually\n"
                    "contains the surface. The band samples 5x5; this checks it\n"
                    "against 17x17, at every level.\n\n");
    std::fprintf(f, "%-5s %-9s %9s %9s %9s %9s\n",
                 "LOD", "column", "band", "relief", "chunks", "worst miss");

    double total_chunks = 0.0;
    double worst_miss_any = -1.0e9;

    for (int lod = 0; lod < kLodCount; ++lod) {
        const float S = lod_chunk_size(lod);
        double band_sum = 0.0, relief_sum = 0.0, chunk_sum = 0.0;
        double worst_miss = -1.0e9;
        int columns = 0;

        // Seven columns a side is the ring this level actually keeps.
        for (int cz = -kRingHalf; cz <= kRingHalf; ++cz) {
            for (int cx = -kRingHalf; cx <= kRingHalf; ++cx) {
                const float x0 = float(cx) * S, z0 = float(cz) * S;
                const float x1 = x0 + S, z1 = z0 + S;

                float lo = 0.0f, hi = 0.0f;
                world_.column_band(x0, z0, x1, z1, lo, hi);

                float tmn = 1.0e9f, tmx = -1.0e9f;
                for (int j = 0; j <= 16; ++j) {
                    for (int i = 0; i <= 16; ++i) {
                        const float x = x0 + (x1 - x0) * float(i) / 16.0f;
                        const float z = z0 + (z1 - z0) * float(j) / 16.0f;
                        const float h = world_.height(x, z);
                        if (h < tmn) tmn = h;
                        if (h > tmx) tmx = h;
                    }
                }

                // Positive means the true surface escaped the band -- the one
                // failure that hides ground. Negative is spare room.
                const double miss = double(tmn) - double(lo) < double(hi) - double(tmx)
                                        ? double(lo) - double(tmn)
                                        : double(tmx) - double(hi);
                const double over = double(lo) - double(tmn) > double(tmx) - double(hi)
                                        ? double(lo) - double(tmn)
                                        : double(tmx) - double(hi);
                (void)miss;
                if (over > worst_miss) worst_miss = over;

                band_sum += double(hi - lo);
                relief_sum += double(tmx - tmn);
                chunk_sum += double(int((hi - lo) / S) + 1);
                ++columns;
            }
        }

        std::fprintf(f, "%-5d %7.1f m %7.1f m %7.1f m %9.1f %8.2f m\n",
                     lod, S, band_sum / columns, relief_sum / columns,
                     chunk_sum / columns, worst_miss);
        total_chunks += chunk_sum;
        if (worst_miss > worst_miss_any) worst_miss_any = worst_miss;
    }

    // The table above samples columns around the ORIGIN, and the origin is a
    // gentle plain -- relief 1.8 m over a 16 m column. That is the FRIENDLY
    // case for a margin rule that scales with the relief the samples happen to
    // see, so passing there says very little. This second pass walks out
    // across the disc, where the continent mask, the mountains and the river
    // gorges are, and reports the worst case rather than the local one.
    std::fprintf(f, "\nThe same question away from the origin, which is a plain.\n");
    std::fprintf(f, "%-5s %12s %12s %s\n", "LOD", "worst miss", "at radius", "verdict");

    double worst_any = -1.0e9;
    int    worst_lod = -1;
    float  worst_x = 0.0f, worst_z = 0.0f;

    for (int lod = 0; lod < kLodCount; ++lod) {
        const float S = lod_chunk_size(lod);
        double worst = -1.0e9;
        float at_r = 0.0f, at_x = 0.0f, at_z = 0.0f;

        // Rings out to the rim. 16 directions, so the sampling is not aligned
        // to the chunk grid and does not keep landing on the same phase.
        static const float kRadii[] = {0.0f, 500.0f, 1500.0f, 3000.0f, 5000.0f,
                                       7000.0f, 8500.0f, 9500.0f};
        for (int ri = 0; ri < 8; ++ri) {
            const int spokes = ri == 0 ? 1 : 16;
            for (int sp = 0; sp < spokes; ++sp) {
                const float a = 6.2831853f * float(sp) / float(spokes) + 0.37f;
                const float px = kRadii[ri] * std::cos(a);
                const float pz = kRadii[ri] * std::sin(a);

                const float x0 = std::floor(px / S) * S;
                const float z0 = std::floor(pz / S) * S;
                const float x1 = x0 + S, z1 = z0 + S;

                float lo = 0.0f, hi = 0.0f;
                world_.column_band(x0, z0, x1, z1, lo, hi);

                float tmn = 1.0e9f, tmx = -1.0e9f;
                for (int j = 0; j <= 16; ++j) {
                    for (int i = 0; i <= 16; ++i) {
                        const float x = x0 + (x1 - x0) * float(i) / 16.0f;
                        const float z = z0 + (z1 - z0) * float(j) / 16.0f;
                        const float h = world_.height(x, z);
                        if (h < tmn) tmn = h;
                        if (h > tmx) tmx = h;
                    }
                }
                const double over = double(lo) - double(tmn) > double(tmx) - double(hi)
                                        ? double(lo) - double(tmn)
                                        : double(tmx) - double(hi);
                if (over > worst) {
                    worst = over;
                    at_r = kRadii[ri];
                    at_x = x0; at_z = z0;
                }
            }
        }

        std::fprintf(f, "%-5d %10.2f m %10.0f m %s\n", lod, worst, at_r,
                     worst > 0.0 ? "GROUND HIDDEN" : "contained");
        if (worst > worst_any) {
            worst_any = worst;
            worst_lod = lod;
            worst_x = at_x; worst_z = at_z;
        }
    }

    std::fprintf(f, "\nworst over the whole disc: %.2f m at LOD %d, column (%.0f, %.0f)\n",
                 worst_any, worst_lod, worst_x, worst_z);
    std::fprintf(f, "  %s\n", worst_any > 0.0
                     ? "POSITIVE: the band hides ground somewhere. The margin rule is wrong."
                     : "still negative everywhere, so the margin rule holds off the plain too.");

    std::fprintf(f, "\nchunks the whole clipmap wants: %.0f, against a pool of %u.\n"
                    "Most of them hold no surface and give their slot straight\n"
                    "back, so the pool is not what this number compares against\n"
                    "-- a flight measures the slots actually held.\n",
                 total_chunks, kSlots);
    std::fprintf(f, "worst the surface escapes the band, over every level: %.2f m\n",
                 worst_miss_any);
    std::fprintf(f, "  (negative is spare room; positive is ground the band hides)\n");
    std::fclose(f);
}

namespace {

// -grad, normalised. The sign is the one invariant 2's note is about: density
// grows into the rock, so the outward normal is MINUS the gradient.
void cpu_normal(const WorldGpu& w, float x, float y, float z, float voxel,
                float arm, float out[3]) {
    const float dx = w.density(x + arm, y, z, voxel) - w.density(x - arm, y, z, voxel);
    const float dy = w.density(x, y + arm, z, voxel) - w.density(x, y - arm, z, voxel);
    const float dz = w.density(x, y, z + arm, voxel) - w.density(x, y, z - arm, voxel);
    const double len = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
    if (len < 1e-20) { out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f; return; }
    out[0] = float(-dx / len);
    out[1] = float(-dy / len);
    out[2] = float(-dz / len);
}

double angle_between(const float a[3], const float b[3]) {
    double d = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    // acos is fine here: this is a metric, downstream of the golden hash on
    // the tools side of invariant 9's line.
    return std::acos(d) * 57.29577951308232;
}

}  // namespace

void Renderer::normal_check(const char* path) {
    if (!ready_) return;
    FILE* f = std::fopen(path, "w");
    if (!f) return;

    std::fprintf(f, "field_normal takes a central difference with a half-voxel\n");
    std::fprintf(f, "arm. This compares every stored normal against the CPU\n");
    std::fprintf(f, "gradient at a 0.25 m arm -- the surface's actual normal --\n");
    std::fprintf(f, "and, as a control, against the CPU at the SAME arm the\n");
    std::fprintf(f, "shader used. A large first column with a small second one\n");
    std::fprintf(f, "means the formula is wrong, not the GPU.\n\n");
    std::fprintf(f, "%-8s %7s %9s %9s %9s %9s\n", "voxel", "verts",
                 "mean err", "worst", "facing away", "vs control");

    static const float kVoxels[] = {0.5f, 2.0f, 8.0f, 32.0f, 64.0f};
    const float surface = world_.height(0.0f, 0.0f);

    for (int vi = 0; vi < 5; ++vi) {
        const float voxel = kVoxels[vi];
        const float span = voxel * float(kChunkCells);
        const float origin[3] = {-span * 0.5f, surface - span * 0.5f, -span * 0.5f};

        allocators_[frame_]->Reset();
        cmd_->Reset(allocators_[frame_].Get(), nullptr);
        mesher_.record_to_compute(cmd_.Get());
        mesher_.begin_frame_bricks();
        mesher_.record_mesh(cmd_.Get(), 0, origin, voxel, 0, 0);
        mesher_.record_counter_copy(cmd_.Get());
        mesher_.record_vertex_copy(cmd_.Get());
        mesher_.record_to_draw(cmd_.Get());
        cmd_->Close();
        ID3D12CommandList* ls[] = {cmd_.Get()};
        queue_->ExecuteCommandLists(1, ls);
        wait_for_gpu();

        UINT nv = 0, ni = 0;
        mesher_.slot_counts(0, nv, ni);
        std::vector<MeshVertex> verts;
        if (!mesher_.read_vertices(0, nv, verts) || verts.empty()) {
            std::fprintf(f, "%-8.1f %7s  (no surface in this chunk)\n", voxel, "-");
            continue;
        }

        double sum = 0.0, worst = 0.0, ctl_worst = 0.0;
        int away = 0, counted = 0;
        float worst_at[3] = {0.0f, 0.0f, 0.0f};
        for (size_t i = 0; i < verts.size(); i += 3) {
            const MeshVertex& v = verts[i];
            // The skirt is off the isosurface on purpose, and its normal is
            // the surface vertex's at half length.
            if (is_skirt_vertex(v.nrm)) continue;

            float truth[3], control[3];
            cpu_normal(world_, v.pos[0], v.pos[1], v.pos[2], voxel, 0.25f, truth);
            cpu_normal(world_, v.pos[0], v.pos[1], v.pos[2], voxel, 0.5f * voxel, control);

            const double err = angle_between(v.nrm, truth);
            const double ctl = angle_between(v.nrm, control);
            if (err > 90.0) ++away;
            if (err > worst) {
                worst = err;
                worst_at[0] = v.pos[0]; worst_at[1] = v.pos[1]; worst_at[2] = v.pos[2];
            }
            if (ctl > ctl_worst) ctl_worst = ctl;
            sum += err;
            ++counted;
        }
        if (counted == 0) { std::fprintf(f, "%-8.1f (all lip)\n", voxel); continue; }

        std::fprintf(f, "%-8.1f %7d %7.1f deg %6.1f deg %7.1f%% %6.1f deg\n",
                     voxel, counted, sum / counted, worst,
                     100.0 * double(away) / double(counted), ctl_worst);
        if (vi == 4) {
            std::fprintf(f, "\n  worst at (%.0f, %.0f, %.0f)\n",
                         worst_at[0], worst_at[1], worst_at[2]);
        }
    }

    std::fprintf(f, "\n'facing away' is the share of normals more than 90 deg\n");
    std::fprintf(f, "from the surface's own -- lit as though the surface faced\n");
    std::fprintf(f, "the other way, which is what a black polygon in daylight is.\n");
    std::fclose(f);
}

bool Renderer::probe_field(const float origin[3], float voxel, const char* path,
                           std::string& error) {
    if (!ready_) { error = "renderer not initialised"; return false; }

    // Written BEFORE any GPU work: a hung device takes the process with it,
    // and the numbers that caused the hang are the ones worth having.
    {
        const WorldConstants& c = world_.constants();
        const worldgen::SeedWorld& sw = world_.world();
        if (FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "cells %d over %d x %d buckets of %.1f m\n",
                         c.cell_count, c.cell_buckets, c.cell_buckets,
                         c.cell_bucket_size);
            std::fprintf(f, "biomes %d, radius %.1f, continent octaves %d\n",
                         c.biome_count, c.radius, c.continent_octaves);
            std::fprintf(f, "rivers: %d x %d bins of %.1f m\n",
                         c.river_grid, c.river_grid, c.river_bin_size);
            std::fprintf(f, "entrances: %d x %d bins of %.1f m\n",
                         c.entrance_grid, c.entrance_grid, c.entrance_bin_size);
            size_t worst = 0;
            for (size_t i = 0; i < sw.lookup.buckets.size(); ++i)
                if (sw.lookup.buckets[i].size() > worst)
                    worst = sw.lookup.buckets[i].size();
            std::fprintf(f, "fullest bucket holds %zu cells\n", worst);
            int maxoct = 0;
            for (size_t i = 0; i < world_.table().size(); ++i)
                if (world_.table()[i].octaves > maxoct)
                    maxoct = world_.table()[i].octaves;
            std::fprintf(f, "largest biome octave count %d\n\n", maxoct);
            std::fclose(f);
        }
    }

    allocators_[frame_]->Reset();
    cmd_->Reset(allocators_[frame_].Get(), nullptr);
    mesher_.record_to_compute(cmd_.Get());
    mesher_.begin_frame_bricks();
    mesher_.record_mesh(cmd_.Get(), 0, origin, voxel, 0, 0);
    mesher_.record_density_copy(cmd_.Get());
    mesher_.record_counter_copy(cmd_.Get());
    mesher_.record_to_draw(cmd_.Get());
    cmd_->Close();
    ID3D12CommandList* lists[] = {cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();

    const float* gpu = mesher_.mapped_density();
    if (!gpu) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "could not map the density readback: Map 0x%08lx, "
                      "device removed 0x%08lx",
                      (unsigned long)mesher_.last_map_hr(),
                      (unsigned long)device_->GetDeviceRemovedReason());
        error = buf;
        return false;
    }

    const worldgen::SeedWorld& w = world_.world();
    const worldgen::WorldParams& wp = world_.params();
    const worldgen::ArchetypeTable& tbl = world_.table();

    double worst = 0.0, sum = 0.0;
    int n = 0, gpu_rock = 0, gpu_air = 0, cpu_rock = 0, cpu_air = 0;
    float gmin = 1e30f, gmax = -1e30f, cmin = 1e30f, cmax = -1e30f;
    float worst_at[3] = {0.0f, 0.0f, 0.0f};
    float worst_pair[2] = {0.0f, 0.0f};

    for (UINT k = 0; k < kChunkCorners; k += 4) {
        for (UINT j = 0; j < kChunkCorners; j += 4) {
            for (UINT i = 0; i < kChunkCorners; i += 4) {
                const float x = origin[0] + float(i) * voxel;
                const float y = origin[1] + float(j) * voxel;
                const float z = origin[2] + float(k) * voxel;
                const float g = gpu[(k * kChunkCorners + j) * kChunkCorners + i];
                const float c = worldgen::terrain_density(w, wp, tbl, x, y, z, voxel);
                if (g > 0.0f) ++gpu_rock; else ++gpu_air;
                if (c > 0.0f) ++cpu_rock; else ++cpu_air;
                if (g < gmin) gmin = g;
                if (g > gmax) gmax = g;
                if (c < cmin) cmin = c;
                if (c > cmax) cmax = c;
                const double d = double(g) - double(c);
                const double a = d < 0.0 ? -d : d;
                if (a > worst) {
                    worst = a;
                    worst_at[0] = x; worst_at[1] = y; worst_at[2] = z;
                    worst_pair[0] = g; worst_pair[1] = c;
                }
                sum += a;
                ++n;
            }
        }
    }
    mesher_.unmap_density();

    UINT verts = 0, inds = 0;
    mesher_.slot_counts(0, verts, inds);

    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(f, "one chunk at (%.1f, %.1f, %.1f), voxel %.2f m\n\n",
                     origin[0], origin[1], origin[2], voxel);
        std::fprintf(f, "GPU density over %d sampled corners: %.3f .. %.3f"
                        " (%d rock, %d air)\n", n, gmin, gmax, gpu_rock, gpu_air);
        std::fprintf(f, "CPU density over the same corners:   %.3f .. %.3f"
                        " (%d rock, %d air)\n", cmin, cmax, cpu_rock, cpu_air);
        std::fprintf(f, "\nworst disagreement: %.6f at (%.1f, %.1f, %.1f)\n",
                     worst, worst_at[0], worst_at[1], worst_at[2]);
        std::fprintf(f, "  GPU said %.4f, CPU said %.4f\n", worst_pair[0], worst_pair[1]);
        std::fprintf(f, "mean disagreement: %.6f\n", n ? sum / n : 0.0);
        std::fprintf(f, "\nthe chunk meshed to %u vertices and %u indices\n", verts, inds);
        std::fclose(f);
    }
    return true;
}

bool Renderer::save_edits(const char* path, std::string& error) const {
    return edits_.save(path, world_.field_id(), error);
}

bool Renderer::load_edits(const char* path, std::string& error, bool& field_mismatch) {
    if (!edits_.load(path, world_.field_id(), error, field_mismatch)) return false;
    // Everything resident was meshed from terrain that has just changed, and
    // every lease points at pool contents that are now wrong.
    pager_.init(kMaxBricks);
    const float lo[3] = {-1.0e9f, -1.0e9f, -1.0e9f};
    const float hi[3] = {1.0e9f, 1.0e9f, 1.0e9f};
    streamer_.invalidate_box(lo, hi);
    return true;
}

bool Renderer::dig(float x, float y, float z, float radius, float strength,
                   unsigned char material) {
    float lo[3], hi[3];
    if (!edits_.edit_sphere(x, y, z, radius, strength, material, lo, hi)) return false;
    streamer_.invalidate_box(lo, hi);
    stats_.chunks_invalidated = streamer_.last_invalidated();
    return true;
}

void Renderer::slot_report(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::vector<UINT> c;
    mesher_.read_all_counts(c);
    const UINT edges[] = {0, 256, 512, 1024, 2048, 3072, 4096, kSlotVertices};
    const int nb = int(sizeof(edges) / sizeof(edges[0]));
    unsigned long long hist[16] = {0};
    std::vector<UINT> used_v, used_i;
    UINT over_v = 0, over_i = 0, worst_v = 0, worst_i = 0;
    for (UINT s = 0; s < kSlots; ++s) {
        const UINT v = c[s * 2], i = c[s * 2 + 1];
        if (v > worst_v) worst_v = v;
        if (i > worst_i) worst_i = i;
        if (v > kSlotVertices) ++over_v;
        if (i > kSlotIndices) ++over_i;
        if (v == 0) continue;
        used_v.push_back(v); used_i.push_back(i);
        int b = 0;
        while (b + 1 < nb && v > edges[b + 1]) ++b;
        if (v > kSlotVertices) b = nb;
        ++hist[b];
    }
    std::sort(used_v.begin(), used_v.end());
    std::sort(used_i.begin(), used_i.end());
    const double slot_bytes = double(kSlotVertices) * sizeof(MeshVertex) +
                              double(kSlotIndices) * 4.0 + double(kHeightSamples) * 4.0;
    std::fprintf(f, "slot pool: %u slots of %u vertices / %u indices, %.0f KB each, %.0f MB total\n",
                 kSlots, kSlotVertices, kSlotIndices, slot_bytes / 1024.0,
                 slot_bytes * kSlots / (1024.0 * 1024.0));
    std::fprintf(f, "GPU dedicated memory: %.0f MB\n", double(adapter_vram_) / (1024.0 * 1024.0));
    std::fprintf(f, "slots holding geometry: %zu; free %zu; chunks still queued %zu\n",
                 used_v.size(), streamer_.free_slot_count(), streamer_.pending_count());
    if (!used_v.empty()) {
        std::fprintf(f, "vertices per chunk: median %u, 90th %u, 99th %u, max %u\n",
                     used_v[used_v.size() / 2], used_v[used_v.size() * 9 / 10],
                     used_v[used_v.size() * 99 / 100], worst_v);
        std::fprintf(f, "indices per chunk:  median %u, 90th %u, 99th %u, max %u\n",
                     used_i[used_i.size() / 2], used_i[used_i.size() * 9 / 10],
                     used_i[used_i.size() * 99 / 100], worst_i);
        unsigned long long sv = 0;
        for (size_t k = 0; k < used_v.size(); ++k) sv += used_v[k];
        std::fprintf(f, "share of reserved vertex space used: %.1f%%\n",
                     100.0 * double(sv) / (double(used_v.size()) * kSlotVertices));
    }
    std::fprintf(f, "over the cap: %u on vertices, %u on indices\n", over_v, over_i);
    std::fprintf(f, "vertex histogram:\n");
    for (int b = 0; b + 1 < nb; ++b)
        std::fprintf(f, "  %5u - %5u: %llu\n", edges[b] + 1, edges[b + 1], hist[b]);
    std::fprintf(f, "  over the cap: %llu\n", hist[nb]);
    std::fclose(f);
}

void Renderer::shutdown() {
    if (ready_) wait_for_gpu();
    tree_field_.shutdown();
    trees_.shutdown();
    ready_ = false;
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}

}  // namespace engine
