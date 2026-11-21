// ---------------------------------------------------------------------------
// Volumetric mist (ADR-004 L4): a froxel volume, 160 x 90 x 128, aligned to
// the view and log-spaced in depth from 0.5 m to 6 km.
//
//   cs_fog_inject     per froxel: the mist's density (fog.h's baked map, the
//                     hour, wind-drifted patches), and the light it scatters
//                     towards the eye -- the key light through its cascade
//                     shadow (which is what draws shafts through a canopy),
//                     the sky and the ground, and every local light in the
//                     froxel's cluster through its own shadow (a torch's glow
//                     in mist). Blended with last frame's, reprojected, over
//                     a jittered position in the slice, so 128 slices read as
//                     smooth.
//   cs_fog_integrate  per column, front to back: in-scattered light and
//                     transmittance accumulated to the far side of each slice.
//
// The post chain's composite reads the result at each pixel's depth, over the
// atmosphere's own aerial perspective (the mist is nearer than the bulk of
// the air), and at the far end for the sky.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/shaders/local_lights.hlsli"

cbuffer FogConstants : register(b0) {
    row_major float4x4 prev_view_proj;   // last frame, unjittered
    float3 cam_pos;    float time;
    float3 cam_right;  float tan_half_fov;
    float3 cam_up;     float aspect;
    float3 cam_fwd;    float jitter;      // where in its slice this frame samples, 0..1
    float  near_z;     float log_ratio;   uint history_valid; float time_factor;
    float  map_extent; float haze;        float sea_level;    float patch;
    float2 drift;      float phase_g;     float albedo;   // drift: metres the patches have moved (weather.h)
    float  screen_w;   float screen_h;    float density_scale; float cam_sky;   // cam_sky: the heightfield GI's share of open sky at the eye
    float4 cloud_shadow;  // ADR-005 C2: the cloud-shadow map's centre x, z, half extent (0: none), base altitude
};

Texture2D<float4>   g_fog_map    : register(t0);   // fog.h: density, thinning height, surface y
Texture3D<float4>   g_history    : register(t1);
Texture3D<float4>   g_inject_in  : register(t2);
RWTexture3D<float4> u_inject     : register(u0);
RWTexture3D<float4> u_integrated : register(u1);
SamplerState        g_lin        : register(s0);
Texture2D<float>    g_cloud_shadow : register(t60);   // clouds.hlsl cs_shadow, last frame's
#include "engine/render/shaders/cloud_shadow.hlsli"

static const uint kFX = 160, kFY = 90, kFZ = 128;

float slice_z(float s) { return near_z * exp(s / float(kFZ) * log_ratio); }

float3 froxel_world(float2 uv, float z) {
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return cam_pos + cam_fwd * z + cam_right * (ndc.x * tan_half_fov * aspect * z) + cam_up * (ndc.y * tan_half_fov * z);
}

float hash31(int3 c) {
    uint h = asuint(c.x) * 0x8da6b343u ^ asuint(c.y) * 0xd8163841u ^ asuint(c.z) * 0xcb1ab31fu;
    h = (h ^ (h >> 13)) * 0x9e3779b1u;
    h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.0f;
}

float vnoise3(float3 p) {
    const float3 c = floor(p);
    const float3 f = p - c;
    const float3 u = f * f * (3.0f - 2.0f * f);
    const int3 i = int3(c);
    const float a = lerp(hash31(i), hash31(i + int3(1, 0, 0)), u.x);
    const float b = lerp(hash31(i + int3(0, 1, 0)), hash31(i + int3(1, 1, 0)), u.x);
    const float d = lerp(hash31(i + int3(0, 0, 1)), hash31(i + int3(1, 0, 1)), u.x);
    const float e = lerp(hash31(i + int3(0, 1, 1)), hash31(i + int3(1, 1, 1)), u.x);
    return lerp(lerp(a, b, u.y), lerp(d, e, u.y), u.z);
}

// Extinction per metre. The map's mist, thinning with height above the
// surface it lies on, times the hour; broken into patches that drift on the
// wind and are flattened, as banks of mist are; and the haze everywhere. The
// drift is the weather's integral of the wind, never wind x time, which
// jumps the moment the wind changes.
float fog_density(float3 p) {
    const float4 m = g_fog_map.SampleLevel(g_lin, p.xz / map_extent + 0.5f, 0);
    const float h = max(p.y - m.z, 0.0f);
    float d = m.x * exp(-h / max(m.y, 1.0f)) * time_factor;
    const float3 q = (p - float3(drift.x, 0.0f, drift.y)) / patch * float3(1.0f, 3.0f, 1.0f);
    const float n = 0.65f * vnoise3(q) + 0.35f * vnoise3(q * 3.1f + 17.0f);
    d *= saturate(1.6f * n - 0.15f);
    return (d + haze) * density_scale;
}

float hg_phase(float c, float g) {
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * kPi * pow(max(1.0f + g2 - 2.0f * g * c, 1e-4f), 1.5f));
}

// The key light's visibility: one comparison tap. The volume and its
// temporal blend do the filtering a surface's PCF does.
float key_visibility(float3 p) {
    if (shadow_res < 0.0f) return 1.0f;
    const float d = length(p - shadow_cam);
    if (d >= shadow_split.w) return 1.0f;
    // Under RT shadows the near two cascades are not drawn (l_shadow_frame.z
    // is the first that is): the mist reads the third, fitted round the
    // camera, whose 0.19 m texels are far finer than a froxel.
    const uint c = max(d < shadow_split.x ? 0u : (d < shadow_split.y ? 1u : (d < shadow_split.z ? 2u : 3u)),
                       uint(l_shadow_frame.z + 0.5f));
    const float4 lp = mul(float4(p, 1.0f), light_vp[c]);
    const float2 uv = float2(lp.x * 0.5f + 0.5f, 0.5f - lp.y * 0.5f) * shadow_scale[c];
    return g_shadow.SampleCmpLevelZero(g_shadow_cmp, float3(uv, float(c)), lp.z - 0.12f / shadow_depth_range[c]);
}

[numthreads(8, 8, 1)]
void cs_fog_inject(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kFX || id.y >= kFY || id.z >= kFZ) return;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(kFX, kFY);
    const float z = slice_z(float(id.z) + jitter);
    const float3 p = froxel_world(uv, z);
    const float3 d = normalize(p - cam_pos);
    const float sigma_t = fog_density(p);
    const float sigma_s = sigma_t * albedo;

    // Radiance arriving at the point, weighted by the phase towards the eye.
    // The cosine is between the light's direction of travel (-l) and the
    // scattered ray's (-d): looking into the light is the forward peak.
    // Through the clouds' shadow too (ADR-005 C2): sunbeams through the gaps
    // in a deck, and no sunlit glow under a closed one.
    float3 L = kSunLux * (key_visibility(p) * cloud_shadow_at(g_cloud_shadow, g_lin, cloud_shadow, kSunDir, p) *
                          hg_phase(dot(d, kSunDir), phase_g));
    // The sky above and the ground below, scattered isotropically: the mean
    // radiance over the sphere.
    // Near the camera, only the share of it the probes say reaches the eye:
    // mist in a sealed chamber glowed with daylight (--leak-test, 10 nits).
    L += 0.5f * (kSkyMean + kGroundRadiance) * lerp(cam_sky, 1.0f, saturate((length(p - shadow_cam) - 64.0f) / 192.0f));
    if (l_cluster.w > 0.5f) {
        const uint c = cluster_of(p, float4(uv * float2(screen_w, screen_h), 0.0f, 1.0f));
        const uint n = g_cluster_n[c];
        [loop] for (uint i = 0; i < n; ++i) {
            const LocalLightGpu Lt = g_lights[g_cluster_ix[c * kClusterMaxItems + i]];
            float3 l;
            const float3 E = light_illuminance(Lt, p, l);
            if (max(E.r, max(E.g, E.b)) <= 0.0f) continue;
            L += E * (hg_phase(dot(d, l), phase_g) * local_shadow(Lt, p, l, l));
        }
    }
    // A lightning flash (ADR-005 C5, flash.hlsli's l_flash): in rain the mist
    // is most of what is seen, and a flash lights all of it -- the sky
    // flickers white -- where the clouds behind are lost in it.
    float flash_lit = 0.0f;
    if (l_flash.w > 0.0f) {
        const float3 fd = l_flash.xyz - p;
        const float r2 = max(dot(fd, fd), 1e4f);
        L += l_flash_col.rgb * (l_flash.w / r2) * hg_phase(dot(d, fd * rsqrt(r2)), phase_g);
        flash_lit = 1.0f;
    }
    float4 cur = float4(L * sigma_s, sigma_t);

    if (history_valid != 0u) {
        // Where this froxel's centre was last frame: perspective w is view
        // depth, which is what the slices are spaced in.
        const float3 pc = froxel_world(uv, slice_z(float(id.z) + 0.5f));
        const float4 c = mul(float4(pc, 1.0f), prev_view_proj);
        if (c.w > 0.0f) {
            const float2 puv = float2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f);
            const float ps = log(max(c.w, 1e-3f) / near_z) / log_ratio * float(kFZ);
            if (all(puv >= 0.0f) && all(puv <= 1.0f) && ps >= 0.0f && ps <= float(kFZ)) {
                const float4 h = g_history.SampleLevel(g_lin, float3(puv, ps / float(kFZ)), 0);
                // A flash lasts a few frames: take this frame whole while it
                // is lit, or 1:12 smears a 30 ms stroke to nothing; the
                // history's decay after it is a short afterglow.
                cur = lerp(h, cur, flash_lit > 0.5f ? 1.0f : 0.08f);
            }
        }
    }
    u_inject[id] = cur;
}

[numthreads(8, 8, 1)]
void cs_fog_integrate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kFX || id.y >= kFY) return;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(kFX, kFY);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    // Metres along the ray per metre of view depth.
    const float stretch = length(float3(ndc.x * tan_half_fov * aspect, ndc.y * tan_half_fov, 1.0f));
    float3 S = float3(0.0f, 0.0f, 0.0f);
    float T = 1.0f;
    float z0 = 0.0f;
    for (uint s = 0; s < kFZ; ++s) {
        const float z1 = slice_z(float(s + 1));
        const float4 v = g_inject_in[uint3(id.xy, s)];
        const float st = max(v.a, 1e-7f);
        const float Ts = exp(-st * (z1 - z0) * stretch);
        S += T * (v.rgb - v.rgb * Ts) / st;
        T *= Ts;
        u_integrated[uint3(id.xy, s)] = float4(min(S, kSceneMaxNits), T);
        z0 = z1;
    }
}
