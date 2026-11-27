// ---------------------------------------------------------------------------
// The trample map (engine/foliage/trample.h): where something has stood in the
// ground cover, and which way it pushed it.
//
//   cs_clear -- the strips that scrolled in as the camera moved, or the lot.
//   cs_splat -- this frame's capsules, into both pairs of channels.
//   cs_decay -- the fade: fast (xy) in a fraction of a second, slow (zw) in
//               tens of them. One field, two recovery rates, so a bush springs
//               back and grass stays laid over without a second system.
//
// TOROIDAL: world texel w lives at w mod N. N is a power of two, so the wrap
// is a mask here and frac(world / map_m) with a WRAP sampler on the reading
// side -- the map follows the camera without anything being moved.
//
// Presentation only: no hash, no save, nothing physics reads.
// ---------------------------------------------------------------------------

cbuffer TrampleConstants : register(b0) {
    float  t_texel;        // metres a texel
    float  t_map;          // metres a side (N * t_texel)
    uint   t_n;            // texels a side, a power of two
    uint   t_stamps;       // how many capsules this frame

    float  t_fast;         // this batch's fade, as a multiplier, for xy
    float  t_slow;         // and for zw
    int    t_clear_x;      // the strip to clear: origin and size in texels
    int    t_clear_z;
    int    t_clear_w;
    int    t_clear_h;
    float  t_pad0;
    float  t_pad1;
};

struct Stamp {
    float x0, z0, x1, z1;
    float radius, strength, pad0, pad1;
};

StructuredBuffer<Stamp>     g_stamps : register(t0);
RWTexture2D<float4>         g_map    : register(u0);

// --- the strips that scrolled in -------------------------------------------

[numthreads(16, 16, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    if (int(id.x) >= t_clear_w || int(id.y) >= t_clear_h) return;
    const uint2 c = uint2((uint(t_clear_x + int(id.x))) & (t_n - 1u),
                          (uint(t_clear_z + int(id.y))) & (t_n - 1u));
    g_map[c] = 0.0f;
}

// --- this frame's capsules --------------------------------------------------

// A thread per texel of a fixed footprint round each stamp; the CPU splits a
// capsule longer than the footprint can hold (a felled log) into pieces.
static const uint kSplatGroups = 4u;        // x kSplatGroups, of 16 x 16
static const uint kSplatSpan = kSplatGroups * 16u;   // 64 texels = 6.4 m

[numthreads(16, 16, 1)]
void cs_splat(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const uint si = g.z;
    if (si >= t_stamps) return;
    const Stamp s = g_stamps[si];
    if (s.strength <= 0.0f || s.radius <= 0.0f) return;

    // The footprint's corner in world texels: the capsule's own box, centred.
    const float cx = 0.5f * (s.x0 + s.x1), cz = 0.5f * (s.z0 + s.z1);
    const int bx = int(floor(cx / t_texel)) - int(kSplatSpan / 2u);
    const int bz = int(floor(cz / t_texel)) - int(kSplatSpan / 2u);
    const int tx = bx + int(g.x * 16u + t.x);
    const int tz = bz + int(g.y * 16u + t.y);

    // The texel's middle, in world metres.
    const float2 p = float2((float(tx) + 0.5f) * t_texel, (float(tz) + 0.5f) * t_texel);

    // Distance to the capsule's axis.
    const float2 a = float2(s.x0, s.z0), b = float2(s.x1, s.z1);
    const float2 ab = b - a;
    const float len2 = dot(ab, ab);
    const float u = len2 > 1.0e-6f ? saturate(dot(p - a, ab) / len2) : 0.0f;
    const float2 near_pt = a + ab * u;
    const float2 v = p - near_pt;
    const float d = length(v);
    if (d >= s.radius) return;

    // Flat under the middle, splayed at the rim, nothing past it -- the curve
    // the analytic spike was tuned on.
    const float2 away = d > 1.0e-4f ? v / d : float2(1.0f, 0.0f);
    const float2 push = away * (s.strength * (1.0f - d / s.radius));

    const uint2 c = uint2(uint(tx) & (t_n - 1u), uint(tz) & (t_n - 1u));
    const float4 old = g_map[c];
    // The stronger push wins, per pair. Two stamps in one dispatch can race
    // for a texel -- two feet 20 cm apart -- and either answer is the same
    // push to within a pixel, so it is left alone rather than paid for with
    // atomics on a packed format.
    const float4 n = float4(dot(push, push) > dot(old.xy, old.xy) ? push : old.xy,
                            dot(push, push) > dot(old.zw, old.zw) ? push : old.zw);
    g_map[c] = n;
}

// --- the fade ---------------------------------------------------------------

[numthreads(16, 16, 1)]
void cs_decay(uint3 id : SV_DispatchThreadID) {
    if (id.x >= t_n || id.y >= t_n) return;
    const uint2 c = id.xy;
    float4 v = g_map[c];
    v.xy *= t_fast;
    v.zw *= t_slow;
    // Below a hundredth of a blade's height nothing can be seen, and a value
    // that never quite reaches zero keeps the whole map dirty for ever.
    if (dot(v.xy, v.xy) < 1.0e-4f) v.xy = 0.0f;
    if (dot(v.zw, v.zw) < 1.0e-4f) v.zw = 0.0f;
    g_map[c] = v;
}
