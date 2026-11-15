// ---------------------------------------------------------------------------
// Ray-traced key-light shadow, and the instruments for the RT scene
// (ADR-004 L5). SM 6.5, DXC.
//
// Per pixel with depth:
//   - one shadow ray towards the key light through gi_occluded: the visibility
//     the composite uses in place of the cascades under --rt-shadows;
//   - where both apply, the cascades' answer at the same point, and which of
//     four ways the two agree -- lit/lit, shadow/shadow, or one only;
//   - a primary ray, terrain only, through gi_trace, against the depth
//     buffer's distance: whether the TLAS is the terrain that was drawn
//     (agree / RT nearer / RT farther or missed), and the angle between
//     gi_trace's normal and the depth buffer's where they agree.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/gi_trace.hlsli"

cbuffer RtConstants : register(b0) {
    row_major float4x4 inv_view_proj;
    float3 cam_pos;  uint width;
    float3 key_dir;  uint height;
    float  near_plane; float far_plane; float csm_reach; uint frame;
    uint   checks;     float light_radius; float2 pad1;   // checks: the instruments; light_radius: the disk, radians
};

Texture2D<float>          g_depth : register(t0);
RWTexture2D<unorm float>  u_vis   : register(u0);
// [0] both lit [1] both shadowed [2] RT shadowed only [3] cascades shadowed only
// [4] primary agree [5] RT nearer [6] RT farther or missed [7] normal error, centi-degrees, summed
// over [8] near, non-grazing agreeing pixels
RWStructuredBuffer<uint>  u_stats : register(u1);

float3 world_at(float2 uv, float z) {
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(float4(ndc, z, 1.0f), inv_view_proj);
    return h.xyz / h.w;
}

float3 normal_from_depth(int2 p, float3 P) {
    const int2 mx = int2(int(width) - 1, int(height) - 1);
    const float2 inv = 1.0f / float2(width, height);
    const float2 uv = (float2(p) + 0.5f) * inv;
    const float3 L = world_at(uv - float2(inv.x, 0.0f), g_depth[clamp(p - int2(1, 0), 0, mx)]);
    const float3 R = world_at(uv + float2(inv.x, 0.0f), g_depth[clamp(p + int2(1, 0), 0, mx)]);
    const float3 U = world_at(uv - float2(0.0f, inv.y), g_depth[clamp(p - int2(0, 1), 0, mx)]);
    const float3 D = world_at(uv + float2(0.0f, inv.y), g_depth[clamp(p + int2(0, 1), 0, mx)]);
    const float3 dx = length(L - P) < length(R - P) ? P - L : R - P;
    const float3 dy = length(U - P) < length(D - P) ? P - U : D - P;
    float3 n = normalize(cross(dy, dx));
    if (dot(n, cam_pos - P) < 0.0f) n = -n;
    return n;
}

[numthreads(8, 8, 1)]
void cs_rt_shadow(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const float z = g_depth[p];
    if (z >= 1.0f) { u_vis[p] = 1.0f; return; }
    const float2 uv = (float2(p) + 0.5f) / float2(width, height);
    const float3 P = world_at(uv, z);
    const float3 n = normal_from_depth(p, P);
    const float d = length(P - cam_pos);

    // First the terrain along the pixel's own ray: whether the TLAS holds the
    // terrain that was drawn, and -- where it does -- the triangle the pixel
    // is on, for a terminator-safe shadow origin.
    GiRay pr;
    pr.origin = cam_pos;
    pr.tmin = 0.05f;
    pr.dir = (P - cam_pos) / d;
    pr.tmax = 4000.0f;
    float th;
    float3 lifted, ns;
    const bool thit = gi_terrain_point(pr, th, lifted, ns);
    const float tol = max(0.1f, 0.01f * d);
    const bool on_terrain = thit && abs(th - d) <= tol;
    // The agreement with the depth buffer is counted here because the
    // terrain lookup is needed anyway; the rest of the instruments wait for
    // `checks`.

    // The shadow ray. On terrain: from the lifted point, a hair above it,
    // judged by the shading normal the surface is lit with. Elsewhere --
    // trees, grass, the sea -- off the depth buffer's surface by a little.
    // Towards a point on the light's disk, a different one per pixel and
    // frame: TAA integrates them into the true penumbra of a sun 0.53
    // degrees across, where aiming every ray at the centre drew edges
    // sharper than any real shadow.
    float3 t1 = normalize(abs(key_dir.y) < 0.99f ? cross(key_dir, float3(0.0f, 1.0f, 0.0f)) : cross(key_dir, float3(1.0f, 0.0f, 0.0f)));
    const float3 t2 = cross(key_dir, t1);
    const float u1 = frac(52.9829189f * frac(dot(float2(p) + float(frame % 64u) * 5.588238f, float2(0.06711056f, 0.00583715f))));
    const float u2 = frac(u1 * 7.23f + float(frame % 16u) * 0.618034f);
    const float rr = light_radius * sqrt(u1), ang = 6.2831853f * u2;
    GiRay sr;
    sr.dir = normalize(key_dir + (t1 * cos(ang) + t2 * sin(ang)) * rr);
    sr.tmax = 3000.0f;
    float3 nl;
    if (on_terrain) {
        sr.origin = lifted;
        sr.tmin = 0.01f + 0.0002f * d;
        nl = ns;
    } else {
        sr.origin = P + n * (0.02f + 0.0005f * d) + key_dir * 0.02f;
        sr.tmin = 0.0f;
        nl = n;
    }
    const float vis = dot(nl, key_dir) > -0.05f ? gi_occluded(sr) : 0.0f;
    u_vis[p] = vis;

    if (checks == 0u) return;
    if (dot(n, key_dir) > 0.1f && d < csm_reach) {
        const bool rt = vis > 0.5f;
        const bool csm = cascade_visibility(P, n) > 0.5f;
        const uint k = rt ? (csm ? 0u : 3u) : (csm ? 2u : 1u);
        InterlockedAdd(u_stats[k], 1u);
    }

    // gi_trace itself, on the same ray: its normal against the depth
    // buffer's, near, where that can be trusted.
    const GiHit h = gi_trace_masked(pr, kRtMaskTerrain);
    if (!thit || th > d + tol) {
        InterlockedAdd(u_stats[6], 1u);
    } else if (th < d - tol) {
        InterlockedAdd(u_stats[5], 1u);
    } else {
        InterlockedAdd(u_stats[4], 1u);
        // The normal check only where the depth buffer's own normal can be
        // trusted: near, and not at a grazing angle. Beyond that it measures
        // depth precision, not gi_trace.
        if (d < 40.0f && dot(n, -pr.dir) > 0.3f) {
            const float c = clamp(dot(h.normal, n), -1.0f, 1.0f);
            InterlockedAdd(u_stats[7], uint(acos(c) * 5729.58f));
            InterlockedAdd(u_stats[8], 1u);
        }
    }
}

[numthreads(8, 1, 1)]
void cs_rt_clear(uint3 id : SV_DispatchThreadID) {
    u_stats[id.x] = 0u;
}
