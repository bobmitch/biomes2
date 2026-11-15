// ---------------------------------------------------------------------------
// Froxel clusters for local lights (ADR-004 L3).
//
// The view frustum is cut into 16 x 9 screen tiles and 24 depth slices,
// log-spaced from 0.5 to 400 m, and each cluster gets the list of lights
// whose range reaches it. One thread a cluster, every light tested against
// the cluster's view-space box: 3,456 clusters x 256 lights at most, which
// is a few microseconds. A pixel finds its cluster from its screen position
// and view depth (local_lights.hlsli) and shades only those lights.
// ---------------------------------------------------------------------------

cbuffer ClusterConstants : register(b0) {
    float3 cam_pos;   uint  light_count;
    float3 cam_right; float tan_half_fov;
    float3 cam_up;    float aspect;
    float3 cam_fwd;   float near_z;
    float  log_scale; float far_z; uint pad0; uint pad1;
};

struct LocalLightGpu {
    float3 pos;      float range;
    float3 intensity; float type;
    float3 spot_dir; float cos_outer;
    float  cos_inner; int shadow;
    float2 pad;
};

StructuredBuffer<LocalLightGpu> g_lights : register(t0);
RWStructuredBuffer<uint> u_count : register(u0);   // lights per cluster
RWStructuredBuffer<uint> u_items : register(u1);   // kMaxItems per cluster
RWStructuredBuffer<uint> u_stats : register(u2);   // overflowed, max wanted, clusters lit, items

static const uint kX = 16, kY = 9, kZ = 24, kMaxItems = 128;   // local_lights.h

[numthreads(4, 1, 1)]
void cs_cluster_clear(uint3 id : SV_DispatchThreadID) {
    u_stats[id.x] = 0u;
}

[numthreads(64, 1, 1)]
void cs_cluster(uint3 id : SV_DispatchThreadID) {
    const uint c = id.x;
    if (c >= kX * kY * kZ) return;
    const uint tx = c % kX, ty = (c / kX) % kY, tz = c / (kX * kY);
    const float z0 = near_z * exp(float(tz) / log_scale);
    const float z1 = near_z * exp(float(tz + 1) / log_scale);
    // The tile in NDC: row 0 is the top of the screen, where NDC y is +1.
    const float x0 = -1.0f + 2.0f * float(tx) / float(kX), x1 = x0 + 2.0f / float(kX);
    const float y1 = 1.0f - 2.0f * float(ty) / float(kY), y0 = y1 - 2.0f / float(kY);
    const float sx = tan_half_fov * aspect, sy = tan_half_fov;
    float3 lo = float3(1e30f, 1e30f, z0), hi = float3(-1e30f, -1e30f, z1);
    [unroll] for (int k = 0; k < 2; ++k) {
        const float z = k == 0 ? z0 : z1;
        lo.x = min(lo.x, min(x0 * sx * z, x1 * sx * z));
        hi.x = max(hi.x, max(x0 * sx * z, x1 * sx * z));
        lo.y = min(lo.y, min(y0 * sy * z, y1 * sy * z));
        hi.y = max(hi.y, max(y0 * sy * z, y1 * sy * z));
    }
    uint n = 0;
    for (uint i = 0; i < light_count; ++i) {
        const LocalLightGpu L = g_lights[i];
        const float3 d = L.pos - cam_pos;
        const float3 v = float3(dot(d, cam_right), dot(d, cam_up), dot(d, cam_fwd));
        const float3 q = clamp(v, lo, hi) - v;
        if (dot(q, q) > L.range * L.range) continue;
        if (n < kMaxItems) u_items[c * kMaxItems + n] = i;
        ++n;
    }
    u_count[c] = min(n, kMaxItems);
    if (n > kMaxItems) InterlockedAdd(u_stats[0], n - kMaxItems);
    InterlockedMax(u_stats[1], n);
    if (n > 0u) InterlockedAdd(u_stats[2], 1u);
    InterlockedAdd(u_stats[3], min(n, kMaxItems));
}
