// ---------------------------------------------------------------------------
// Sun shadows: three cascades, sampled by the terrain (mesh.hlsl) and the
// trees (trees.hlsl) through this one function, so ground and trees agree on
// where the shadow falls.
//
// Cascades are chosen by distance from the camera; each is an orthographic
// view down the sun fitted to a sphere round its slice of the view frustum
// and snapped to whole texels (Renderer::compute_cascades), so its edges stay
// put as the camera moves instead of crawling. Past the last cascade the
// shadow fades out: terrain beyond ~500 m is lit, and fog covers the change.
// ---------------------------------------------------------------------------
#ifndef ENGINE_SHADOW_HLSLI
#define ENGINE_SHADOW_HLSLI

cbuffer ShadowConstants : register(b2) {
    row_major float4x4 light_vp[3];
    float4 shadow_split;       // far edge of each cascade, metres from the camera; w = on
    float4 shadow_texel;       // world metres per shadow texel, per cascade
    float4 shadow_depth_range; // metres of light-space depth per cascade
    float3 shadow_cam;         float shadow_res;
};

Texture2DArray<float>  g_shadow     : register(t8);
SamplerComparisonState g_shadow_cmp : register(s1);

// 1 lit, 0 in shadow. `n` is the surface normal used for the offset; pass the
// geometric one where there is one, a crown's for foliage.
float shadow_factor(float3 world, float3 n) {
    if (shadow_split.w < 0.5f) return 1.0f;
    const float d = length(world - shadow_cam);
    if (d >= shadow_split.z) return 1.0f;
    uint c = d < shadow_split.x ? 0u : (d < shadow_split.y ? 1u : 2u);
    // Offset along the normal by a couple of texels: the cheapest cure for
    // acne that does not detach shadows from their casters.
    const float3 p = world + n * (shadow_texel[c] * 1.5f);
    const float4 lp = mul(float4(p, 1.0f), light_vp[c]);
    const float2 uv = float2(lp.x * 0.5f + 0.5f, 0.5f - lp.y * 0.5f);
    // 0.12 m of constant bias, in this cascade's depth units.
    const float z = lp.z - 0.12f / shadow_depth_range[c];
    const float t = 1.0f / shadow_res;
    float s = 0.0f;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            s += g_shadow.SampleCmpLevelZero(g_shadow_cmp, float3(uv + float2(i, j) * t, float(c)), z);
        }
    }
    s *= (1.0f / 9.0f);
    // Fade over the last 10% of the last cascade.
    const float fade = saturate((shadow_split.z - d) / (0.1f * shadow_split.z));
    return lerp(1.0f, s, fade);
}

#endif
