// ---------------------------------------------------------------------------
// The atmosphere's per-frame tables (ADR-004 L2, Hillaire 2020):
//
//   cs_sky_view  the whole sky round the camera, 192 x 108, world azimuth by
//                zenith angle (atmosphere.h): what the post chain draws behind
//                everything, sun and moon included.
//   cs_aerial    32 x 32 x 32 froxels of in-scattered light and transmittance
//                along each view ray, out to the far plane on a square-root
//                spacing: the haze between the camera and every surface,
//                applied in the post chain's composite.
//
// The static tables -- transmittance and multiple scattering -- are built on
// the CPU by the same atmosphere_march.h and uploaded; sampling them here
// uses the same parameterisation, so the two sides cannot drift.
// ---------------------------------------------------------------------------

#include "engine/render/atmosphere.h"

cbuffer AtmosphereConstants : register(b0) {
    row_major float4x4 inv_view_proj;
    float3 cam_pos;   float cam_alt_km;
    float3 sun_dir;   float far_plane;
    float3 sun_lux;   float pad0;     // at the top of the atmosphere
    float3 moon_dir;  float pad1;
    float3 moon_lux;  float pad2;
};

Texture2D<float4>   g_trans : register(t0);
Texture2D<float4>   g_ms    : register(t1);
RWTexture2D<float4> u_sky   : register(u0);
RWTexture3D<float4> u_ap    : register(u1);
SamplerState        g_lin   : register(s0);

float3 atm_sample_transmittance(float r, float mu) {
    float u, v;
    atm_trans_uv(r, mu, u, v);
    return g_trans.SampleLevel(g_lin, float2(u, v), 0).rgb;
}

float3 atm_sample_ms(float r, float mu_s) {
    const float u = saturate(mu_s * 0.5f + 0.5f);
    const float v = saturate((r - kAtmGround) / (kAtmTop - kAtmGround));
    return g_ms.SampleLevel(g_lin, float2(u, v), 0).rgb;
}

#include "engine/render/atmosphere_march.h"

AtmLights frame_lights() {
    AtmLights l;
    l.dir0 = sun_dir;  l.lux0 = sun_lux;
    l.dir1 = moon_dir; l.lux1 = moon_lux;
    return l;
}

[numthreads(8, 8, 1)]
void cs_sky_view(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(kAtmSkyW) || id.y >= uint(kAtmSkyH)) return;
    const float r = kAtmGround + cam_alt_km;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(kAtmSkyW, kAtmSkyH);
    const float3 d = atm_sky_dir(uv.x, uv.y, r);
    const AtmState s = atm_march(float3(0.0f, r, 0.0f), d, 30, frame_lights(), false);
    u_sky[id.xy] = float4(s.L, 1.0f);
}

static const uint kApSize = 32;

[numthreads(8, 8, 1)]
void cs_aerial(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kApSize || id.y >= kApSize) return;
    const float2 uv = (float2(id.xy) + 0.5f) / float(kApSize);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(float4(ndc, 1.0f, 1.0f), inv_view_proj);
    const float3 d = normalize(h.xyz / h.w - cam_pos);
    const float r = kAtmGround + cam_alt_km;
    const float3 o = float3(0.0f, r, 0.0f);
    const AtmLights lights = frame_lights();
    AtmState s = atm_state();
    float t0 = 0.0f;
    for (uint k = 0; k < kApSize; ++k) {
        // Slice k ends at far * ((k + 1) / 32)^2: fine near the camera, where
        // a slice is a few metres, coarse at the horizon, where the haze is.
        const float f = float(k + 1) / float(kApSize);
        const float t1 = far_plane * f * f / 1000.0f;   // km
        s = atm_integrate(s, o, d, t0, t1, 1, lights, false);
        t0 = t1;
        u_ap[uint3(id.xy, k)] = float4(s.L, dot(s.T, float3(1.0f, 1.0f, 1.0f) / 3.0f));
    }
}
