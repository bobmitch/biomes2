// ---------------------------------------------------------------------------
// The sea's ripples: two 256^2 FFT tiles (engine/render/ocean_spectrum.h),
// synthesised every frame.
//
//   cs_spectrum  h(k, t) from a fixed random field and the weather's
//                spectrum, then the three complex fields the shading needs,
//                two real fields packed in each (real + i imag):
//                  F0 = slope x + i slope z
//                  F1 = dDx/dx + i dDz/dz     (the choppy displacement's
//                  F2 = dDx/dz + i height      derivatives, for the Jacobian)
//   cs_rows      inverse FFT along x, all three fields, in group memory
//   cs_cols      inverse FFT along z, then the output texel:
//                  (slope x, slope z, J - 1, persistent foam)
//
// Each wavevector's frequency is fixed (deep water, w = sqrt(g k)), and the
// weather only reweights amplitudes, so a veering wind never slides a phase
// (water_wave.h says why that matters). The random field is a hash of the
// texel, so nothing is stored and a seed picks the sea.
// ---------------------------------------------------------------------------

#include "engine/render/ocean_spectrum.h"

cbuffer FftConstants : register(b0) {
    float  time;      float choppy;    float amp;       float foam_decay;
    float2 wind_dir;  float wind_u;    uint  seed;
};

RWStructuredBuffer<float2> g_a   : register(u0);   // spectra, then the columns' input
RWStructuredBuffer<float2> g_b   : register(u1);   // after the row pass
RWTexture2DArray<float4>   g_out : register(u2);   // per cascade: slope x, slope z, J - 1, foam

uint at(uint c, uint f, uint x, uint y) { return ((c * 3u + f) * OD_N + y) * OD_N + x; }

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

// A standard complex Gaussian (E|xi|^2 = 2) per texel, Box-Muller.
float2 gauss(uint c, uint x, uint y) {
    const uint h1 = pcg(seed ^ pcg(c * 0x9E3779B9u ^ pcg(x * 73856093u ^ y * 19349663u)));
    const uint h2 = pcg(h1 ^ 0x68E31DA4u);
    const float u1 = max(float(h1 >> 8) / 16777216.0f, 1e-7f);
    const float u2 = float(h2 >> 8) / 16777216.0f;
    const float r = sqrt(-2.0f * log(u1));
    return float2(r * cos(6.2831853f * u2), r * sin(6.2831853f * u2));
}

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 conj2(float2 a) { return float2(a.x, -a.y); }

[numthreads(8, 8, 1)]
void cs_spectrum(uint3 id : SV_DispatchThreadID) {
    const uint x = id.x, y = id.y, c = id.z;
    if (x >= OD_N || y >= OD_N) return;
    const float kx = od_kx(int(c), int(x));
    const float kz = od_kx(int(c), int(y));
    const uint xm = (OD_N - x) & (OD_N - 1u), ym = (OD_N - y) & (OD_N - 1u);
    // h0(k) and h0(-k), so h(k, t) is Hermitian and every field comes out real.
    const float2 h0 = gauss(c, x, y) * (amp * sqrt(0.5f * od_phillips(int(c), kx, kz, wind_u, wind_dir.x, wind_dir.y)));
    const float2 h0m = gauss(c, xm, ym) * (amp * sqrt(0.5f * od_phillips(int(c), -kx, -kz, wind_u, wind_dir.x, wind_dir.y)));
    const float k = sqrt(kx * kx + kz * kz);
    const float w = sqrt(9.81f * k);
    float sw, cw;
    sincos(w * time, sw, cw);
    const float2 e = float2(cw, sw);
    const float2 h = cmul(h0, e) + cmul(conj2(h0m), conj2(e));
    float2 f0 = 0.0f, f1 = 0.0f, f2 = 0.0f;
    if (k > 1e-6f) {
        // i kx h + i (i kz h)
        f0 = float2(-kx * h.y, kx * h.x) + float2(-kz * h.x, -kz * h.y);
        // (kx^2 / k) h + i (kz^2 / k) h
        const float ax = kx * kx / k, az = kz * kz / k;
        f1 = float2(ax * h.x - az * h.y, ax * h.y + az * h.x);
        // (kx kz / k) h + i h
        const float axz = kx * kz / k;
        f2 = float2(axz * h.x - h.y, axz * h.y + h.x);
    }
    g_a[at(c, 0u, x, y)] = f0;
    g_a[at(c, 1u, x, y)] = f1;
    g_a[at(c, 2u, x, y)] = f2;
}

// --- the inverse FFT: 256 points, radix 2, in group memory --------------------

groupshared float2 s_f[3][OD_N];

uint bitrev8(uint v) { return reversebits(v) >> 24u; }

// Called by all 128 threads of a group with s_f loaded in bit-reversed order.
void ifft_shared(uint t) {
    [unroll] for (uint span = 1u; span < OD_N; span <<= 1u) {
        GroupMemoryBarrierWithGroupSync();
        const uint j = t & (span - 1u);
        const uint i0 = ((t - j) << 1u) + j;
        const uint i1 = i0 + span;
        float sw, cw;
        sincos(3.14159265f * float(j) / float(span), sw, cw);   // e^{+i pi j / span}: inverse
        const float2 w = float2(cw, sw);
        [unroll] for (uint f = 0u; f < 3u; ++f) {
            const float2 a = s_f[f][i0];
            const float2 b = cmul(s_f[f][i1], w);
            s_f[f][i0] = a + b;
            s_f[f][i1] = a - b;
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

// One group per row (y) of one cascade: SV_GroupID.x = y, .y = cascade.
[numthreads(128, 1, 1)]
void cs_rows(uint3 g : SV_GroupID, uint t : SV_GroupThreadID) {
    const uint y = g.x, c = g.y;
    [unroll] for (uint f = 0u; f < 3u; ++f) {
        s_f[f][bitrev8(t)] = g_a[at(c, f, t, y)];
        s_f[f][bitrev8(t + 128u)] = g_a[at(c, f, t + 128u, y)];
    }
    ifft_shared(t);
    [unroll] for (uint f2 = 0u; f2 < 3u; ++f2) {
        g_b[at(c, f2, t, y)] = s_f[f2][t];
        g_b[at(c, f2, t + 128u, y)] = s_f[f2][t + 128u];
    }
}

// The output texel from the three fields' spatial values. The displacement
// is x' = x - choppy D (the sign that sharpens crests, as the Gerstner
// trains do), so J = (1 - c Dxx)(1 - c Dzz) - c^2 Dxz^2. Foam is kept where
// the surface compresses and decays over a couple of seconds, IN THE TILE:
// the compression moves on with the wave and leaves its foam behind it as a
// streak, which is the look of real whitecaps.
float4 finish(uint c, uint x, uint y, float2 f0, float2 f1, float2 f2) {
    const float dxx = f1.x, dzz = f1.y, dxz = f2.x;
    const float jm1 = (1.0f - choppy * dxx) * (1.0f - choppy * dzz) - choppy * choppy * dxz * dxz - 1.0f;
    const float prev = g_out[uint3(x, y, c)].w;
    // Laid down only where the ripples compress hard (J under 0.85): at any
    // compression at all, it built up over most of the tile and a gale went
    // white all over.
    const float foam = max(prev * foam_decay, saturate((-jm1 - 0.15f) * 3.0f));
    return float4(f0.x, f0.y, jm1, foam);
}

// One group per column (x) of one cascade.
[numthreads(128, 1, 1)]
void cs_cols(uint3 g : SV_GroupID, uint t : SV_GroupThreadID) {
    const uint x = g.x, c = g.y;
    [unroll] for (uint f = 0u; f < 3u; ++f) {
        s_f[f][bitrev8(t)] = g_b[at(c, f, x, t)];
        s_f[f][bitrev8(t + 128u)] = g_b[at(c, f, x, t + 128u)];
    }
    ifft_shared(t);
    g_out[uint3(x, t, c)] = finish(c, x, t, s_f[0][t], s_f[1][t], s_f[2][t]);
    g_out[uint3(x, t + 128u, c)] = finish(c, x, t + 128u, s_f[0][t + 128u], s_f[1][t + 128u], s_f[2][t + 128u]);
}
