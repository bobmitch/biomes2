// ---------------------------------------------------------------------------
// Rain (docs/adr-005-clouds-and-rain.md, phase C3): streaks and splashes,
// drawn onto the lit scene after the sea, before TAA. Nothing is stored: a
// drop is a hash of its index.
//
//   vs_streak / ps_streak  drops in a box round the camera, 44 x 24 x 44 m,
//          falling at terminal speed and carried by the wind. Each drop has a
//          FIXED place in the world, wrapped into the box, so walking through
//          the rain does not drag it along. A streak is the drop's path over
//          a short exposure; it is at least ~0.7 px wide with its opacity cut
//          to match, so far streaks thin to a haze instead of aliasing.
//          Under a crown (the canopy map, ADR-004 L6) a drop is kept only with
//          the crown's vertical transmittance: the canopy catches the rain.
//   vs_splash / ps_splash  impacts at WORLD points within 36 m, each fixed
//          while it lives (0.4 s), set on the drawn surface (splash_ground) --
//          ground, rock and sea alike: a crown, and on the sea a ring. Only on
//          surfaces facing up, and only where the canopy lets the rain through.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/canopy_common.hlsli"
#include "engine/render/shaders/hfgi_common.hlsli"

cbuffer RainConstants : register(b0) {
    row_major float4x4 view_proj;       // this frame, jittered: the scene's own
    row_major float4x4 inv_view_proj;
    float3 cam_pos;     float time;
    float  intensity;   float fall;     float box;      float height;   // 0..1; m/s; half widths m
    float2 wind;        float exposure; float pixel_angle;              // m/s; s of smear; radians a pixel
    float3 light;       float near_plane;                               // what a drop sends back, nits
    float4 canopy;      // the canopy map's min corner x, z, its reference height; w > 0.5 built
    float2 inv_size;    float far_plane; float splash_count;
    float  near_drops;  float debug;  float drip;  float pad0;   // drops [0, near_drops) in the near box, the rest in the far one;
                                                   // debug (BIOMES_RAIN_DBG): 1 splashes big and opaque, 2 also unchecked (on the map's height)
    float4 hf;          // the heightfield GI map's min corner x, z; w > 0.5 built (height_gi.h)
};

Texture2D<float>       g_depth  : register(t0);
Texture2DArray<float4> g_canopy : register(t1);
Texture2D<float>       g_hf_height : register(t2);   // the top surface's height (hfgi_common.hlsli)

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(uint i, uint salt) { return float(pcg(i * 9781u + salt * 6271u + 17u) & 0xFFFFFFu) / 16777216.0f; }

float wrap(float x, float L) { return x - L * floor(x / L + 0.5f); }

// How much of the sky's rain reaches (x, y, z) under the crowns: the canopy
// map's vertical transmittance where the point is below the crowns' top.
float canopy_through(float3 p) {
    if (canopy.w < 0.5f) return 1.0f;
    const int2 g = int2(floor((p.xz - canopy.xy) / kCanopyTexel));
    if (any(g < 0) || any(g >= int(kCanopySize))) return 1.0f;
    const float4 c = g_canopy.Load(int4(g, 0, 0));
    if (c.x < 0.02f) return 1.0f;
    const float top = c.z / c.x + canopy.z;
    return p.y < top ? saturate(c.w) : 1.0f;
}

struct StreakOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;   // across 0..1, along 0 (head) .. 1 (tail)
    float  a    : TEXCOORD1;
};

StreakOut degenerate_streak() {
    StreakOut o;
    o.clip = float4(0.0f, 0.0f, -1.0f, 1.0f);
    o.uv = 0.0f;
    o.a = 0.0f;
    return o;
}

StreakOut vs_streak(uint vid : SV_VertexID) {
    const uint drop = vid / 6u, corner = vid % 6u;
    // The rain rate is how many drops there are; the crowns' drips add up to
    // a third as many again under them (below).
    const float keep_roll = rnd(drop, 7u);
    if (keep_roll > max(intensity, drip * 0.35f)) return degenerate_streak();
    // Two layers: the near box, and one three times as wide (half again as
    // tall) whose streaks are mostly under a pixel -- the sheets a downpour
    // shows at a distance. Its drops inside the near box are skipped.
    const bool far_layer = float(drop) >= near_drops;
    const float bx = far_layer ? 3.0f * box : box, hy = far_layer ? 1.5f * height : height;
    // The wind near the ground is slower than at 10 m (a log profile). The
    // drops a crown sheds fall a few metres, sheltered: slower and straighter.
    // Which a drop is depends on where it is, so it is decided below; the
    // drop's speed must not, so a drip is a drop the rain's roll rejects.
    const bool drip_drop = keep_roll > intensity;
    const float wk = drip_drop ? 0.15f : 0.7f;
    const float3 v = float3(wk * wind.x, -fall * (drip_drop ? 0.6f : 0.85f + 0.3f * rnd(drop, 3u)), wk * wind.y);
    const float3 base = float3(rnd(drop, 0u) * 2.0f * bx, rnd(drop, 1u) * 2.0f * hy, rnd(drop, 2u) * 2.0f * bx);
    const float3 w = base + v * time - cam_pos;
    const float3 p = cam_pos + float3(wrap(w.x, 2.0f * bx), wrap(w.y, 2.0f * hy), wrap(w.z, 2.0f * bx));
    const float3 to = p - cam_pos;
    const float dist = length(to);
    if (dist < 0.6f) return degenerate_streak();
    if (far_layer && max(abs(to.x), abs(to.z)) < box - 4.0f && abs(to.y) < height - 3.0f) return degenerate_streak();
    const float through = canopy_through(p);
    if (drip_drop) {
        // A drip: only under crowns, as many as the crowns are dense and wet.
        if (rnd(drop, 11u) > (1.0f - through) || keep_roll > drip * 0.35f) return degenerate_streak();
    } else if (rnd(drop, 11u) > through) {
        return degenerate_streak();
    }
    const float3 vd = normalize(v);
    const float len = length(v) * exposure;
    // At least ~0.7 of a pixel wide, with the opacity cut to match: a 2 mm
    // drop 20 m off is a twentieth of a pixel, and drawn that thin it is
    // aliasing, not rain.
    // The floor is a little wider than a drop's true coverage would say: one
    // drop in a pixel is invisible, a sheet of them is not.
    const float width = max(0.002f, dist * pixel_angle * 0.8f);
    const float fade = min(1.0f, (far_layer ? 0.008f : 0.005f) / width);
    const float3 side = normalize(cross(vd, to)) * (0.5f * width);
    const float2 c = corner == 0u ? float2(0, 0) : corner == 1u ? float2(1, 0) : corner == 2u ? float2(0, 1)
                   : corner == 3u ? float2(1, 0) : corner == 4u ? float2(1, 1) : float2(0, 1);
    const float3 wp = p - vd * (len * c.y) + side * (c.x * 2.0f - 1.0f);
    StreakOut o;
    o.clip = mul(float4(wp, 1.0f), view_proj);
    o.uv = c;
    // Fade in from the box's edges so drops do not pop as they wrap.
    // The far layer fades in where the near one fades out.
    float edge = saturate((bx - max(abs(to.x), abs(to.z))) / (far_layer ? 12.0f : 4.0f)) *
                 saturate((hy - abs(to.y)) / (far_layer ? 6.0f : 3.0f));
    if (far_layer) edge *= 1.0f - saturate((box - max(abs(to.x), abs(to.z))) / 4.0f) * saturate((height - abs(to.y)) / 3.0f);
    // A drop at arm's length smears across the screen as a wire; out of
    // focus, it is barely there.
    const float near = saturate((dist - 0.6f) / 4.0f);
    // Under the crowns a drop sees the canopy above it, not the sky: dimmer
    // (the same transmittance that thins the drops).
    o.a = fade * edge * near * (drip_drop ? 0.5f : 0.35f + 0.65f * through);
    return o;
}

float linear_depth(float z) {
    return near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
}

// Behind the scene at this pixel: no draw (the scene's depth, the sea's
// included).
bool occluded(float4 clip) {
    const float z = g_depth.Load(int3(int2(clip.xy), 0));
    return z < 1.0f && linear_depth(z) < clip.w;
}

// The same with a tolerance: a splash lies ON the surface it is tested against.
bool occluded_by(float4 clip, float tol) {
    const float z = g_depth.Load(int3(int2(clip.xy), 0));
    return z < 1.0f && linear_depth(z) < clip.w * 0.99f - tol;
}

float4 ps_streak(StreakOut i) : SV_Target0 {
    if (occluded(i.clip)) discard;
    const float across = 1.0f - abs(i.uv.x * 2.0f - 1.0f);
    const float along = smoothstep(0.0f, 0.25f, i.uv.y) * smoothstep(1.0f, 0.6f, i.uv.y);
    const float a = saturate(0.5f * across * along * i.a);
    return float4(light, a);
}

// --- splashes -------------------------------------------------------------------

struct SplashOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;   // -1..1 over the ring's square
    float  age  : TEXCOORD1;
    float  a    : TEXCOORD2;
    float  ring : TEXCOORD3;   // 1 on the sea: a ring; elsewhere only the crown
};

SplashOut degenerate_splash() {
    SplashOut o;
    o.clip = float4(0.0f, 0.0f, -1.0f, 1.0f);
    o.uv = 0.0f;
    o.age = 1.0f;
    o.a = 0.0f;
    o.ring = 0.0f;
    return o;
}

float3 world_at(float2 uv, float z) {
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(float4(ndc, z, 1.0f), inv_view_proj);
    return h.xyz / h.w;
}

// Where a splash lands: the heightfield GI's map of the top surface (2 m
// texels, hfgi_common.hlsli) gives a first height; the depth buffer, read
// where that point projects, gives the surface plane there, and the splash
// goes where the plane meets the vertical through its own (x, z). So the
// splash is a WORLD point -- it does not slide with the view, which a point
// read from a random pixel did -- and it sits on the drawn surface, sea
// included. False if the surface there is not in view or not near the map's.
bool splash_ground(float2 xz, out float3 P, out float3 n) {
    P = 0.0f;
    n = float3(0.0f, 1.0f, 0.0f);
    if (hf.w < 0.5f) return false;
    const float2 huv = float2((xz.x - hf.x) / kHfExtent, 1.0f - (xz.y - hf.y) / kHfExtent);
    if (any(huv <= 0.0f) || any(huv >= 1.0f)) return false;
    // Bilinear by hand (Load only; no sampler bound).
    const float2 tc = huv * float(kHfSize) - 0.5f;
    const int2 t0 = int2(floor(tc));
    const float2 f = tc - float2(t0);
    const int2 hi = int2(kHfSize - 1u, kHfSize - 1u);
    const float h00 = g_hf_height.Load(int3(clamp(t0, int2(0, 0), hi), 0));
    const float h10 = g_hf_height.Load(int3(clamp(t0 + int2(1, 0), int2(0, 0), hi), 0));
    const float h01 = g_hf_height.Load(int3(clamp(t0 + int2(0, 1), int2(0, 0), hi), 0));
    const float h11 = g_hf_height.Load(int3(clamp(t0 + int2(1, 1), int2(0, 0), hi), 0));
    if (min(min(h00, h10), min(h01, h11)) < kHfNone * 0.5f) return false;
    float h = lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
    // The map sees the seabed, the depth buffer the sea.
    const bool sea = h < 0.0f;
    if (sea) h = 0.0f;
    const float3 guess = float3(xz.x, h, xz.y);
    const float4 c = mul(float4(guess, 1.0f), view_proj);
    if (c.w <= near_plane) return false;
    const float2 ndc = c.xy / c.w;
    if (any(abs(ndc) > 0.98f)) return false;
    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    const int2 px = int2(uv / inv_size);
    const float z = g_depth.Load(int3(px, 0));
    if (z >= 1.0f) return false;
    const float3 S = world_at((float2(px) + 0.5f) * inv_size, z);
    const int2 px1 = px + int2(2, 0), px2 = px + int2(0, 2);
    const float3 Sx = world_at((float2(px1) + 0.5f) * inv_size, g_depth.Load(int3(px1, 0)));
    const float3 Sy = world_at((float2(px2) + 0.5f) * inv_size, g_depth.Load(int3(px2, 0)));
    n = normalize(cross(Sy - S, Sx - S));
    if (n.y < 0.0f) n = -n;
    if (n.y < 0.75f && debug < 1.5f) return false;
    // The plane through S meets the vertical at xz.
    const float y = S.y - (n.x * (xz.x - S.x) + n.z * (xz.y - S.z)) / n.y;
    // Something else in front (a trunk, a rock, a leaf) or ground the map
    // does not describe: no splash rather than a guess.
    const float dy = abs(y - h);
    if (debug > 1.5f) { P = guess; n = float3(0.0f, 1.0f, 0.0f); return true; }
    if (dy > (sea ? 3.5f : 0.6f) || length(S - guess) > 3.0f + dy) return false;
    P = float3(xz.x, y, xz.y);
    return true;
}

SplashOut vs_splash(uint vid : SV_VertexID) {
    const uint id = vid / 6u, corner = vid % 6u;
    const float roll = rnd(id, 5u);
    if (roll > max(intensity, drip * 0.35f)) return degenerate_splash();
    const float life = 0.4f;
    const float ph = rnd(id, 1u);
    const float cyc = time / life + ph;
    const uint epoch = uint(floor(cyc));
    const float age = frac(cyc);
    const uint k = id * 7919u + epoch * 104729u;
    // A world spot for this life of the splash, wrapped into a square round
    // the camera as the drops are: fixed in the world while it lives.
    const float R = 36.0f;
    const float2 base = float2(rnd(k, 2u), rnd(k, 3u)) * (2.0f * R);
    const float2 off = float2(wrap(base.x - cam_pos.x, 2.0f * R), wrap(base.y - cam_pos.z, 2.0f * R));
    const float hd = length(off);
    if (hd > R || hd < 0.8f) return degenerate_splash();
    float3 P, n;
    if (!splash_ground(cam_pos.xz + off, P, n)) return degenerate_splash();
    const float through = canopy_through(P + float3(0.0f, 0.05f, 0.0f));
    // As the streaks: the rain where the crowns let it through, the drips
    // where they do not.
    if (roll > intensity) {
        if (rnd(k, 9u) > 1.0f - through || roll > drip * 0.35f) return degenerate_splash();
    } else if (rnd(k, 9u) > through) {
        return degenerate_splash();
    }
    const float dist = length(P - cam_pos);
    // A few centimetres: a 2 mm drop's crown, and on water the ring it
    // leaves. Never under ~1.5 pixels across, with the opacity cut to
    // match -- linearly, not by area, so a far splash still reads.
    // On the sea the ring spreads to ~12 cm; on land the crown is ~4 cm.
    const float rw = abs(P.y) < 3.0f ? 0.03f + 0.09f * age : 0.025f + 0.02f * age;
    const float r = debug > 0.5f ? 0.3f : max(rw, dist * pixel_angle * 0.75f);
    const float3 t1 = normalize(cross(n, float3(0.0f, 0.0f, 1.0f)));
    const float3 t2 = cross(n, t1);
    const float2 c = corner == 0u ? float2(-1, -1) : corner == 1u ? float2(1, -1) : corner == 2u ? float2(-1, 1)
                   : corner == 3u ? float2(1, -1) : corner == 4u ? float2(1, 1) : float2(-1, 1);
    const float3 wp = P + n * 0.01f + (t1 * c.x + t2 * c.y) * r;
    SplashOut o;
    o.clip = mul(float4(wp, 1.0f), view_proj);
    o.uv = c;
    o.age = age;
    o.a = saturate((R - hd) / 8.0f) * (0.35f + 0.65f * through) * (rw / r);
    // Rings belong to standing water. On leaves and grass they read as
    // bubbles; there a drop throws up a crown and is gone. The sea is the
    // only water drawn, at y = 0 give or take its waves.
    o.ring = abs(P.y) < 3.0f ? 1.0f : 0.0f;
    if (debug > 0.5f) o.a = 4.0f;
    return o;
}

float4 ps_splash(SplashOut i) : SV_Target0 {
    if (occluded_by(i.clip, 0.08f) && debug < 1.5f) discard;
    const float r = length(i.uv);
    const float ring = smoothstep(0.6f, 0.8f, r) * (1.0f - smoothstep(0.85f, 1.0f, r));
    // On the sea the crown is the first instant, then the ring; on land
    // there is only the crown, held for half the life.
    const float crown_r = i.ring > 0.5f ? 0.35f : 0.8f;
    const float crown = (1.0f - smoothstep(0.0f, crown_r, r)) *
                        (1.0f - smoothstep(0.0f, i.ring > 0.5f ? 0.25f : 0.5f, i.age));
    const float a = saturate((ring * 0.5f * i.ring + crown * 0.6f) * (1.0f - i.age) * i.a);
    return float4(light, a);
}
