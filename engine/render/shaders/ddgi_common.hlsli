// ---------------------------------------------------------------------------
// DDGI's layout (ADR-004 L6), shared by the probe update (ddgi_update.hlsl)
// and the readers (ddgi.hlsli). ddgi.h has the C++ side; keep them equal.
//
// Three camera-centred cascades of 24 x 12 x 24 probes, 2, 8 and 32 m
// apart. A probe is addressed by its WORLD grid coordinate g; it is stored at
// g mod dims, so the volume scrolls with the camera and only the slab that
// entered is reset. Each probe stores irradiance in an 8x8 octahedral tile
// and the mean and mean square of its ray distances in a 16x16 one, each
// with a one-texel gutter for bilinear filtering across the fold.
// Atlas: tile column x + 24 y, row z, one array slice per cascade.
// ---------------------------------------------------------------------------
#ifndef ENGINE_DDGI_COMMON_HLSLI
#define ENGINE_DDGI_COMMON_HLSLI

static const int  kDdgiCascades = 3;
static const int3 kDdgiDims = int3(24, 12, 24);
static const int  kDdgiProbes = 24 * 12 * 24;        // per cascade
static const int  kDdgiIrr = 8, kDdgiVis = 16;       // tile interiors
static const uint kDdgiRays = 128;                   // per probe, per update
static const uint kDdgiRaysInactive = 32;            // a probe in rock keeps looking for a way out
static const uint kDdgiPhases = 4;                   // a quarter of the probes a frame
static const float kDdgiMiss = 1e27f;                // ray distance for a miss
// A ray an inactive probe did not trace. Readers skip distances at or past
// half of it, so it must sit far above a miss: at 2e27, half was exactly the
// miss, every sky ray was skipped as untraced, and the probes saw only the
// ground -- the olive cast, a twentieth of the sky's blue.
static const float kDdgiUnused = 1e30f;

struct DdgiProbe {
    float4 offset;   // xyz relocation in metres, w state: 0 active, 1 off in rock, 2 idle (nothing near)
    int4   coord;    // the world grid coordinate this slot holds, w updates since reset
};

float ddgi_sign_nz(float x) { return x >= 0.0f ? 1.0f : -1.0f; }

// Octahedral about +y: the upper hemisphere is the inner diamond.
float2 ddgi_oct_encode(float3 d) {
    d /= (abs(d.x) + abs(d.y) + abs(d.z));
    float2 e = d.xz;
    if (d.y < 0.0f) e = (1.0f - abs(e.yx)) * float2(ddgi_sign_nz(e.x), ddgi_sign_nz(e.y));
    return e;
}

float3 ddgi_oct_decode(float2 e) {
    float3 d = float3(e.x, 1.0f - abs(e.x) - abs(e.y), e.y);
    if (d.y < 0.0f) {
        const float2 xz = (1.0f - abs(d.zx)) * float2(ddgi_sign_nz(d.x), ddgi_sign_nz(d.z));
        d.x = xz.x;
        d.z = xz.y;
    }
    return normalize(d);
}

int3 ddgi_wrap(int3 g) {
    return ((g % kDdgiDims) + kDdgiDims) % kDdgiDims;
}

int ddgi_slot(int3 s) {
    return s.x + kDdgiDims.x * (s.y + kDdgiDims.y * s.z);
}

int3 ddgi_slot_coord(int i) {
    return int3(i % kDdgiDims.x, (i / kDdgiDims.x) % kDdgiDims.y, i / (kDdgiDims.x * kDdgiDims.y));
}

// The texel at the interior corner of a probe's tile.
int2 ddgi_tile_origin(int3 s, int interior) {
    return int2((s.x + kDdgiDims.x * s.y) * (interior + 2) + 1, s.z * (interior + 2) + 1);
}

float2 ddgi_atlas_size(int interior) {
    return float2(kDdgiDims.x * kDdgiDims.y * (interior + 2), kDdgiDims.z * (interior + 2));
}

// Normalised atlas uv for a direction out of a probe's tile.
float2 ddgi_uv(int3 s, float3 dir, int interior) {
    const float2 e = ddgi_oct_encode(dir) * 0.5f + 0.5f;
    return (float2(ddgi_tile_origin(s, interior)) + e * float(interior)) / ddgi_atlas_size(interior);
}

#endif
