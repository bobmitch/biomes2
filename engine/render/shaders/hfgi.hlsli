// ---------------------------------------------------------------------------
// Reading the heightfield GI (ADR-004 L6, the no-RT tier's GI; height_gi.h):
// at a point, the sky it sees past the terrain round it and the light the
// terrain bounces at it, both as L1 spherical harmonics gathered over a
// 512 m map of the ground seen from above -- off screen as much as on.
//
// Height matters because the map describes the TOP surface. A point well
// above it (a crown, a cliff top) sees more sky, so the answer fades to the
// open fallback over 40 m. A point well BELOW it is under rock or an
// overhang -- a cave, a dug chamber -- and gets neither sky nor this bounce.
//
// Included by shadow.hlsli, whose constants carry the map (l_hf); its
// textures are a sixth range of the lit pipelines' shadow table, heap
// [48..52] (t38..t42), sampled through the lit pipelines' linear sampler (s3, shadow.hlsli).
// ---------------------------------------------------------------------------
#ifndef ENGINE_HFGI_HLSLI
#define ENGINE_HFGI_HLSLI

#include "engine/render/shaders/hfgi_common.hlsli"
#include "engine/render/shaders/near_field_common.hlsli"

Texture2D<float4> g_hf_vis    : register(t38);   // sky visibility, SH
Texture2D<float4> g_hf_r      : register(t39);   // bounce radiance, SH, red
Texture2D<float4> g_hf_g      : register(t40);
Texture2D<float4> g_hf_b      : register(t41);
Texture2D<float2> g_hf_height : register(t42);   // the top surface's height, its curvature over 6 m

// The near field (near_field.h): probes and occupancy under the top surface,
// in the range DDGI's probes once held -- heap [40..41], t26..t27.
StructuredBuffer<float4> g_nf_probes : register(t26);
Texture3D<float>         g_nf_occ    : register(t27);
Texture2D<float>         g_nf_top    : register(t28);   // per 1 m column, the topmost rock's surface

// How far `p` lies under the REAL top surface of its 1 m column (negative
// under it), where the near field reaches. The heightfield's map holds the
// highest point of each 2 m texel and ordinary ground sits up to a metre and
// a half under it; this is the ground itself, so a tunnel reads metres down
// and a meadow about nothing.
bool nf_depth(float3 p, out float dh) {
    dh = 0.0f;
    if (l_nf.w < 0.5f) return false;
    const int2 c = int2(floor(p.xz + 0.5f));
    const int2 rel = c - int2(l_nf.xz);
    if (any(rel < 2) || any(rel >= kNfDims.xz - 2)) return false;
    const float top = g_nf_top.Load(int3(nf_wrap(int3(c.x, 0, c.y), kNfDims).xz, 0));
    if (top < -1.0e8f) return false;
    dh = p.y - top;
    return true;
}

// Whether the straight line from a to b crosses no rock: the occupancy's
// nearest 1 m point every half metre. Two loads, at a half and 0.85 of the
// way, stepped clean over a metre-thick roof wherever the rounding put both
// in air -- a 4 m tunnel under a thin roof took the open-air probes over it
// in bright bands a metre apart along every wall.
bool nf_clear(float3 a, float3 b) {
    const float3 v = b - a;
    const int n = int(ceil(length(v) * 2.0f));
    [loop] for (int i = 1; i <= n; ++i) {
        const float3 q = a + v * (float(i) / float(n + 1));
        if (g_nf_occ.Load(int4(nf_wrap(int3(floor(q + 0.5f)), kNfDims), 0)) > 0.5f) return false;
    }
    return true;
}

// The probes round a point, as the heightfield's SH. Each of the eight is
// weighted by trilinear position, by facing (a probe behind the surface
// sees the other side of it), and by whether rock lies between it and the
// point -- two point loads of the occupancy -- so a tunnel under a metre of
// roof does not borrow the sunlit probes over it. False where the volume
// does not reach; true with zeros where every probe is walled off.
bool nf_sample(float3 world, float3 n, out float4 vis, out float4 sr, out float4 sg, out float4 sb) {
    vis = sr = sg = sb = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (l_nf.w < 0.5f) return false;
    const float3 p = world + n * 0.4f;
    const float3 f = (p - 1.0f) * 0.5f;   // probe P stands at 2P + 1
    const int3 p0 = int3(floor(f));
    const float3 t = f - float3(p0);
    // A block of margin each side: the volume's edge is where it scrolls.
    const int3 lo = int3(l_nf.xyz) / 2 + 4, hi = int3(l_nf.xyz) / 2 + kNfProbeDims - 5;
    if (any(p0 < lo) || any(p0 + 1 > hi)) return false;
    float wsum = 0.0f;
    [loop] for (int c = 0; c < 8; ++c) {
        const int3 o = int3(c & 1, (c >> 1) & 1, c >> 2);
        const int3 q = p0 + o;
        const float3 tw = lerp(1.0f - t, t, float3(o));
        float w = tw.x * tw.y * tw.z;
        const uint i4 = nf_probe_index(q) * uint(kNfProbeFloat4s);
        const float4 meta = g_nf_probes[i4 + 4u];
        if (meta.x < 0.5f) continue;
        const float3 pc = float3(q * 2 + 1) + meta.yzw;
        const float3 v = pc - p;
        const float len = length(v);
        const float bf = 0.5f + 0.5f * dot(v / max(len, 1e-4f), n);
        w *= bf * bf + 0.05f;
        if (!nf_clear(p, pc)) continue;
        vis += w * g_nf_probes[i4 + 0u];
        sr  += w * g_nf_probes[i4 + 1u];
        sg  += w * g_nf_probes[i4 + 2u];
        sb  += w * g_nf_probes[i4 + 3u];
        wsum += w;
    }
    if (wsum > 1e-6f) {
        const float inv = 1.0f / wsum;
        vis *= inv; sr *= inv; sg *= inv; sb *= inv;
    }
    return true;
}

// How much of open air's ambient light reaches a point OF AIR under the top
// surface -- the probes' mean sky visibility over the sphere, doubled so an
// open hemisphere reads 1 -- and how far to trust it over the caller's own
// rule for open air (`k`: 0 above the top surface, 1 from 1.5 m under it).
// -1 (k 1): the point is in rock.
// Open air keeps the caller's rule: lighting it by the heightfield's sky
// visibility instead moved 63% of an open meadow's pixels, by up to 47
// levels, against haze and height fog tuned to the old one.
float nf_openness(float3 p, out float k) {
    k = 0.0f;
    if (l_hf.w < 0.5f || l_nf.w < 0.5f) return 1.0f;
    const float2 uv = float2((p.x - l_hf.x) / kHfExtent, 1.0f - (p.z - l_hf.y) / kHfExtent);
    if (any(uv <= 0.0f) || any(uv >= 1.0f)) return 1.0f;
    float dh;
    if (!nf_depth(p, dh) || dh > -0.5f) return 1.0f;
    // Most of what lies under the top surface is rock -- every froxel behind
    // the terrain in view -- and holds no air at all: -1, one load, not the
    // probes. The caller gives it no mist and leaves it out of the openness
    // averaged along the ray. Neither "open" nor "dark" is right: open (1)
    // lit a tunnel's fog wherever a ray's last slice sampled inside the wall
    // -- bright bands a metre apart down a 4 m tunnel -- and dark (0) would
    // thin the haze over every hillside a ray's last slice cuts into.
    const int3 vox = int3(floor(p + 0.5f));
    const int3 rel = vox - int3(l_nf.xyz);
    if (all(rel >= 0) && all(rel < kNfDims) && g_nf_occ.Load(int4(nf_wrap(vox, kNfDims), 0)) > 0.5f) {
        k = 1.0f;
        return -1.0f;
    }
    float4 v, r, g, b;
    if (!nf_sample(p, float3(0.0f, 0.0f, 0.0f), v, r, g, b)) return 1.0f;
    k = smoothstep(-0.5f, -1.5f, dh);
    return saturate(2.0f * v.x / (kHfY00 * 4.0f * 3.14159265f));
}

// How far the near field's probes answer at a surface point: 1 under real
// cover, 0 in the open, the same rule hfgi_sample blends them by (less the
// volume's margin, which only matters at its edge). Where it is 1 the
// probes carry a local light's bounce, and the light's own fill
// (local_lights.hlsli) steps aside.
float nf_cover(float3 world) {
    if (l_hf.w < 0.5f || l_nf.w < 0.5f) return 0.0f;
    float dnf;
    if (!nf_depth(world, dnf)) return 0.0f;
    float k = smoothstep(-0.5f, -1.5f, dnf);
    const float2 uv = float2((world.x - l_hf.x) / kHfExtent, 1.0f - (world.z - l_hf.y) / kHfExtent);
    if (all(uv > 0.0f) && all(uv < 1.0f)) {
        const float h = g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f).x;
        if (h > kHfNone * 0.5f && world.y - h < -1.5f) k = 1.0f;
    }
    return k;
}

// `vis_e`: the sky's share as an irradiance factor -- pi for a point that
// sees a whole open hemisphere about n, as kSkyMean * vis_e is the sky's
// irradiance. `bounce`: the terrain's bounce irradiance, lux. `cover` how
// much to trust it (0 off the map), for the caller to blend its fallback.
void hfgi_sample(float3 world, float3 n, out float vis_e, out float3 bounce, out float cover) {
    vis_e = 0.0f;
    bounce = float3(0.0f, 0.0f, 0.0f);
    cover = 0.0f;
    if (l_hf.w < 0.5f) return;
    const float2 uv = float2((world.x - l_hf.x) / kHfExtent, 1.0f - (world.z - l_hf.y) / kHfExtent);
    const float2 e = min(uv, 1.0f - uv);
    if (min(e.x, e.y) <= 0.0f) return;
    const float h = g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f).x;
    if (h < kHfNone * 0.5f) return;
    cover = saturate(min(e.x, e.y) * 16.0f);
    float2 suv = uv;
    float k = 1.0f;
    const float dh = world.y - h;
    // Under the top surface the near field answers where it reaches:
    // blended in from half a metre to a metre and a half down, and alone
    // below that, in place of the search for a way out below.
    // Under the REAL top surface (nf_depth), faded in from half a metre to a
    // metre and a half down; and wherever the map's own covered branch would
    // run (more than 1.5 m under its 2 m map: a narrow pit, a steep slope),
    // the probes instead of its search. Deciding cover by the 2 m map ran
    // the probe path on ordinary bumpy ground: +0.36 ms of terrain at 1440p.
    float dnf;
    float knf = 0.0f;
    if (nf_depth(world, dnf)) knf = max(smoothstep(-0.5f, -1.5f, dnf), dh < -1.5f ? 1.0f : 0.0f);
    float4 nv = 0.0f, nr = 0.0f, ng = 0.0f, nb = 0.0f;
    // An if, not `knf > 0 && nf_sample(...)`: HLSL's && does NOT short-circuit,
    // and that one line ran the eight probes on every terrain pixel and threw
    // the answer away -- +0.33 ms at 1440p with no pixel using it.
    bool have_nf = false;
    if (knf > 0.0f) have_nf = nf_sample(world, n, nv, nr, ng, nb);
    if (have_nf && knf >= 1.0f) {
        vis_e = hf_irradiance(nv, n);
        bounce = float3(hf_irradiance(nr, n), hf_irradiance(ng, n), hf_irradiance(nb, n));
        return;
    }
    if (dh < -1.5f) {
        // Under the top surface. An overhang is open to the side and a cave
        // is not: look for the nearest column whose surface lies below this
        // point -- a way out to the sky -- and take ITS light, fading with
        // the distance in. None within 12 m: enclosed, and no sky or bounce
        // reaches it. "Under the surface means covered" alone drew every
        // overhang's underside black.
        float best = 1e9f;
        [loop] for (int r = 1; r <= 3; ++r) {
            const float rad = 2.0f * float(1 << r);   // 4, 8, 16 m
            [loop] for (int a = 0; a < 8; ++a) {
                const float ang = 0.785398f * float(a) + 0.39f * float(r);
                const float2 q = world.xz + float2(cos(ang), sin(ang)) * rad;
                const float2 quv = float2((q.x - l_hf.x) / kHfExtent, 1.0f - (q.y - l_hf.y) / kHfExtent);
                if (any(quv < 0.0f) || any(quv > 1.0f)) continue;
                const float hq = g_hf_height.SampleLevel(g_gi_linear, quv, 0.0f).x;
                if (hq > kHfNone * 0.5f && hq < world.y - 0.5f && rad < best) {
                    best = rad;
                    suv = quv;
                }
            }
            if (best < 1e8f) break;
        }
        k = saturate(1.0f - best / 12.0f) * (best < 1e8f ? 1.0f : 0.0f);
        // The search is for an overhang's underside, open to the side. More
        // than 4 m under the top surface it is a tunnel or a chamber, and a
        // column whose surface lies below the point -- any slope nearby --
        // lit a sealed tunnel 12 m down to EV100 5 where the near field does
        // not reach (or is not ready yet). Enclosed: nothing.
        if (dh < -4.0f) k = 0.0f;
    }
    vis_e = hf_irradiance(g_hf_vis.SampleLevel(g_gi_linear, suv, 0.0f), n) * k;
    bounce = float3(hf_irradiance(g_hf_r.SampleLevel(g_gi_linear, suv, 0.0f), n),
                    hf_irradiance(g_hf_g.SampleLevel(g_gi_linear, suv, 0.0f), n),
                    hf_irradiance(g_hf_b.SampleLevel(g_gi_linear, suv, 0.0f), n)) * k;
    if (dh >= -1.5f) {
        // Above it: towards an open hemisphere with height.
        const float f = saturate(dh / 40.0f);
        vis_e = lerp(vis_e, 3.14159265f * (0.5f + 0.5f * n.y), f);
    }
    if (have_nf) {
        vis_e = lerp(vis_e, hf_irradiance(nv, n), knf);
        bounce = lerp(bounce, float3(hf_irradiance(nr, n), hf_irradiance(ng, n), hf_irradiance(nb, n)), knf);
    }
}

#endif
