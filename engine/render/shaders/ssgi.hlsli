// ---------------------------------------------------------------------------
// Reading the screen-space bounce (ADR-004 L6, an experiment, OFF by default:
// it added 1-2% to the shade for 0.3 ms, because the bounce that matters in
// this world comes from terrain and canopy that is off screen). The irradiance
// post.hlsl's cs_ssgi measured LAST frame from the surfaces on screen --
// the light leaving every surface within a few metres, gathered through a
// visibility bitmask (Therrien et al. 2023, "Screen Space Indirect Lighting
// with Visibility Bitmask"). The scene reprojects the point into last
// frame's view and takes it where the depth agrees.
//
// Included by shadow.hlsli, whose constants carry last frame's matrix and
// camera; the texture is a fifth range of the lit pipelines' shadow table,
// heap [45] (t29), sampled through ddgi.hlsli's linear sampler (s3).
// ---------------------------------------------------------------------------
#ifndef ENGINE_SSGI_HLSLI
#define ENGINE_SSGI_HLSLI

Texture2D<float4> g_ssgi : register(t29);   // rgb irradiance (lux), a distance from that frame's camera

float3 ssgi_irradiance(float3 world) {
    if (l_prev_cam.w < 0.5f) return float3(0.0f, 0.0f, 0.0f);
    const float4 c = mul(float4(world, 1.0f), l_prev_vp);
    if (c.w <= 1e-4f) return float3(0.0f, 0.0f, 0.0f);
    const float2 uv = float2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f);
    if (any(uv < 0.0f) || any(uv > 1.0f)) return float3(0.0f, 0.0f, 0.0f);
    const float4 s = g_ssgi.SampleLevel(g_ddgi_linear, uv, 0.0f);
    // Last frame saw something else here -- a nearer surface, or this one
    // from the far side of an edge the bilinear tap straddles.
    const float d = length(world - l_prev_cam.xyz);
    if (l_prev_cam.w > 2.5f) return s.rgb;   // BIOMES_SSGI_DBG=2: no depth check
    if (s.a <= 0.0f || abs(s.a - d) > 0.05f * d + 0.25f) return float3(0.0f, 0.0f, 0.0f);
    // Fade out at the screen's edge, where the next frame's view loses it.
    const float2 e = min(uv, 1.0f - uv);
    return s.rgb * saturate(min(e.x, e.y) * 20.0f);
}

#endif
