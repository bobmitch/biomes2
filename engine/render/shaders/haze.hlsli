// ---------------------------------------------------------------------------
// The sky and the air between the eye and a surface (ADR-004 L2, L4), for
// every pass that composites over the scene: post.hlsl's cs_composite, and
// the sea (water.hlsl), which draws after it.
//
// The includer declares, by these names:
//   g_skyview (Texture2D), g_ap and g_fog (Texture3D), samplers g_linear and
//   g_wrap_u; and in its constants cam_alt_km, sky_sun_dir, sky_moon_dir,
//   sun_disk, moon_disk, far_plane, fog_on, fog_near, fog_log_ratio, and the
//   height fog's hfog_a, hfog_b, hfog_c (below).
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

// --- The exploration fog: layered HEIGHT fog -------------------------------------
//
// What limits how far the land can be seen. Three layers whose extinction
// falls off exponentially with ALTITUDE above a base (sea level), not with
// height above the local ground: from the lowland every near-horizontal
// sight line runs through the dense air and the land closes in at ~1.5 km;
// a peak's summit stands out of it; and from a hill above the layer the
// lowland a few km off is hidden -- the line down to it crosses kilometres
// of the dense air -- while the ground just below, seen steeply, is not.
// Terrain-following mist (fog.hlsl's map) cannot do that: it thins over a
// hilltop as much as over a valley floor, and climbing cleared the view.
//
//   ground  shallow and dense: the lowland's ~1.5 km
//   valley  shallower and denser still, swelling after dawn and blown away
//           by wind: the morning's sea of fog
//   deep    thin and tall: peaks fade by ~20 km
//
// Analytic: the optical depth of an exponential layer along a straight ray
// has a closed form, exact at any range -- the sky included -- and cheap per
// pixel; the froxel mist keeps the near field's patches and light shafts.
//
//   hfog_a  ground sigma at the base (1/m), its scale height (m); valley the same
//   hfog_b  deep sigma, its scale height; the base's altitude; w > 0.5 on
//   hfog_c  the fog's colour: x how far towards grey (cloud cover), y its
//           dimming (a storm deck), z the albedo, w the eye's sky share

// One layer's optical depth from height h0 above the base along a ray whose
// height changes by dy a metre, over t metres. Below the base the layer is
// as dense as at it (the base is a floor, not a mirror).
float hfog_layer(float s0, float H, float h0, float dy, float t) {
    if (s0 <= 0.0f) return 0.0f;
    h0 = max(h0, 0.0f);
    // The stretch above the base, and the stretch (if any) under it.
    float t_above = t, t_below = 0.0f;
    if (dy < 0.0f) {
        const float tb = h0 / -dy;
        if (tb < t) { t_above = tb; t_below = t - tb; }
    }
    float tau;
    if (abs(dy) < 1e-5f) {
        tau = s0 * exp(-h0 / H) * t_above;
    } else {
        const float h1 = max(h0 + dy * t_above, 0.0f);
        tau = s0 * H / dy * (exp(-h0 / H) - exp(-h1 / H));
    }
    return tau + s0 * t_below;
}

float hfog_tau(float3 o, float3 d, float t) {
    if (hfog_b.w < 0.5f) return 0.0f;
    const float h0 = o.y - hfog_b.z;
    return hfog_layer(hfog_a.x, hfog_a.y, h0, d.y, t) + hfog_layer(hfog_a.z, hfog_a.w, h0, d.y, t) +
           hfog_layer(hfog_b.x, hfog_b.y, h0, d.y, t);
}

// What thick fog looks like along this direction: the sky's own horizon in
// that azimuth -- warm and bright towards the sun (the atmosphere's Mie
// glow is in the table), cool away from it, dark at night -- so the land
// fades into the horizon rather than into a grey wall; greyed and dimmed as
// the deck closes, as fill_light greys the sky light.
float3 hfog_colour(float3 d) {
    const float3 hz = normalize(float3(d.x, 0.04f, d.z) + float3(1e-4f, 0.0f, 0.0f));
    float3 c = sky_view_at(hz);
    // Looking DOWN into the fog, the light it sends up is the whole sky's
    // round it, not the horizon's in one azimuth: taken alone, every ray
    // towards the sun's azimuth -- straight down included -- carried the
    // sun's glow, a pillar of light standing in the fog at dusk.
    if (d.y < 0.0f) {
        const float3 r = normalize(float3(-hz.z, 0.0f, hz.x));
        const float3 a = 0.25f * (sky_view_at(normalize(float3(-hz.x, 0.04f, -hz.z))) +
                                  sky_view_at(normalize(r + float3(0.0f, 0.04f, 0.0f))) +
                                  sky_view_at(normalize(-r + float3(0.0f, 0.04f, 0.0f))) + c);
        c = lerp(c, a, saturate(-d.y * 4.0f));
    }
    c *= hfog_c.z;
    const float y = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    return lerp(c, float3(y, y, y), hfog_c.x) * hfog_c.y;
}

// (in-scattered light, transmittance) from the eye `o` along `d` for `t`
// metres. Near the eye the air is lit as the eye is (hfog_c.w, the GI's
// share of open sky), so a sealed chamber does not glow with daylit fog.
float4 height_fog(float3 o, float3 d, float t) {
    const float tau = hfog_tau(o, d, t);
    if (tau <= 1e-5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float T = exp(-min(tau, 60.0f));
    const float lit = lerp(hfog_c.w, 1.0f, saturate((t - 64.0f) / 192.0f));
    return float4(hfog_colour(d) * ((1.0f - T) * lit), T);
}

#endif
