// ---------------------------------------------------------------------------
// Volumetric clouds (docs/adr-005-clouds-and-rain.md, phase C1), after
// Schneider's Nubis (Horizon Zero Dawn, SIGGRAPH 2015 / 2017) with
// Hillaire's integration and multiple scattering (SIGGRAPH 2016).
//
//   cs_noise_base    once: 128^3 tileable Perlin-Worley (R) + Worley octaves (GBA)
//   cs_noise_detail  once: 32^3 tileable Worley octaves (RGB)
//   cs_weather       each frame: the weather map over +-map_extent metres --
//                    coverage, cloud type, precipitation -- from the storm
//                    dial and a 2D noise carried by the wind aloft
//   cs_march         each frame, quarter resolution: the march through the
//                    cloud shell, jittered per frame: (in-scatter, T), distance
//   cs_temporal      reproject last frame's by each pixel's cloud distance,
//                    clamp to this frame's 3x3, blend; also into `out`, the
//                    texture the post composite reads at a fixed slot
//
// The planet is the atmosphere's (kAtmGround km): world y = 0 is sea level
// on its surface, its centre (0, -R, 0) metres. Radiance in nits, as the
// atmosphere's (ADR-004 L1).
// ---------------------------------------------------------------------------

#include "engine/render/atmosphere.h"
#include "engine/render/wind.h"

cbuffer CloudConstants : register(b0) {
    row_major float4x4 inv_view_proj;   // this frame, unjittered
    row_major float4x4 prev_view_proj;  // last frame, unjittered, world -> clip
    float3 cam_pos;     float time;
    float3 key_dir;     float cam_alt_km;   // towards the key light (sun or moon)
    float3 key_lux_top; float frame;        // its illuminance at the top of the atmosphere
    float3 sky_mean;    float coverage;     // the sky's mean radiance; the storm's coverage
    float3 ground;      float cloud_type;   // the lit ground's radiance; the storm's type, 0 stratus .. 1 cumulonimbus
    float2 drift;       float2 wind_dir;    // metres the clouds have moved; the wind's heading
    float  base_alt;    float sigma;        float base_scale; float detail_scale;   // m, 1/m at density 1, m, m
    float2 res;         float2 inv_res;     // the march's resolution
    float  map_extent;  float history_valid; float precip;  float shear;          // m; 0/1; 0..1; m of downwind lean at the top
    uint   prev_index;  float haze_km;      float detail_amount; float pad0;
    float4 shadow_map;  // the cloud-shadow map: centre x, z in the cloud-base plane; half extent m; texels
};

Texture3D<float4> g_base     : register(t0);
Texture3D<float4> g_detail   : register(t1);
Texture2D<float4> g_weather  : register(t2);
Texture2D<float4> g_trans    : register(t3);   // the atmosphere's transmittance table
Texture2D<float4> g_skyview  : register(t4);   // the atmosphere's sky-view table
Texture2D<float4> g_raw      : register(t5);
Texture2D<float>  g_raw_dist : register(t6);
Texture2D<float4> g_hist0    : register(t7);
Texture2D<float>  g_hdist0   : register(t8);
Texture2D<float4> g_hist1    : register(t9);
Texture2D<float>  g_hdist1   : register(t10);

RWTexture3D<float4> u_base     : register(u0);
RWTexture3D<float4> u_detail   : register(u1);
RWTexture2D<float4> u_weather  : register(u2);
RWTexture2D<float4> u_raw      : register(u3);
RWTexture2D<float>  u_raw_dist : register(u4);
RWTexture2D<float4> u_hist0    : register(u5);
RWTexture2D<float>  u_hdist0   : register(u6);
RWTexture2D<float4> u_hist1    : register(u7);
RWTexture2D<float>  u_hdist1   : register(u8);
RWTexture2D<float4> u_out      : register(u9);
RWTexture2D<float>  u_out_dist : register(u10);
RWTexture2D<float>  u_shadow   : register(u11);   // the cloud-shadow map (post.hlsl reads it)

SamplerState g_wrap   : register(s0);   // the noise volumes tile
SamplerState g_clamp  : register(s1);
SamplerState g_wrap_u : register(s2);   // the sky-view table wraps in azimuth

static const float kR = kAtmGround * 1000.0f;   // metres

float remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5f) * (nhi - nlo);
}

// --- tileable noise for the volumes -------------------------------------------

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float3 hash33(int3 c, uint salt) {
    uint h = pcg(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ salt);
    const float a = float(h & 1023u) / 1023.0f;
    h = pcg(h);
    const float b = float(h & 1023u) / 1023.0f;
    h = pcg(h);
    const float d = float(h & 1023u) / 1023.0f;
    return float3(a, b, d);
}
int3 wrap3(int3 c, int period) { return ((c % period) + period) % period; }

// Inverted Worley: 1 at a feature point, falling to 0 a cell away. Tiles
// with `period` cells over [0, 1).
float worley(float3 p, int period, uint salt) {
    const float3 x = p * float(period);
    const int3 i = int3(floor(x));
    const float3 f = x - float3(i);
    float d = 1.0f;
    [unroll] for (int oz = -1; oz <= 1; ++oz)
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    [unroll] for (int ox = -1; ox <= 1; ++ox) {
        const int3 o = int3(ox, oy, oz);
        const float3 r = float3(o) + hash33(wrap3(i + o, period), salt) - f;
        d = min(d, dot(r, r));
    }
    return 1.0f - saturate(sqrt(d));
}
float worley_fbm(float3 p, int period, uint salt) {
    return worley(p, period, salt) * 0.625f + worley(p, period * 2, salt + 1u) * 0.25f +
           worley(p, period * 4, salt + 2u) * 0.125f;
}

// Periodic gradient (Perlin) noise in [-1, 1].
float perlin(float3 p, int period, uint salt) {
    const float3 x = p * float(period);
    const int3 i = int3(floor(x));
    const float3 f = x - float3(i);
    const float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float n[8];
    [unroll] for (int k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const float3 g = normalize(hash33(wrap3(i + o, period), salt) * 2.0f - 1.0f + 1e-4f);
        n[k] = dot(g, f - float3(o));
    }
    const float x00 = lerp(n[0], n[1], u.x), x10 = lerp(n[2], n[3], u.x);
    const float x01 = lerp(n[4], n[5], u.x), x11 = lerp(n[6], n[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

[numthreads(4, 4, 4)]
void cs_noise_base(uint3 id : SV_DispatchThreadID) {
    if (any(id >= 128u)) return;
    const float3 p = (float3(id) + 0.5f) / 128.0f;
    // Perlin fbm, three octaves, into [0, 1].
    const float pf = saturate(0.5f + 0.5f * (perlin(p, 4, 11u) * 0.625f + perlin(p, 8, 12u) * 0.25f +
                                             perlin(p, 16, 13u) * 0.125f) * 1.4f);
    const float w0 = worley_fbm(p, 4, 21u);
    // Perlin-Worley: the Perlin's billows given Worley's cellular edges.
    const float pw = saturate(remap(pf, 0.0f, 1.0f, w0, 1.0f));
    u_base[id] = float4(pw, w0, worley_fbm(p, 8, 31u), worley_fbm(p, 16, 41u));
}

[numthreads(4, 4, 4)]
void cs_noise_detail(uint3 id : SV_DispatchThreadID) {
    if (any(id >= 32u)) return;
    const float3 p = (float3(id) + 0.5f) / 32.0f;
    u_detail[id] = float4(worley_fbm(p, 2, 51u), worley_fbm(p, 4, 61u), worley_fbm(p, 8, 71u), 1.0f);
}

// --- the weather map ----------------------------------------------------------

float fbm2(float2 q) {
    return wind_vnoise2(q.x, q.y) * 0.5f + wind_vnoise2(q.x * 2.03f + 17.1f, q.y * 2.03f - 3.7f) * 0.3f +
           wind_vnoise2(q.x * 4.11f - 9.3f, q.y * 4.11f + 5.2f) * 0.2f;
}

// R coverage, G type, B precipitation. The storm dial sets the mean; the
// noise, carried at the clouds' drift, breaks it into fields of cloud and
// clear. A gale's sky is nearly closed, a calm day's a few fair-weather
// cumulus, and between them the clouds come in banks.
[numthreads(8, 8, 1)]
void cs_weather(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    u_weather.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const float2 p = ((float2(id.xy) + 0.5f) / float2(w, h) * 2.0f - 1.0f) * map_extent;
    const float2 q = (p - drift) / 9000.0f;
    // How bad the weather is belongs to the TIME, the same over the whole
    // sky: the storm dial sets coverage and type, and the map only lets them
    // drift a little (+-0.15), so the sky is not uniform. At +-0.55 the map's
    // low spots were clear holes several kilometres wide, and with the clouds
    // barely drifted at the start of a session one always sat over spawn.
    // The 3D noise shapes the clouds; this does not.
    const float cn = fbm2(q);
    const float cov = saturate(coverage + (cn - 0.5f) * 0.3f);
    const float tn = fbm2(q * 0.45f + 31.0f);
    const float type = saturate(cloud_type + (tn - 0.5f) * 0.3f);
    const float pr = saturate((cov - 0.65f) * 3.0f) * saturate(type * 1.6f - 0.6f) * precip;
    u_weather[id.xy] = float4(cov, type, pr, 0.0f);
}

// --- the cloud field ----------------------------------------------------------

// The cloud's top over its base, by type: a stratus sheet ~400 m thick, a
// cumulus ~1.8 km, a cumulonimbus 8 km.
float type_top(float type) {
    return type < 0.5f ? lerp(400.0f, 1800.0f, type * 2.0f) : lerp(1800.0f, 8000.0f, (type - 0.5f) * 2.0f);
}
static const float kMaxTop = 8000.0f;

// The density profile up a cloud of this type (Schneider): stratus a flat
// sheet, cumulus rounded with a flat base, cumulonimbus tall with an anvil.
float height_profile(float hf, float type) {
    const float strat = smoothstep(0.0f, 0.15f, hf) * (1.0f - smoothstep(0.35f, 0.8f, hf));
    const float cumu = smoothstep(0.0f, 0.12f, hf) * (1.0f - smoothstep(0.45f, 1.0f, hf));
    const float cb = smoothstep(0.0f, 0.06f, hf) * (1.0f - smoothstep(0.8f, 1.0f, hf));
    return type < 0.5f ? lerp(strat, cumu, type * 2.0f) : lerp(cumu, cb, (type - 0.5f) * 2.0f);
}

float altitude_of(float3 p) {
    return length(p + float3(0.0f, kR, 0.0f)) - kR;
}

float4 weather_at(float3 p) {
    const float2 uv = p.xz / (2.0f * map_extent) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f)) {
        // Past the map, the storm's mean: the sky does not end at a square.
        return float4(saturate(coverage), cloud_type, 0.0f, 0.0f);
    }
    return g_weather.SampleLevel(g_clamp, uv, 0);
}

// Density 0..1 at world point p. `fine` adds the detail erosion (the march's
// own samples); the light march leaves it out.
float cloud_density(float3 p, float4 wx, bool fine) {
    const float alt = altitude_of(p) - base_alt;
    const float top = type_top(wx.g);
    const float hf = alt / top;
    if (hf <= 0.0f || hf >= 1.0f) return 0.0f;
    // Carried by the wind aloft, and leaning downwind with height.
    const float3 q = p - float3(drift.x, 0.0f, drift.y) - float3(wind_dir.x, 0.0f, wind_dir.y) * (shear * hf);
    const float4 b = g_base.SampleLevel(g_wrap, float3(q.x, alt, q.z) / base_scale, 0);
    const float low = b.g * 0.625f + b.b * 0.25f + b.a * 0.125f;
    float c = saturate(remap(b.r, -(1.0f - low), 1.0f, 0.0f, 1.0f));
    c *= height_profile(hf, wx.g);
    // Coverage as a threshold on the shaped field (Schneider). The field
    // rarely passes 0.7 once its height profile is applied, so the threshold
    // is scaled to that: with Schneider's plain 1 - coverage nothing at all
    // stood under half cover, and a map that varied round 0.5 left whole
    // regions of the sky empty. 0.3 is scattered cumulus, 0.5 broken, 0.9 a
    // deck.
    const float cov = wx.r;
    c = saturate(remap(c, 0.7f * (1.0f - cov), 1.0f, 0.0f, 1.0f)) * saturate(cov * 1.5f);
    if (c <= 0.0f || !fine) return c;
    // Erode the edges: billowy below, wispy above.
    const float3 d = g_detail.SampleLevel(g_wrap, q / detail_scale + float3(0.0f, time * 0.004f, 0.0f), 0).rgb;
    const float hfn = d.r * 0.625f + d.g * 0.25f + d.b * 0.125f;
    const float m = lerp(hfn, 1.0f - hfn, saturate(hf * 6.0f));
    return saturate(remap(c, m * detail_amount, 1.0f, 0.0f, 1.0f));
}

float hg(float c, float g) {
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(1.0f + g2 - 2.0f * g * c, 1e-4f), 1.5f));
}
// Dual-lobe: a strong forward lobe (the silver lining round a cloud in front
// of the sun) and a weak back one.
float cloud_phase(float c, float k) {
    return lerp(hg(c, 0.8f * k), hg(c, -0.3f * k), 0.25f);
}

// The key light's radiance reaching altitude `alt` m (illuminance x
// transmittance through the air above, from the atmosphere's table).
float3 key_at(float alt) {
    const float r = kAtmGround + alt * 0.001f;
    const float mu = key_dir.y;   // the shell is near enough flat over the cloud's footprint
    float u, v;
    atm_trans_uv(r, mu, u, v);
    return key_lux_top * g_trans.SampleLevel(g_clamp, float2(u, v), 0).rgb * atm_planet_visibility(r, mu);
}

// Where a ray from o along d meets the sphere of radius R round the planet's
// centre: (near, far), negative where it does not.
float2 sphere(float3 o, float3 d, float R) {
    const float3 c = o + float3(0.0f, kR, 0.0f);
    const float b = dot(c, d);
    const float cc = dot(c, c) - R * R;
    const float disc = b * b - cc;
    if (disc < 0.0f) return float2(-1.0f, -1.0f);
    const float s = sqrt(disc);
    return float2(-b - s, -b + s);
}

float ign(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float3 sky_view(float3 d) {
    float u, v;
    atm_sky_uv(d, kAtmGround + cam_alt_km, u, v);
    return g_skyview.SampleLevel(g_wrap_u, float2(u, v), 0).rgb;
}

[numthreads(8, 8, 1)]
void cs_march(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(res.x) || id.y >= uint(res.y)) return;
    const float2 uv = (float2(id.xy) + 0.5f) * inv_res;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    // BIOMES_CLOUDS_DBG=1: the weather map itself, over the screen.
    if (pad0 > 0.5f && pad0 < 1.5f) {
        u_raw[id.xy] = float4(g_weather.SampleLevel(g_clamp, uv, 0).rgb * 20000.0f, 0.0f);
        u_raw_dist[id.xy] = 0.001f;
        return;
    }
    const float4 hp = mul(float4(ndc, 1.0f, 1.0f), inv_view_proj);
    float3 ro = cam_pos;
    float3 d = normalize(hp.xyz / hp.w - cam_pos);
    // BIOMES_CLOUDS_DBG=2: a top-down chart of the sky's real cover -- a ray
    // straight up from the ground under every pixel, over +-10 km, its
    // transmittance painted (bright clear, dark cloud). What share of the sky
    // the clouds actually cover, which the view from the ground cannot show:
    // straight up it looks between the clouds, at the horizon through them all.
    const bool chart = pad0 > 1.5f && pad0 < 2.5f;
    if (chart) {
        ro = float3((uv.x * 2.0f - 1.0f) * 10000.0f, 0.0f, (uv.y * 2.0f - 1.0f) * 10000.0f);
        d = float3(0.0f, 1.0f, 0.0f);
    }

    // The shell between the base and the tallest top.
    const float Rb = kR + base_alt, Rt = kR + base_alt + kMaxTop;
    const float cam_alt = altitude_of(ro);
    const float2 sb = sphere(ro, d, Rb), st = sphere(ro, d, Rt), sg = sphere(ro, d, kR);
    float t0, t1;
    if (cam_alt < base_alt) {
        if (sg.x > 0.0f && !chart) { u_raw[id.xy] = float4(0, 0, 0, 1); u_raw_dist[id.xy] = 0.0f; return; }   // the ground first
        t0 = sb.y; t1 = st.y;
    } else if (cam_alt < base_alt + kMaxTop) {
        t0 = 0.0f; t1 = sb.x > 0.0f ? sb.x : st.y;
    } else {
        if (st.x < 0.0f) { u_raw[id.xy] = float4(0, 0, 0, 1); u_raw_dist[id.xy] = 0.0f; return; }
        t0 = st.x; t1 = sb.x > 0.0f ? sb.x : st.y;
    }
    const float kFar = 80000.0f;
    t1 = min(t1, t0 + 40000.0f);
    if (t0 > kFar || t1 <= t0) { u_raw[id.xy] = float4(0, 0, 0, 1); u_raw_dist[id.xy] = 0.0f; return; }

    const int steps = int(lerp(64.0f, 128.0f, saturate((t1 - t0) / 20000.0f)));
    const float dt = (t1 - t0) / float(steps);
    const float j = frac(ign(float2(id.xy)) + frame * 0.61803399f);
    const float cphase = dot(d, key_dir);

    float3 L = 0.0f;
    float T = 1.0f, dsum = 0.0f, wsum = 0.0f;
    float t = t0 + dt * j;
    int empty = 0;
    [loop] for (int s = 0; s < steps; ++s) {
        const float3 p = ro + d * t;
        const float4 wx = weather_at(p);
        float dens = cloud_density(p, wx, false);
        if (dens > 0.0f) {
            dens = cloud_density(p, wx, true);
            if (dens > 0.0f) {
                // Optical depth to the key light: a cone of six taps,
                // growing, through the field without its detail.
                float tau = 0.0f;
                float ls = 60.0f;
                float3 lp = p;
                [unroll] for (int k = 0; k < 6; ++k) {
                    lp += key_dir * ls;
                    tau += cloud_density(lp, weather_at(lp), k < 2) * ls;
                    ls *= 1.8f;
                }
                tau *= sigma;
                const float alt = altitude_of(p);
                const float hf = saturate((alt - base_alt) / type_top(wx.g));
                // Multiple scattering (Hillaire 2016): octaves with less
                // extinction, less energy and a broader phase.
                float ms = 0.0f, a = 1.0f, bb = 1.0f, cc = 1.0f;
                [unroll] for (int o = 0; o < 4; ++o) {
                    ms += bb * exp(-tau * a) * cloud_phase(cphase, cc);
                    a *= 0.5f; bb *= 0.5f; cc *= 0.5f;
                }
                // Powder (Schneider): the inside of a billow facing the
                // light is darker than its edges, seen away from the light.
                const float powder = lerp(1.0f, 1.0f - exp(-dens * sigma * 120.0f), 0.5f - 0.5f * cphase);
                const float3 ambient = lerp(ground * 0.6f, sky_mean * 1.4f, hf);
                const float3 S = key_at(alt) * (ms * powder) + ambient;
                const float st_ = dens * sigma;
                const float e = exp(-st_ * dt);
                L += T * S * (1.0f - e);
                dsum += t * T * (1.0f - e);
                wsum += T * (1.0f - e);
                T *= e;
                if (T < 0.01f) break;
            }
            empty = 0;
        } else {
            // Through clear air, stride: at most two steps in one.
            ++empty;
            if (empty > 3) t += dt;
        }
        t += dt;
        if (t > t1) break;
    }
    if (chart || pad0 > 2.5f) {   // 3: the same for the camera's own rays
        u_raw[id.xy] = float4(T.xxx * 20000.0f, 0.0f);
        u_raw_dist[id.xy] = 0.001f;
        return;
    }
    const float dist = wsum > 1e-4f ? dsum / wsum : t0 + 0.5f * (t1 - t0);
    // Far clouds fade into the atmosphere's own horizon (the sky-view table
    // holds the air's in-scatter along the whole path).
    const float h = 1.0f - exp(-dist / (haze_km * 1000.0f));
    L = lerp(L, sky_view(d) * (1.0f - T), h);
    u_raw[id.xy] = float4(L, T);
    u_raw_dist[id.xy] = dist * 0.001f;   // km
}

// --- the cloud-shadow map (ADR-005 C2) -------------------------------------
//
// Transmittance of the key light through the cloud layer, top-down, indexed
// in the plane of the cloud BASE: a texel is the point where a ray towards
// the light leaves the bottom of the layer. A ground point P finds its texel
// by sliding along the light onto that plane, P.xz + l.xz (base - P.y) / l.y,
// so one map serves every height of terrain. Marched through the field
// without its detail (16 taps); a tenth of the light gets through any cloud,
// for the light scattered on through it.
[numthreads(8, 8, 1)]
void cs_shadow(uint3 id : SV_DispatchThreadID) {
    const uint n = uint(shadow_map.w);
    if (id.x >= n || id.y >= n) return;
    if (key_dir.y < 0.03f) { u_shadow[id.xy] = 1.0f; return; }
    const float2 xz = shadow_map.xy + ((float2(id.xy) + 0.5f) / float(n) * 2.0f - 1.0f) * shadow_map.z;
    const float3 start = float3(xz.x, base_alt, xz.y);
    const float len = min(kMaxTop / key_dir.y, 12000.0f);
    const float dt = len / 16.0f;
    float tau = 0.0f;
    [loop] for (int s = 0; s < 16; ++s) {
        const float3 p = start + key_dir * ((float(s) + 0.5f) * dt);
        tau += cloud_density(p, weather_at(p), false) * dt;
    }
    u_shadow[id.xy] = lerp(0.1f, 1.0f, exp(-tau * sigma));
}

// --- temporal ---------------------------------------------------------------

[numthreads(8, 8, 1)]
void cs_temporal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(res.x) || id.y >= uint(res.y)) return;
    const int2 p = int2(id.xy);
    const float4 cur = g_raw[p];
    const float dist = g_raw_dist[p];
    float4 outc = cur;
    if (history_valid > 0.5f) {
        const float2 uv = (float2(p) + 0.5f) * inv_res;
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float4 hp = mul(float4(ndc, 1.0f, 1.0f), inv_view_proj);
        const float3 d = normalize(hp.xyz / hp.w - cam_pos);
        const float3 wp = cam_pos + d * (dist > 0.0f ? dist * 1000.0f : 30000.0f);
        const float4 pc = mul(float4(wp, 1.0f), prev_view_proj);
        if (pc.w > 0.0f) {
            const float2 pn = pc.xy / pc.w;
            const float2 puv = float2(pn.x * 0.5f + 0.5f, 0.5f - pn.y * 0.5f);
            if (all(puv >= 0.0f) && all(puv <= 1.0f)) {
                const float4 hist = prev_index == 0u ? g_hist0.SampleLevel(g_clamp, puv, 0)
                                                     : g_hist1.SampleLevel(g_clamp, puv, 0);
                float4 lo = cur, hi = cur;
                [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x) {
                    const float4 s = g_raw[clamp(p + int2(x, y), int2(0, 0), int2(res) - 1)];
                    lo = min(lo, s);
                    hi = max(hi, s);
                }
                outc = lerp(clamp(hist, lo, hi), cur, 0.12f);
            }
        }
    }
    if (prev_index == 0u) { u_hist1[p] = outc; u_hdist1[p] = dist; }
    else                  { u_hist0[p] = outc; u_hdist0[p] = dist; }
    u_out[p] = outc;
    u_out_dist[p] = dist;
}
