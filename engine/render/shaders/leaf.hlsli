// ---------------------------------------------------------------------------
// Shared by the trees and the ground cover: hashes, value noise, the
// screen-door dither, and the procedural leaf pattern a card is cut from.
// One copy, so a bush's leaves and a tree's are the same leaves.
// ---------------------------------------------------------------------------
#ifndef ENGINE_LEAF_HLSLI
#define ENGINE_LEAF_HLSLI

// A dither in [0, 1): interleaved gradient noise. A LOD swap and the far
// fade are screen-door fades on it; complementary sides of one threshold
// draw every pixel exactly once, as mesh.hlsl does for terrain chunks.
float dither(float2 px) {
    return frac(52.9829189f * frac(dot(px, float2(0.06711056f, 0.00583715f))));
}

float hash11(float x) {
    uint h = asuint(x) * 0x9E3779B1u;
    h = (h ^ (h >> 15)) * 0x85EBCA77u;
    return float((h ^ (h >> 13)) & 0xFFFFu) / 65535.0f;
}

float hash2(float2 c) {
    const uint2 q = uint2(int2(floor(c)) + int2(0x4000, 0x4000));
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u;
    h = (h ^ (h >> 13)) * 0xcb1ab31fu;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

float vnoise(float2 p) {
    const float2 c = floor(p);
    const float2 f = p - c;
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(hash2(c), hash2(c + float2(1, 0)), u.x),
                lerp(hash2(c + float2(0, 1)), hash2(c + float2(1, 1)), u.x), u.y);
}

// A card's leaf coverage in [0, 1], procedural. Broad leaves: a cluster of
// pointed ellipses round a stalk. Needles: a twig with fine strokes off it.
// `blur` widens every edge as the card shrinks on screen, so coverage tends
// to the pattern's average instead of shimmering, and the dithered test then
// keeps that fraction of pixels.
// One layer of small leaves: the card is a grid of `cells` x `cells`, and
// each cell holds one pointed leaf, jittered and turned by its own hash. Only
// the pixel's own cell is tested, so the cost is one leaf per layer whatever
// the count; leaves stay inside their cell, so nothing is cut off at an edge.
// `narrow` stretches them (a willow's).
float leaf_layer(float2 uv, float cells, float seed, float narrow) {
    const float2 g = uv * cells;
    const float2 c = floor(g);
    const float2 f = g - c - 0.5f;
    const float h0 = hash2(c + seed * 97.0f);
    const float h1 = hash2(c.yx + seed * 53.0f + 17.0f);
    const float h2 = hash2(c + seed * 31.0f + 41.0f);
    if (h2 < 0.06f) return 0.0f;   // a few gaps, so a card is not a lattice
    const float ang = h0 * 6.2831853f;
    const float2 d = f - (float2(h1, h2) - 0.5f) * 0.18f;
    const float2 r = float2(d.x * cos(ang) + d.y * sin(ang), -d.x * sin(ang) + d.y * cos(ang));
    const float len = 0.47f;
    const float wid = (narrow > 0.5f ? 0.10f : 0.25f) * (1.0f - 0.55f * saturate(r.x / len + 0.5f));
    const float e = (r.x * r.x) / (len * len) + (r.y * r.y) / (wid * wid);
    return saturate((1.0f - e) / 0.2f);
}

float leaf_coverage_sharp(float2 uv, float kind, float seed, float blur, float narrow);

// What leaf_coverage converges to as the card shrinks: the pattern's own
// average fill. The first version only widened the edges with `blur`, which
// DIVIDED coverage towards zero, so every crown thinned out with distance.
float leaf_coverage(float2 uv, float kind, float seed, float blur, float narrow) {
    // A round card: a cluster of leaves, not a square of them.
    const float rim = 1.0f - saturate((length(uv - 0.5f) - 0.40f) / 0.1f);
    const float avg = kind < 1.5f ? 0.55f : 0.5f;
    return rim * lerp(leaf_coverage_sharp(uv, kind, seed, blur, narrow), avg, saturate(blur * 5.0f - 0.25f));
}

float leaf_coverage_sharp(float2 uv, float kind, float seed, float blur, float narrow) {
    if (kind < 1.5f) {
        // Two layers half a cell apart: leaves overlap irregularly, which a
        // single grid never does. 0.2 m leaves on a 1 m card at 7 cells.
        const float a0 = leaf_layer(uv, 7.0f, seed, narrow);
        const float a1 = leaf_layer(uv + 0.5f / 7.0f, 7.0f, seed + 0.37f, narrow);
        return max(a0, a1);
    }
    const float twig = 1.0f - saturate(abs(uv.x - 0.5f) / max(0.02f + blur, 0.03f));
    const float along = frac(uv.y * 9.0f + abs(uv.x - 0.5f) * 6.0f + seed * 3.0f);
    const float stroke = 1.0f - saturate(abs(along - 0.5f) / max(0.18f + blur * 2.0f, 0.2f));
    const float body = saturate(1.0f - abs(uv.x - 0.5f) * 2.1f) * saturate(1.0f - abs(uv.y - 0.5f) * 2.05f);
    return max(twig, stroke * step(0.05f, body) * saturate(body * 4.0f));
}

#endif
