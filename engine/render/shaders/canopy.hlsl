// ---------------------------------------------------------------------------
// The canopy map's build (ADR-004 L6; canopy.h): clear, splat every crown's
// vertical chord into fixed-point accumulators, resolve to slice 0, then
// blur slice k from slice k-1 at a stride of 2^(k-1) texels.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/canopy_common.hlsli"

cbuffer CanopyBuild : register(b0) {
    float2 origin_xz;  float y_ref;  uint tree_count;
    uint   groups_x;   float3 pad0;
};
cbuffer CanopySlice : register(b1) { uint slice; };

// Per tree, world metres: (centre x, centre y, centre z, horizontal radius),
// (vertical radius, extinction, 0, 0).
struct CanopyTree { float4 a; float4 b; };
StructuredBuffer<CanopyTree> g_trees : register(t0);
RWStructuredBuffer<uint>     u_accum : register(u0);   // per texel: tau, tau x (bottom + off), tau x (top + off)
RWTexture2DArray<float4>     u_map   : register(u1);
Texture2DArray<float4>       g_map   : register(t1);

static const float kTauScale = 1024.0f;   // fixed point: tau
static const float kHScale = 16.0f;       // fixed point: tau x metres
static const float kHOff = 1024.0f;       // heights relative to y_ref, kept positive

[numthreads(64, 1, 1)]
void cs_canopy_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < kCanopySize * kCanopySize * 3u) u_accum[id.x] = 0u;
}

// One group a tree; its 64 threads walk the crown's footprint.
[numthreads(64, 1, 1)]
void cs_canopy_splat(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const uint t = gid.x + gid.y * groups_x;
    if (t >= tree_count) return;
    const CanopyTree tr = g_trees[t];
    const float cx = tr.a.x - origin_xz.x, cz = tr.a.z - origin_xz.y;
    const float rh = tr.a.w, rv = tr.b.x, sigma = tr.b.y;
    const int x0 = max(0, int(floor(cx - rh))), x1 = min(int(kCanopySize) - 1, int(floor(cx + rh)));
    const int z0 = max(0, int(floor(cz - rh))), z1 = min(int(kCanopySize) - 1, int(floor(cz + rh)));
    if (x1 < x0 || z1 < z0) return;
    const uint w = uint(x1 - x0 + 1), n = w * uint(z1 - z0 + 1);
    const float inv_r2 = 1.0f / (rh * rh);
    for (uint k = gi; k < n; k += 64u) {
        const int tx = x0 + int(k % w), tz = z0 + int(k / w);
        const float dx = float(tx) + 0.5f - cx, dz = float(tz) + 0.5f - cz;
        const float d2 = (dx * dx + dz * dz) * inv_r2;
        if (d2 >= 1.0f) continue;
        const float half_chord = rv * sqrt(1.0f - d2);
        const float tau = sigma * 2.0f * half_chord;
        const float yc = tr.a.y - y_ref;
        const float b = clamp(yc - half_chord + kHOff, 0.0f, 2.0f * kHOff);
        const float tp = clamp(yc + half_chord + kHOff, 0.0f, 2.0f * kHOff);
        const uint i = (uint(tz) * kCanopySize + uint(tx)) * 3u;
        InterlockedAdd(u_accum[i + 0u], uint(tau * kTauScale + 0.5f));
        InterlockedAdd(u_accum[i + 1u], uint(tau * b * kHScale + 0.5f));
        InterlockedAdd(u_accum[i + 2u], uint(tau * tp * kHScale + 0.5f));
    }
}

// Slice 0: tau, tau x bottom, tau x top (heights relative to y_ref), and the
// vertical transmittance a sky ray straight up would see.
[numthreads(8, 8, 1)]
void cs_canopy_resolve(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kCanopySize || id.y >= kCanopySize) return;
    const uint i = (id.y * kCanopySize + id.x) * 3u;
    const float tau = float(u_accum[i]) / kTauScale;
    float4 o = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (tau > 1e-4f) {
        o.x = tau;
        o.y = float(u_accum[i + 1u]) / kHScale - kHOff * tau;
        o.z = float(u_accum[i + 2u]) / kHScale - kHOff * tau;
        o.w = exp(-kCanopyPath * tau);
    }
    u_map[uint3(id.xy, 0u)] = o;
}

// Slice k from slice k-1: a 3x3 binomial at a stride of 2^(k-1) texels. The
// strides double, so five passes reach a footprint of tens of metres for
// nine taps each.
[numthreads(8, 8, 1)]
void cs_canopy_blur(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kCanopySize || id.y >= kCanopySize) return;
    const int s = 1 << (slice - 1u);
    const int m = int(kCanopySize) - 1;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float w = (i == 0 ? 2.0f : 1.0f) * (j == 0 ? 2.0f : 1.0f);
            // Outside the map is open sky: tau 0, transmittance 1.
            const int2 p = int2(id.xy) + int2(i, j) * s;
            const float4 v = (p.x < 0 || p.y < 0 || p.x > m || p.y > m)
                           ? float4(0.0f, 0.0f, 0.0f, 1.0f) : g_map.Load(int4(p, int(slice) - 1, 0));
            acc += v * w;
        }
    }
    u_map[uint3(id.xy, slice)] = acc / 16.0f;
}
