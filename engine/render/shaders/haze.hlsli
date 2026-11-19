// ---------------------------------------------------------------------------
// The sky and the air between the eye and a surface (ADR-004 L2, L4), for
// every pass that composites over the scene: post.hlsl's cs_composite, and
// the sea (water.hlsl), which draws after it.
//
// The includer declares, by these names:
//   g_skyview (Texture2D), g_ap and g_fog (Texture3D), samplers g_linear and
//   g_wrap_u; and in its constants cam_alt_km, sky_sun_dir, sky_moon_dir,
//   sun_disk, moon_disk, far_plane, fog_on, fog_near, fog_log_ratio.
// and includes engine/render/atmosphere.h and lighting_units.h first.
// ---------------------------------------------------------------------------
#ifndef HAZE_HLSLI
#define HAZE_HLSLI

// The sky along a direction from the sky-view table alone, no disks: what a
// rough reflection sees, where the sun's own glint is drawn separately.
float3 sky_view_at(float3 d) {
    float u, v;
    atm_sky_uv(d, kAtmGround + cam_alt_km, u, v);
    return g_skyview.SampleLevel(g_wrap_u, float2(u, v), 0).rgb;
}

// The sky along a direction: the sky-view table, then the sun's disk, limb
// darkened, and the moon's.
float3 sky_at(float3 d) {
    float3 c = sky_view_at(d);
    const float r_sun = 0.2667f * 0.0174533f, r_moon = 0.259f * 0.0174533f;
    // The angle to each centre, from the chord: acos is too coarse this
    // close to 1 in single precision.
    const float a_sun = 2.0f * asin(saturate(0.5f * length(d - sky_sun_dir)));
    const float a_moon = 2.0f * asin(saturate(0.5f * length(d - sky_moon_dir)));
    if (a_sun < r_sun) {
        const float t = a_sun / r_sun;
        c += sun_disk * (1.0f - 0.6f * (1.0f - sqrt(max(0.0f, 1.0f - t * t))));
    }
    if (a_moon < r_moon) c += moon_disk;
    return min(c, kSceneMaxNits);
}

// The haze between the camera and a point `dist` metres away along this
// pixel's ray: (in-scattered light, transmittance). The volume's slice k ends
// at far * ((k + 1) / 32)^2 (atmosphere.hlsl); nearer than slice 0 fades
// from nothing.
float4 aerial(float2 uv, float dist) {
    const float w = sqrt(saturate(dist / far_plane)) * 32.0f - 1.0f;
    float4 s = g_ap.SampleLevel(g_linear, float3(uv, (max(w, 0.0f) + 0.5f) / 32.0f), 0);
    if (w < 0.0f) {
        const float f = w + 1.0f;
        s = float4(s.rgb * f, lerp(1.0f, s.a, f));
    }
    return s;
}

// The mist between the eye and view depth z (L4). Slice s of the volume holds
// what has accumulated by the FAR side of the slice, view depth
// near * e^((s + 1) / 128 * ln(far / near)); nearer than slice 0's far side
// fades from nothing.
float4 mist(float2 uv, float z) {
    if (fog_on < 0.5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float n = 128.0f;
    const float z1 = fog_near * exp(fog_log_ratio / n);
    if (z <= z1) {
        const float4 s0 = g_fog.SampleLevel(g_linear, float3(uv, 0.5f / n), 0);
        const float f = saturate(z / z1);
        return float4(s0.rgb * f, lerp(1.0f, s0.a, f));
    }
    const float w = log(z / fog_near) / fog_log_ratio * n;
    return g_fog.SampleLevel(g_linear, float3(uv, saturate((w - 0.5f) / n)), 0);
}

#endif
