// ---------------------------------------------------------------------------
// The clouds' shadow at a world point (ADR-005 C2; clouds.hlsl cs_shadow).
//
// `map` is the top-down transmittance of the key light through the cloud
// layer, indexed in the plane of the cloud BASE; `cs` its centre x, z, half
// extent (m) and the base's altitude (m); `l` towards the key light. A point
// slides along the light onto the base's plane to find its texel, so one map
// serves every height. Outside the map, 1.
//
// One function for every reader -- the composite's direct light, the sea's
// glint and body, the mist's in-scatter -- so they all see the same clouds.
// ---------------------------------------------------------------------------
#ifndef CLOUD_SHADOW_HLSLI
#define CLOUD_SHADOW_HLSLI

float cloud_shadow_at(Texture2D<float> map, SamplerState s, float4 cs, float3 l, float3 p) {
    if (l.y <= 0.03f || cs.z <= 0.0f) return 1.0f;
    const float2 q = p.xz + l.xz * ((cs.w - p.y) / l.y);
    const float2 uv = (q - cs.xy) / (2.0f * cs.z) + 0.5f;
    if (any(uv <= 0.0f) || any(uv >= 1.0f)) return 1.0f;
    return map.SampleLevel(s, uv, 0);
}

#endif
