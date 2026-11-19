// ---------------------------------------------------------------------------
// The sea.
//
// Why this exists: the generator produces an archipelago -- the harness
// measures 7.2 landmasses a seed with the spawn mass holding 74% of the land
// -- and before this, none of it was visible. sea_level is 0 and land fraction
// is 35%, so about two thirds of every vista was seabed drawn as slightly
// lower green ground, and the whole world read as one continent. A coastline
// without water is not a coastline.
//
// Three things make it a surface rather than a blue plane:
//
//  1. Gerstner displacement from engine/render/water_wave.h, which is the same
//     text the C++ harness runs. --sea-report measures that function; this
//     draws it. They cannot drift.
//
//  2. The water asks the SHARED FIELD FUNCTION how deep it is. real_field.hlsl
//     is the same terrain the mesher builds from, so the shoreline here is the
//     actual shoreline rather than a painted gradient, and it is correct at
//     every LOD because it does not come from the LOD at all. This is M1 being
//     worth something beyond the terrain: the field is available to anything
//     that wants to ask it.
//
//  3. Displacement fades out with distance. The grid samples every 167 m near
//     the horizon and the shortest wave is 5.3 m, so displacing out there
//     produces aliasing confetti. Distant sea is drawn flat with a sheen,
//     which is also what distant sea looks like.
// ---------------------------------------------------------------------------

#include "engine/field/real_field.hlsl"
#include "engine/render/water_wave.h"

cbuffer WaterConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;    float time;
    float3 sun_dir;    float far_plane;
    float3 cam_fwd;    float aspect;
    float3 cam_right;  float tan_half_fov;
    float3 cam_up;     float pad1;
    float  sea_level;  float sea_state;  float grid_cells;  float extent;
    float  debug_mode; float cam_water; float2 pad0;
    // The frame's light (ADR-004 L2), the same values the shadow constants
    // carry for everything else; lighting.hlsli reads them by these names.
    float4 l_sun_dir;
    float4 l_sun_lux;
    float4 l_sky_mean;
    float4 l_ground;
    float4 l_sky_zenith;
    float4 l_sky_horizon_sun;
    float4 l_sky_horizon_away;
    // The weather's sea (water_wave.h, weather.cpp): each train's deep-water
    // amplitude, and the crests' target steepness. sea_state above is now
    // the weather's roughness, the wind over 20 m/s.
    float4 wave_amp[WW_WAVES / 4];
    float  wave_steep; float3 pad2;
    // Since the sea draws AFTER the composite, onto the lit scene: what it
    // needs of the post chain (haze.hlsli reads these by name), and its optics.
    float2 inv_size;      float near_plane;    float cam_alt_km;
    float3 sky_sun_dir;   float fog_on;
    float3 sky_moon_dir;  float fog_near;
    float3 sun_disk;      float fog_log_ratio;
    float3 moon_disk;     float cam_sky;
    float3 extinction;    float ssr_on;       // per metre, linear rgb
    float3 deep_colour;   float refraction;   // linear; the screen shift at full strength
    float  frame;         float storm_turbidity;  float2 pad3;   // extinction x (1 + this x roughness^2)
};

// The lit scene and its depth as the composite left them -- copies, because
// the sea draws into the one and tests against the other -- and the sky and
// the air (haze.hlsli).
Texture2D<float4> g_scene_in : register(t0);
Texture2D<float>  g_depth_in : register(t1);
Texture2D<float4> g_skyview  : register(t2);
Texture3D<float4> g_ap       : register(t3);
Texture3D<float4> g_fog      : register(t4);
SamplerState g_linear : register(s0);
SamplerState g_point  : register(s1);
SamplerState g_wrap_u : register(s2);

#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/atmosphere.h"
#include "engine/render/shaders/haze.hlsli"

WwSea frame_sea() {
    WwSea s;
    [unroll] for (int i = 0; i < WW_WAVES; ++i) s.amp[i] = wave_amp[i >> 2][i & 3];
    s.steep = wave_steep;
    return s;
}

struct VSOut {
    float4 clip   : SV_Position;
    float3 world  : TEXCOORD0;
    float3 nrm    : TEXCOORD1;
    float  depth  : TEXCOORD2;   // metres of water under this point
    float  dist   : TEXCOORD3;
    float  fade   : TEXCOORD4;
    float  minwl  : TEXCOORD5;   // shortest wave this spacing can carry
    float  hs     : TEXCOORD6;   // significant height the grid carries here (the foam's yardstick)
    float2 cap_q  : TEXCOORD7;   // ww_local's breaking scale and Q, for the pixel's own normal
};

// A PROJECTED grid: the vertices are laid out on the SCREEN and dropped onto
// the sea plane, not laid out on the sea and projected to the screen.
//
// The world-space grid this replaces could not work and the arithmetic says
// why. Spacing has to run from under a metre at the camera to kilometres at
// the horizon -- a range of 32,000 to 1 -- and no warp of 192 cells covers
// that. The version that concentrated hard enough to give 0.5 m underfoot was
// already at 16 m spacing 100 metres out, which cut every wave in the sea: a
// storm rendered as a flat plane. Backing the warp off traded that for visible
// facets in the mid-ground. There is no setting that is right, because the
// quantity that matters is not distance, it is how much SCREEN a cell covers.
//
// On the screen the answer is trivial and uniform: every cell is the same few
// pixels across wherever it lands, so detail is always where the pixels are.
//
// Two known costs, both handled: a ray near the horizon meets the plane at a
// huge distance (clamped to the far plane), and displacing a vertex moves it
// off the screen point it was built for, so the grid is drawn oversized and
// the edges fall outside the view.
float3 screen_ray(float2 ndc) {
    return normalize(cam_fwd
                   + cam_right * (ndc.x * tan_half_fov * aspect)
                   + cam_up    * (ndc.y * tan_half_fov));
}

VSOut vs_main(uint vid : SV_VertexID) {
    VSOut o;

    const uint n = uint(grid_cells) + 1u;
    const uint ix = vid % n;
    const uint iz = vid / n;

    // 1.25 rather than 1.0: displacement pushes vertices sideways by up to the
    // Gerstner horizontal throw, and a grid that stopped exactly at the screen
    // edge would tear away from it.
    const float over = 1.25f;
    const float2 ndc = (float2(float(ix), float(iz)) / grid_cells * 2.0f - 1.0f) * over;

    const float3 dir = screen_ray(ndc);

    // The plane the grid is laid on decides only WHERE vertices sample the
    // sea; their heights come from ww_surface below. Still sea level is the
    // right plane for any camera above every trough, and a swimmer is not
    // one: an eye floating in a trough is above the water around it and
    // BELOW sea level, so every downward ray missed the plane, went to the
    // far plane, and the water nearest the swimmer was never drawn. --swim at
    // a storm photographed the seabed through nothing, with the swell hanging
    // overhead like a ceiling.
    //
    // So when the eye is above the water directly under it, lay the grid
    // half a metre beneath the eye if that is lower. Any camera more than
    // half a metre above sea level gets exactly the old plane. cam_water is
    // the C++ ww_height_at, the function physics floats the swimmer on.
    // A SUBMERGED camera keeps sea level and the looking-up branch below.
    float plane_y = sea_level;
    if (cam_pos.y >= cam_water) plane_y = min(sea_level, cam_pos.y - 0.5f);

    // Where this ray meets the sea. Rays going up, or so near parallel that
    // the intersection is past the far plane, are pinned to the far plane so
    // the grid still closes at the horizon instead of flying to infinity.
    const float denom = dir.y;
    float tray = far_plane;
    if (denom < -0.0005f) {
        tray = (plane_y - cam_pos.y) / denom;
        if (tray < 0.0f || tray > far_plane) tray = far_plane;
    } else if (denom > 0.0005f && cam_pos.y < plane_y) {
        // Looking UP from under the surface: the same plane, the other side.
        tray = (plane_y - cam_pos.y) / denom;
        if (tray < 0.0f || tray > far_plane) tray = far_plane;
    }

    const float3 hit = cam_pos + dir * tray;
    const float2 rest = float2(hit.x, hit.z);
    const float  d = tray;

    // How much sea one cell covers, which is what decides the shortest wave
    // this grid can carry. One cell subtends a fixed ANGLE, so its footprint
    // grows with distance and again with how flat the ray strikes the plane --
    // the second term is why the sea goes smooth toward the horizon without
    // any distance curve being chosen by hand.
    // A projected cell is ANISOTROPIC: across the view it is d*ang, along the
    // view it is that divided by how flatly the ray strikes the sea, which
    // near eye level is a factor of a hundred. Taking the long axis alone said
    // 6.4 m at fifty metres out and cut every wave but the swell -- a storm
    // with no chop in it. The geometric mean is the cell's effective size and
    // keeps the short waves out to a sensible distance without letting them
    // alias along the stretched axis.
    const float ang = (2.0f * over * tan_half_fov) / grid_cells;
    const float lateral = d * ang;
    const float along = lateral / max(abs(denom), 0.02f);
    const float spacing = sqrt(max(lateral * along, 1e-8f));
    const float minwl = 6.0f * spacing;

    // Nothing left to fade by distance: the spacing term already did it. This
    // only stops the far plane ring itself from heaving.
    const float fade = saturate(1.0f - (d - far_plane * 0.5f) / (far_plane * 0.5f));

    // The terrain, from the same field the mesher uses, under the REST point:
    // the depth tames the waves (water_wave.h), so it has to be known before
    // the displacement. Positive means there is water here; negative means
    // this vertex is inland, and the depth test against the terrain already
    // drawn is what removes it. Read at the rest point rather than where the
    // water is thrown to, a few metres off: on a beach that is centimetres.
    const float r = sqrt(rest.x * rest.x + rest.y * rest.y) / w_radius;
    const float ground = real_terrain_height(rest.x, rest.y, r, 0.0f);
    const float depth = sea_level - ground;

    float3 nrm;
    const float3 p = ww_surface(rest.x, rest.y, time, frame_sea(), depth, fade, minwl, nrm);
    const float3 world = float3(p.x, sea_level + p.y, p.z);

    o.world = world;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.nrm = nrm;
    o.depth = depth;
    o.dist = d;
    o.fade = fade;
    o.minwl = minwl;
    const float3 loc = ww_local(frame_sea(), depth, fade, minwl);
    o.hs = loc.z / max(fade, 1e-4f);
    o.cap_q = loc.xy;
    return o;
}

// Debug views, so that "the sea looks wrong" can be turned into a number
// instead of an argument. 0 is the real shading. Returned with alpha 0, the
// post chain's debug mark (post.hlsl): ps_final shows the colour as it is,
// unexposed. They used to go through the exposure and came out solid white.
float4 debug_view(VSOut i, float3 n, float foam, float ssr_conf) {
    if (debug_mode < 0.5f) return float4(0, 0, 0, 1);
    if (debug_mode < 1.5f) {           // depth, 0..50 m
        return float4(saturate(i.depth / 50.0f).xxx, 0.0f);
    }
    if (debug_mode < 2.5f) {           // foam coverage
        return float4(foam, foam * 0.5f, 0.0f, 0.0f);
    }
    if (debug_mode < 3.5f) {           // normal
        return float4(n * 0.5f + 0.5f, 0.0f);
    }
    if (debug_mode < 4.5f) {           // displacement, +/- 4 m over the full range
        return float4(saturate((i.world.y - sea_level) * 0.125f + 0.5f).xxx, 0.0f);
    }
    if (debug_mode < 5.5f) {           // the significant height the LOD and depth let through, 0..8 m
        return float4(saturate(i.hs * 0.125f).xxx, 0.0f);
    }
    // 6: where the reflection came from -- green the screen, red the sky.
    return float4(1.0f - ssr_conf, ssr_conf, 0.0f, 0.0f);
}

// Value noise for the shore's foam.
float whash(float2 p) {
    p = frac(p * float2(0.1031f, 0.1030f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}
float wnoise(float2 p) {
    const float2 i0 = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(whash(i0), whash(i0 + float2(1, 0)), u.x),
                lerp(whash(i0 + float2(0, 1)), whash(i0 + float2(1, 1)), u.x), u.y);
}

float ign(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// View distance from a stored depth: D3D's 0..1 with near and far planes.
float linear_depth(float z) {
    return near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
}

float2 uv_of(float4 c) {
    const float2 ndc = c.xy / c.w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// Screen-space reflection: march the reflected ray through the scene's depth
// (the sea is not in it -- the copy is from before the sea drew) and return
// the lit scene where it first passes behind a surface by less than that
// surface could be thick. (rgb, confidence); confidence 0 is "the sky".
//
// World space, with steps growing 1.35x from a third of a metre: a calm sea
// reflects a beach ten metres off and a mountain four kilometres off, and a
// fixed step in screen space spends its budget on neither. 28 steps reach
// ~3.8 km (40 at 1.2x reached as far and cost 0.5 ms at 1440p). The start is jittered per pixel and frame, so TAA integrates the
// steps' gaps; a binary search then pins the crossing.
float4 ssr(float3 P, float3 R, float2 px) {
    if (ssr_on < 0.5f) return float4(0, 0, 0, 0);
    const float j = ign(px + frame * 5.588238f);
    float step = 0.3f;
    float t = step * (0.5f + j);
    float prev_t = 0.0f;
    [loop] for (int s = 0; s < 28; ++s) {
        const float4 c = mul(float4(P + R * t, 1.0f), view_proj);
        if (c.w <= near_plane) break;
        const float2 uv = uv_of(c);
        if (any(uv < 0.0f) || any(uv > 1.0f)) break;
        const float zs = g_depth_in.SampleLevel(g_point, uv, 0);
        if (zs < 1.0f) {
            const float diff = c.w - linear_depth(zs);
            // Behind a surface by less than its likely thickness: a hit. By
            // more, the ray passed behind something (a trunk, a ridge) and
            // may yet come out the far side; keep going.
            if (diff > 0.0f && diff < 0.5f + 2.0f * step + 0.01f * c.w) {
                float lo = prev_t, hi = t;
                [unroll] for (int b = 0; b < 6; ++b) {
                    const float mid = 0.5f * (lo + hi);
                    const float4 cm = mul(float4(P + R * mid, 1.0f), view_proj);
                    const float zm = g_depth_in.SampleLevel(g_point, uv_of(cm), 0);
                    if (zm < 1.0f && cm.w > linear_depth(zm)) hi = mid; else lo = mid;
                }
                const float4 ch = mul(float4(P + R * hi, 1.0f), view_proj);
                const float2 huv = uv_of(ch);
                // Where the ray crosses into "behind", it must be AT the
                // surface. A ray that cleared a hilltop from the water but
                // runs behind that hill as the CAMERA sees it crosses at the
                // silhouette, metres or hundreds of metres behind it: the
                // coarse far steps took those for hits, and every distant
                // headland's reflection reached far down the sea in a slab.
                const float zh = g_depth_in.SampleLevel(g_point, huv, 0);
                if (zh >= 1.0f || ch.w - linear_depth(zh) > 2.0f + 0.05f * ch.w) {
                    prev_t = t;
                    step *= 1.35f;
                    t += step;
                    continue;
                }
                // Fade at the screen's edges, where the next pixel out would
                // have had nothing to show.
                const float2 e = min(huv, 1.0f - huv);
                const float conf = saturate(min(e.x, e.y) * 12.0f);
                return float4(g_scene_in.SampleLevel(g_linear, huv, 0).rgb, conf);
            }
        }
        prev_t = t;
        step *= 1.35f;
        t += step;
    }
    return float4(0, 0, 0, 0);
}

// The sky along a reflected direction R, as the viewer sees the sky: the
// composited scene where R lands on screen on a pixel of sky (the mist and
// the atmosphere already in it, exactly). Off screen or onto land that SSR
// did not take, the sky table under the mist of the nearest on-screen column
// to infinity -- never the water pixel's own column, which runs down through
// shadowed mist under the sea and came out darker than the horizon above it.
float3 sky_reflection(float3 R) {
    const float4 c = mul(float4(cam_pos + R * 1e4f, 1.0f), view_proj);
    const float2 raw = c.w > 0.0f ? uv_of(c) : float2(0.5f, 0.0f);
    const float2 ruv = clamp(raw, inv_size * 0.5f, 1.0f - inv_size * 0.5f);
    const bool on = c.w > 0.0f && all(raw == ruv);
    if (on && g_depth_in.SampleLevel(g_point, ruv, 0) >= 1.0f) return g_scene_in.SampleLevel(g_linear, ruv, 0).rgb;
    const float4 m = mist(ruv, 1e9f);
    return sky_view_at(R) * m.a + m.rgb;
}

float4 ps_main(VSOut i) : SV_Target0 {
    // Inland. The terrain in front of this already won the depth test almost
    // everywhere, but the shoreline is where the two surfaces meet at a
    // grazing angle and a vertex can land a little the wrong side of it.
    clip(i.depth + 0.02f);

    // The normal is recomputed HERE rather than interpolated from the
    // vertices. A grid cell in the mid-ground is tens of metres across, so an
    // interpolated normal is flat across the whole of it and the sea reads as
    // faceted glass however good the wave function is.
    //
    // It uses the pixel's OWN spacing limit, half the vertex's, because a
    // pixel resolves finer detail than the cell it sits in -- that is exactly
    // the detail the geometry could not carry.
    const WwSea sea = frame_sea();
    float3 pn;
    ww_surface_with(i.world.x, i.world.z, time, sea, i.depth, i.fade,
                    max(i.minwl * 0.5f, 0.35f), i.cap_q.x, i.cap_q.y, pn);
    float3 gn = normalize(i.nrm);
    float3 n = normalize(lerp(pn, gn, 0.25f));
    const float3 v = normalize(cam_pos - i.world);
    const float3 l = normalize(sun_dir);
    const float2 px = i.clip.xy;
    const float2 uv = px * inv_size;
    const float dist = length(i.world - cam_pos);
    const float vz = max(dot(i.world - cam_pos, cam_fwd), near_plane);
    // Seen from below -- a swimmer looking up -- the surface faces away.
    const bool below = dot(v, gn) < 0.0f;
    if (below) { n = -n; gn = -gn; }

    // What lights the water's body and its foam: the sun on level water and
    // the sky, as on level ground.
    const float diffuse = 0.25f + 0.75f * saturate(dot(n, l));
    const float3 light_in = (kSunLux * max(kSunDir.y, 0.0f) * diffuse + kPi * kSkyMean) / kPi;
    // What deep water sends back: the light scattered up out of the body.
    const float3 deep_in = deep_colour * light_in;

    // --- refraction: the seabed through the water ------------------------
    // The view ray's length under the surface is the scene's depth behind
    // the pixel less the water's own, along the ray (view depths scale with
    // the ray). The seabed is shifted on screen by the normal's tilt, more
    // the more water there is to bend through, and a shift that lands on
    // something IN FRONT of the water (a rock standing out of it) is refused:
    // that is the classic leak of the foreground into the water.
    float zb = g_depth_in[int2(px)];
    float under_len = zb < 1.0f ? max(linear_depth(zb) - vz, 0.0f) * dist / vz : 1e6f;
    const float2 tilt = float2(dot(n, cam_right), -dot(n, cam_up));
    float2 ruv = uv + tilt * (refraction * saturate(under_len / 3.0f));
    {
        const float zr = g_depth_in.SampleLevel(g_point, ruv, 0);
        if (zr < 1.0f && linear_depth(zr) < vz) {
            ruv = uv;
        } else {
            zb = zr;
            under_len = zb < 1.0f ? max(linear_depth(zb) - vz, 0.0f) * dist / vz : 1e6f;
        }
    }
    const float3 behind = g_scene_in.SampleLevel(g_linear, ruv, 0).rgb;
    // Light reaches the seabed through the water above it, then comes back
    // along the view ray: both legs attenuate. Beer-Lambert per channel --
    // red goes first, which is the whole colour of shallow water.
    const float3 seabed_p = cam_pos - v * (dist + min(under_len, 1e4f));
    const float down = below ? 0.0f : max(sea_level - seabed_p.y, 0.0f);
    // A rough sea is a murky one: breaking waves stir up the bottom and fill
    // the water with bubbles. Without this a gale over sand showed the
    // seabed through every trough as pale blobs.
    const float3 ext = extinction * (1.0f + storm_turbidity * sea_state * sea_state);
    const float3 t_view = exp(-ext * min(under_len, 1e4f));
    const float3 t_down = exp(-ext * min(down, 1e4f));
    const float3 under = behind * (t_view * t_down) + deep_in * (1.0f - t_view);

    // --- reflection -------------------------------------------------------
    // Schlick, with water's real normal-incidence reflectance: the grazing
    // angle going almost fully reflective is most of why water looks wet.
    // The old cap at 0.9 was for a two-colour stand-in sky; the sky is the
    // atmosphere's own now, and the near scene is in it.
    const float ndv = saturate(dot(n, v));
    const float fres = below ? 0.0f : 0.02f + 0.98f * pow(1.0f - ndv, 5.0f);
    // The reflection along the shading normal. One that leaves at or below the
    // horizon -- off the back of a wave -- meets the next wave, not the sky
    // or the shore, and the rougher the sea the higher above the horizon that
    // is still true: `open`. Clamped to the horizon instead, every back slope
    // in a gale reflected the brightest, beigest band of the sky (pale blobs),
    // and the same rays sent to SSR ran along the sea and really did meet the
    // far headland, so its reflection reached far below its mirror image with
    // a vertical edge at its tip.
    const float3 rs = reflect(-v, n);
    const float open = smoothstep(-0.05f, 0.04f + 0.2f * saturate(sea_state), rs.y);
    // The ray takes a calmer normal than the shading: the per-pixel ripples
    // would scatter it into noise TAA cannot settle. Not where the reflection
    // is too faint to matter (looking down into the water near the camera,
    // Fresnel gives it a few percent) or closed off by the next wave.
    float3 rr = reflect(-v, normalize(lerp(n, gn, 0.4f)));
    rr.y = max(rr.y, 0.01f);
    rr = normalize(rr);
    // Wind breaks a reflection up: a breeze leaves the shore's image in
    // pieces and a gale leaves none. Without this, a storm wave seen from a
    // swimmer's trough reflected the beach like polished glass.
    const float calm = saturate(1.2f - 1.2f * sea_state);
    float4 hit = (below || fres < 0.06f || open < 0.01f || calm <= 0.0f) ? float4(0, 0, 0, 0)
                                                                         : ssr(i.world + gn * 0.05f, rr, px);
    hit.a *= calm;
    // The sky as it is seen through the mist, which the composite has already
    // worked out for every pixel of sky on screen: where the reflected
    // direction lands on the screen and finds sky, that pixel IS the
    // reflection (sky_reflection). The sky table alone stayed clear blue under
    // a grey sky, and beside the hazy headlands SSR found the sea broke into
    // hard-edged bands. The eye-to-water stretch of mist is laid over the
    // whole sea below.
    const float4 mf = mist(uv, vz);
    const float3 sky_r = sky_reflection(normalize(float3(rs.x, max(rs.y, 0.01f), rs.z)));
    const float3 refl = lerp(deep_in, lerp(sky_r, hit.rgb, hit.a), open);

    // Sun glitter. The exponent falls as the wind gets up: a calm sea gives
    // one hard highlight, a rough one scatters it into a broad track,
    // because the facets that catch the sun are spread over more angles.
    // Normalised Blinn-Phong, (shine + 8) / 8 pi, so the glint carries the
    // sun's real energy (ADR-004 L1). Not where a reflected surface hides it.
    const float3 h = normalize(l + v);
    // 250 at the rough end, not 60: real glitter is many small sparkles off
    // ripples these normals do not carry, and a broad lobe on smooth wave
    // faces a metre across painted a gale's sunward sea with soft pale blobs.
    const float  shine = lerp(900.0f, 250.0f, saturate(sea_state));
    const float3 spec = below ? float3(0, 0, 0) :
        kSunLux * ((shine + 8.0f) / (8.0f * kPi) * pow(saturate(dot(n, h)), shine) *
                   saturate(dot(n, l))) * f_schlick(float3(0.02f, 0.02f, 0.02f), saturate(dot(v, h))) *
        (1.0f - hit.a);

    // Foam where the water is shallow enough to break, and on the crests once
    // the sea is rough. Both are gated on `fade` so the far field does not
    // sprout foam from a normal that is not being displaced any more.
    // At the shore: a drifting lace that thickens towards the waterline,
    // whole at the edge and in patches by two metres. It was a smooth function
    // of depth, which with the shallows now clear read as a white smear.
    const float shallow = saturate(1.0f - i.depth / 2.2f);
    const float lace = 0.6f * wnoise(i.world.xz * 0.45f + float2(time * 0.13f, time * 0.07f)) +
                       0.4f * wnoise(i.world.xz * 1.7f - float2(time * 0.21f, -time * 0.11f));
    const float shore_foam = smoothstep(1.0f - shallow, 1.25f - shallow, lace) * shallow * i.fade;
    const float wave_h = (i.world.y - sea_level) / max(0.7f * i.hs, 0.001f);
    // The top quarter of the wave, not the top half. Measured from a camera
    // standing in the water: the displacement visible from there runs -3.34 m
    // to +1.21 m against an amplitude of 3.71, because the near crests hide
    // everything taller behind them. A threshold set from the sea's full
    // crest height therefore marks water nobody can see, which is how the
    // first two attempts both produced exactly 2.9% coverage and did not move
    // when the number changed.
    const float crest_foam = saturate((wave_h - 0.25f) * 2.6f) *
                             saturate((sea_state - 0.28f) * 2.4f) * i.fade;
    const float foam = saturate(max(shore_foam, crest_foam));

    const float4 dbg = debug_view(i, n, foam, hit.a);
    if (dbg.a < 0.5f) return dbg;

    float3 col = lerp(under, refl, fres) + spec;
    col = lerp(col, srgb_to_linear(float3(0.86f, 0.90f, 0.93f)) * light_in, foam);
    col = min(col, kSceneMaxNits);

    // The air between the eye and the water, as the composite does for every
    // other surface (haze.hlsli). The seabed behind already carries its own,
    // which is a little too much haze through deep clear water, and the
    // reflection carries the haze to what it hit.
    const float4 ap = aerial(uv, dist);
    col = col * ap.a + ap.rgb * lerp(cam_sky, 1.0f, saturate((dist - 64.0f) / 192.0f));
    col = col * mf.a + mf.rgb;
    return float4(col, 1.0f);
}
