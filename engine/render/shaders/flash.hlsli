// ---------------------------------------------------------------------------
// A lightning flash's light on a surface (ADR-005 C5; lightning.h). Included
// after scene_out.hlsli by the lit scene shaders -- terrain, rocks, trees,
// grass -- whose shadow constants carry l_flash (where the light is, its
// luminous intensity in cd, 0 none) and l_flash_col (its colour).
//
// A point source kilometres off: inverse square from the flash point, no
// window, so it lights the whole landscape and falls off over it. Into the
// AMBIENT target, not the direct one: the composite multiplies the direct
// target by the SUN's shadow and the clouds' shadow, which have nothing to
// say about the flash. Dimmed by the sky visibility the surface already
// worked out (the canopy, the probes), which is the nearest thing to the
// flash's own shadow the scene has -- a wood's floor flickers, it does not
// light up as an open field does.
// ---------------------------------------------------------------------------
#ifndef FLASH_HLSLI
#define FLASH_HLSLI

float3 flash_at(float3 world, out float3 l) {
    const float3 d = l_flash.xyz - world;
    const float r2 = max(dot(d, d), 1.0f);
    l = d * rsqrt(r2);
    return l_flash_col.rgb * (l_flash.w / r2);
}

float3 flash_light(Surface s, float3 world) {
    if (l_flash.w <= 0.0f) return 0.0f;
    float3 l;
    const float3 E = flash_at(world, l);
    return direct_light(s, l, E, 1.0f) * gi_sky_visibility(world, s.n);
}

float3 flash_leaf(float3 albedo, float3 n, float3 v, float3 world, float wrap, float through) {
    if (l_flash.w <= 0.0f) return 0.0f;
    float3 l;
    const float3 E = flash_at(world, l);
    return leaf_light(albedo, n, v, l, E, wrap, through, 1.0f) * gi_sky_visibility(world, n);
}

#endif
