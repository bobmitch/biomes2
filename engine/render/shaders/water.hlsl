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
#include "engine/render/ocean_spectrum.h"

cbuffer WaterConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;    float time;
    float3 sun_dir;    float far_plane;
    float3 cam_fwd;    float aspect;
    float3 cam_right;  float tan_half_fov;
    float3 cam_up;     float pad1;
    float  sea_level;  float sea_state;  float grid_cells;  float extent;
    float  debug_mode; float cam_water; float foam_mean; float pad0;   // foam_mean: the foam pattern's (foam_pattern.h)
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
    // The ripples (ocean_detail.h): on/off, the mean square slope each FFT
    // tile carries, and the choppiness its Jacobian was made with.
    float  detail_on;     float detail_var0;  float detail_var1;  float choppy;
    // The weather's wind (weather.h), as grass and trees get it: direction x,
    // z; mean m/s; gustiness. And the metres the air has moved (wind.hlsli).
    float4 wind_v;
    float2 wind_drift;    float swash_runup;  float clouds_on;   // the swash's highest run up the beach (water_wave.h)
    float4 cloud_shadow;  // ADR-005 C2: the cloud-shadow map's centre x, z, half extent, base altitude (cloud_shadow.hlsli)
    float4 hfog_a;        // the exploration fog (haze.hlsli), as post.hlsl
    float4 hfog_b;
    float4 hfog_c;
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
// The ripples' two tiles (ocean_fft.hlsl): slope x, slope z, J - 1, foam.
Texture2DArray<float4> g_detail : register(t5);
SamplerState g_wrap : register(s3);
Texture2D<float> g_cloud_shadow : register(t6);   // clouds.hlsl cs_shadow
// Foam's bubbles (foam_pattern.h): R b (0 a hole's centre, 1 the film), GB
// its gradient / 2.5 biased to 0.5, A b / 2; 16 cells a tile, mip-mapped.
Texture2D<float4> g_foam : register(t7);
#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/atmosphere.h"
#include "engine/render/shaders/haze.hlsli"
#include "engine/render/shaders/wind.hlsli"
#include "engine/render/shaders/cloud_shadow.hlsli"

WwSwash frame_swash() {
    WwSwash s;
    [unroll] for (int i = 0; i < WW_SWASH_TRAINS; ++i) s.amp[i] = wave_amp[i >> 2][i & 3];
    s.runup = swash_runup;
    return s;
}

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
    float  gen    : TEXCOORD8;   // the GENERATED column's water here (sea level less real_terrain_height; negative on land)
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
    // And the first kUnderRows rows reach far BELOW the screen, coarsely
    // (to ndc -4): on a beach the sheet is lifted up to a metre and a half
    // over sea level (the swash and its 12 cm over the sand), and a lifted
    // vertex near a low camera moves UP the screen -- at 1.2 m over the swash
    // the grid's own bottom row came into view, a straight edge across the
    // water. Only for an eye over the water: under it, rays down meet nothing.
    const float over = 1.25f;
    const uint kUnderRows = 12u;
    float ndc_y = (float(iz) / grid_cells * 2.0f - 1.0f) * over;
    if (cam_pos.y >= cam_water)
        ndc_y = iz < kUnderRows ? lerp(-4.0f, -over, float(iz) / float(kUnderRows))
                                : lerp(-over, over, float(iz - kUnderRows) / (grid_cells - float(kUnderRows)));
    const float2 ndc = float2((float(ix) / grid_cells * 2.0f - 1.0f) * over, ndc_y);

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
    const float ang = (2.0f * over * tan_half_fov) / (grid_cells - float(kUnderRows));
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
    const float depth_gen = sea_level - ground;
    // The sea is two layers. The BASE is the same everywhere -- the sea, a
    // dug pit, a tunnel, a channel: flat at sea level, with the wind's
    // ripples in the pixel shader (normals, not height). The TOP is the deep
    // sea's trains and swell, weighted by the GENERATED depth: none on land,
    // rising over the shelf, full in deep water, under the breaking cap
    // (water_wave.h: ww_depth_factor, WW_BREAK_HS); storms scale this layer
    // only. Where the sheet is drawn is the depth test's business alone:
    // terrain over sea level hides it, so any air under sea level shows water
    // at any distance. Inland pits and caves are calm by construction, a
    // channel dug across a beach gets almost nothing (its generated depth is
    // shallow), and a dredged harbour stays calm.
    float3 nrm;
    const float3 p = ww_surface(rest.x, rest.y, time, frame_sea(), depth_gen, fade, minwl, nrm);
    // The swash (water_wave.h): within a metre and a half of the still-water
    // line the sheet rises and falls with each arriving set, so the water
    // runs up the beach and back; the terrain in front hides it where the
    // beach is higher. Only on the LOW beach (ww_swash_band): an inland
    // pit's column carried the run-up's height too. Where the ground behind a
    // pixel is dug the pixel shader takes it away again.
    const float reach = ww_swash_reach(depth_gen) * ww_swash_band(depth_gen, swash_runup);
    const float swash = reach > 0.0f ? ww_swash(rest.x, rest.y, time, frame_swash()) * reach : 0.0f;
    float y = p.y + swash;
    // A little past the edge too, 12 cm over the generated ground out to
    // 30 cm beyond it: the edge is made per pixel (edge_fade, in the pixel
    // shader), which can only fade away water that was drawn. Where it is wet
    // it never sinks below the bed either: a storm trough over half a metre of
    // water fell under the seabed and bared it.
    if (reach > 0.0f && depth_gen + swash > -0.3f) y = max(y, 0.12f - depth_gen);
    const float3 world = float3(p.x, sea_level + y, p.z);

    o.world = world;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.nrm = nrm;
    o.depth = depth_gen + swash;   // the water over the generated bed here, for the waves' normal
    o.dist = d;
    o.fade = fade;
    o.minwl = minwl;
    const float3 loc = ww_local(frame_sea(), depth_gen, fade, minwl);
    o.hs = loc.z / max(fade, 1e-4f);
    o.cap_q = loc.xy;
    o.gen = depth_gen;
    return o;
}

// Debug views, so that "the sea looks wrong" can be turned into a number
// instead of an argument. 0 is the real shading. Returned with alpha 0, the
// post chain's debug mark (post.hlsl): ps_final shows the colour as it is,
// unexposed. They used to go through the exposure and came out solid white.
float4 debug_view(VSOut i, float3 n, float foam, float ssr_conf) {
    if (debug_mode < 0.5f || debug_mode > 6.5f) return float4(0, 0, 0, 1);
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
    const float4 hf = height_fog(cam_pos, R, 1e6f);
    return (sky_view_at(R) * hf.a + hf.rgb) * m.a + m.rgb;
}

float4 ps_main(VSOut i) : SV_Target0 {
    // Where water is, is the depth test's: the sheet lies at sea level
    // everywhere and terrain over sea level hides it. What a pixel still needs
    // is the GROUND behind it (the depth copy): it makes the edge, decides
    // whether the water here is dug, and sets the contact foam.
    const float2 px = i.clip.xy;
    const float2 uv = px * inv_size;
    const float3 v = normalize(cam_pos - i.world);
    const float zb0 = g_depth_in[int2(px)];
    float3 P = i.world;   // the point shaded
    float dist = length(P - cam_pos);
    float vz = max(dot(P - cam_pos, cam_fwd), near_plane);
    float under_len = zb0 < 1.0f ? max(linear_depth(zb0) - vz, 0.0f) * dist / vz : 1e6f;
    const float3 gp = cam_pos - v * (dist + min(under_len, 1e4f));
    // DUG: the ground behind lies well under the GENERATED ground here (a pit,
    // a channel, a tunnel's mouth). There the beach's swash and its 12 cm lift
    // do not apply -- the water is calm at sea level -- so a pixel of sheet
    // they raised is shaded where its ray meets sea level instead, and the
    // ground behind decides whether any water shows (edge_fade).
    // Against the generated ground AT the ground point, not under the surface
    // point: at a grazing view the ground behind a pixel lies metres along
    // the ray, and a seabed falling away offshore read as dug -- the open
    // sea's crests were flattened to sea level in a slab. Extrapolated from
    // the surface point along the generated depth's own gradient over this
    // triangle (the interpolants' screen derivatives), with a tolerance
    // growing with the distance.
    float2 ggen = float2(0.0f, 0.0f);
    {
        const float2 ex = ddx(i.world.xz), ey = ddy(i.world.xz);
        const float gx = ddx(i.gen), gy = ddy(i.gen);
        const float det = ex.x * ey.y - ex.y * ey.x;
        if (abs(det) > 1e-12f) ggen = float2(gx * ey.y - ex.y * gy, ex.x * gy - ey.x * gx) / det;
        const float gl = length(ggen);
        if (gl > 2.0f) ggen *= 2.0f / gl;
    }
    float2 hv = gp.xz - i.world.xz;
    const float hd = length(hv);
    if (hd > 20.0f) hv *= 20.0f / hd;
    const float gen_ground = sea_level - (i.gen + dot(ggen, hv));
    const float dug = zb0 < 1.0f ? saturate((gen_ground - gp.y - 0.3f - 0.03f * hd) / 0.3f) : 0.0f;
    if (dug > 0.0f && P.y > sea_level + 1e-3f && v.y > 1e-3f) {
        P = lerp(P, cam_pos - v * ((cam_pos.y - sea_level) / v.y), dug);
        dist = length(P - cam_pos);
        vz = max(dot(P - cam_pos, cam_fwd), near_plane);
        under_len = zb0 < 1.0f ? max(linear_depth(zb0) - vz, 0.0f) * dist / vz : 1e6f;
    }
    // The water over the ground behind, VERTICALLY (to the swash's surface on
    // the beach): the edge fades over its last 8 cm, and the contact foam lies
    // in its first 10-30 cm.
    const float gdepth = sea_level - gp.y;
    float thick = 1e4f;
    if (zb0 < 1.0f) {
        thick = gdepth;
        if (gdepth < 1.6f)
            thick += ww_swash(gp.x, gp.z, time, frame_swash()) * ww_swash_reach(gdepth) *
                     ww_swash_band(i.gen, swash_runup) * (1.0f - dug);
    }

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
    float jac_g;
    ww_surface_with(P.x, P.z, time, sea, i.depth, i.fade,
                    max(i.minwl * 0.5f, 0.35f), i.cap_q.x, i.cap_q.y, pn, jac_g);
    float3 gn = normalize(i.nrm);
    const float3 base = normalize(lerp(pn, gn, 0.25f));
    const float3 l = normalize(sun_dir);

    // --- the ripples (ocean_detail.h) --------------------------------------
    // Each tile's band is kept only while a pixel can resolve it: the 31 m
    // tile's shortest ripple is 0.6 m and the 7.7 m tile's 6 cm, so they fade
    // as the pixel's footprint on the water (stretched along the view at a
    // grazing angle, as the grid's spacing is) reaches a quarter and then
    // half of that. What fades is not lost: its slope widens the sun's glint
    // below, which is the glitter path a rough sea shows far off.
    const float ang = 2.0f * tan_half_fov * inv_size.y;
    const float lateral = vz * ang;
    const float fp = sqrt(lateral * lateral / max(abs(dot(v, gn)), 0.02f));
    const float f0 = detail_on * saturate(2.0f - fp / 0.15f);
    const float f1 = detail_on * saturate(2.0f - fp / 0.015f);
    const float4 d0 = f0 > 0.0f ? g_detail.SampleLevel(g_wrap, float3(P.xz / od_tile[0], 0.0f), 0) : float4(0, 0, 0, 0);
    const float4 d1 = f1 > 0.0f ? g_detail.SampleLevel(g_wrap, float3(P.xz / od_tile[1], 1.0f), 0) : float4(0, 0, 0, 0);
    // --- the wind on the water --------------------------------------------
    // Ripples answer the wind in seconds, so the gust field the grass and
    // trees bend in (wind.hlsli: carried by the air, at p - drift) also
    // roughens the water under it: dark patches of ruffled water run across
    // the sea at the wind's speed, and in light air the water between them
    // goes glassy. The ripples' strength follows the local wind as their
    // slope does (Cox-Munk), with a threshold: capillary ripples do not form
    // under ~1.2 m/s and are not fully grown until ~3.8. With a softer one
    // (0.5-2.5) a light air's lulls never went glassy and no patch showed. The 7.7 m tile answers in full, the 31 m
    // tile's longer ripples, which take longer to grow, by 60%. The trains --
    // waves metres long, built over minutes -- do not answer gusts.
    const float u_mean = max(wind_v.z, 0.1f);
    const float u_here = wind_v.z * wind_gust(P.xz, wind_v, wind_drift, time);
    const float e_mean = od_cox_munk(u_mean) * smoothstep(1.2f, 3.8f, u_mean);
    const float e_here = od_cox_munk(u_here) * smoothstep(1.2f, 3.8f, u_here);
    const float gs = clamp(sqrt(e_here / max(e_mean, 1e-5f)), 0.0f, 2.0f);
    const float s1 = gs, s0 = lerp(1.0f, gs, 0.6f);
    const float2 dslope = d0.xy * (f0 * s0) + d1.xy * (f1 * s1);
    // Slopes add: the trains' (from their normal) and the ripples'.
    float3 n = normalize(float3(-(-base.x / base.y + dslope.x), 1.0f, -(-base.z / base.y + dslope.y)));
    const float var_unres = detail_on * (detail_var0 * (1.0f - f0) * s0 * s0 + detail_var1 * (1.0f - f1) * s1 * s1);
    // Seen from below -- a swimmer looking up -- the surface faces away.
    const bool below = dot(v, gn) < 0.0f;
    if (below) { n = -n; gn = -gn; }

    // What lights the water's body and its foam: the sun on level water and
    // the sky, as on level ground.
    // Under the clouds' shadow (ADR-005 C2) the sun leaves the sea's body and
    // its glint, as it leaves the land in the composite: under a storm deck
    // the sea went on sparkling.
    const float cls = clouds_on > 0.5f ? cloud_shadow_at(g_cloud_shadow, g_linear, cloud_shadow, l, P) : 1.0f;
    const float diffuse = 0.25f + 0.75f * saturate(dot(n, l));
    const float3 light_in = (kSunLux * (max(kSunDir.y, 0.0f) * diffuse * cls) + kPi * kSkyMean) / kPi;
    // What deep water sends back: the light scattered up out of the body.
    const float3 deep_in = deep_colour * light_in;

    // --- refraction: the seabed through the water ------------------------
    // The view ray's length under the surface is the scene's depth behind
    // the pixel less the water's own, along the ray (view depths scale with
    // the ray). The seabed is shifted on screen by the normal's tilt, more
    // the more water there is to bend through, and a shift that lands on
    // something IN FRONT of the water (a rock standing out of it) is refused:
    // that is the classic leak of the foreground into the water.
    float zb = zb0;
    const float under0 = under_len;   // before the refraction's shift, for the foam
    // The water's edge, per pixel, from the GROUND behind this pixel and the
    // swash evaluated right there: water is present where that ground lies
    // under sea level plus the swash, and fades into the scene over its last
    // 8 cm of depth (thick). Both are smooth per pixel, so the edge follows a
    // true contour of the beach. (Two attempts by the sheet's own geometry drew
    // a staircase: on the beach the sheet sits a few centimetres over the
    // ground, nearly coplanar with it, and its height is interpolated across
    // grid cells.)
    float edge_fade = 1.0f;
    if (zb0 < 1.0f) {
        edge_fade = saturate(thick / 0.08f);
        // In dug water the edge is a dug WALL, steep, and the water meets it
        // along the mesh's half-metre facets -- the real waterline, toothed
        // where a wall stands near vertical at sea level. There the last 1.2 m
        // of the view ray under water fades out, so the wall shows through its
        // own first few centimetres of water and the teeth dissolve into it.
        // Only in dug water: from above, a natural beach would clear over its
        // shallows.
        edge_fade = min(edge_fade, lerp(1.0f, smoothstep(0.0f, 1.2f, under_len), dug));
    }
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
    // Roughness the pixel cannot resolve still does what ripples do to a
    // reflection: a rough patch at a grazing view shadows and masks itself and
    // reflects less, and what it does reflect comes from higher, darker sky.
    // That is how a gust's ruffled patch reads as dark against glassy water a
    // kilometre off, where its ripples are long since below a pixel. The rms
    // slope raises the angle Fresnel sees and lifts the reflected ray.
    const float rough_unres = sqrt(var_unres);
    const float ndv = saturate(dot(n, v) + 0.8f * rough_unres);
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
    // Whether the next wave closes the view is a question for the WAVES, not
    // the ripples on them: with the ripples' tilt in it, calm water at a
    // grazing angle was pocked with dark spots.
    const float3 rs_base = reflect(-v, below ? -base : base);
    const float open = smoothstep(-0.05f, 0.04f + 0.2f * saturate(sea_state), rs_base.y);
    // The ray takes a calmer normal than the shading: the per-pixel ripples
    // would scatter it into noise TAA cannot settle. Not where the reflection
    // is too faint to matter (looking down into the water near the camera,
    // Fresnel gives it a few percent) or closed off by the next wave.
    float3 rr = reflect(-v, normalize(lerp(below ? -base : base, gn, 0.4f)));
    rr.y = max(rr.y, 0.01f);
    rr = normalize(rr);
    // Wind breaks a reflection up: a breeze leaves the shore's image in
    // pieces and a gale leaves none. Without this, a storm wave seen from a
    // swimmer's trough reflected the beach like polished glass.
    const float calm = saturate(1.2f - 1.2f * u_here / 20.0f);   // the local wind: a reflection survives in a lull
    float4 hit = (below || fres < 0.06f || open < 0.01f || calm <= 0.0f) ? float4(0, 0, 0, 0)
                                                                         : ssr(P + gn * 0.05f, rr, px);
    hit.a *= calm;
    // The sky as it is seen through the mist, which the composite has already
    // worked out for every pixel of sky on screen: where the reflected
    // direction lands on the screen and finds sky, that pixel IS the
    // reflection (sky_reflection). The sky table alone stayed clear blue under
    // a grey sky, and beside the hazy headlands SSR found the sea broke into
    // hard-edged bands. The eye-to-water stretch of mist is laid over the
    // whole sea below.
    const float4 mf = mist(uv, vz);
    const float3 sky_r = sky_reflection(normalize(float3(rs.x, max(rs.y, 0.01f) + 1.0f * rough_unres, rs.z)));
    const float3 refl = lerp(deep_in, lerp(sky_r, hit.rgb, hit.a), open);

    // Sun glitter. Near the camera the ripples are in the normal and every
    // facet that catches the sun is its own hard sparkle; further off the
    // ripples are too small to resolve and their slope goes into the lobe
    // instead (Beckmann: mean square slope m^2, Blinn-Phong exponent about
    // 2 / m^2 - 2), so a rough sea's sparkles merge into a broad track. The
    // floor is ~900, a calm sea's single hard highlight. It used to be a
    // hand-set lerp on the wind, 900 to 60 and then 250: broad lobes on
    // smooth metre-wide faces were soft pale blobs, and narrow ones on a
    // distant rough sea had no track at all.
    // Normalised Blinn-Phong, (shine + 8) / 8 pi, so the glint carries the
    // sun's real energy (ADR-004 L1). Not where a reflected surface hides it.
    const float3 h = normalize(l + v);
    const float  shine = 2.0f / (var_unres + 0.0022f) - 2.0f;
    const float3 spec = below ? float3(0, 0, 0) :
        kSunLux * ((shine + 8.0f) / (8.0f * kPi) * pow(saturate(dot(n, h)), shine) *
                   saturate(dot(n, l))) * f_schlick(float3(0.02f, 0.02f, 0.02f), saturate(dot(v, h))) *
        ((1.0f - hit.a) * cls);

    // Contact foam, one rule for every shore: where the water over the ground
    // behind the pixel is THIN, VERTICALLY (thick: 10-30 cm), broken up by a
    // drifting lace -- at a beach's edge, round a rock or a trunk standing in
    // the water, at the foot of a dug wall. Faint scum on calm water, surf
    // where the natural sea reaches (sea_w: the generated column is sea or
    // its beach, and not dug) as the weather roughens. Faded where the ray
    // under water is long, which is an occluder (an overhang's underside)
    // rather than a shallow. It replaced a shore foam keyed to the vertices'
    // generated depth, which could not see a dug edge.
    const float lace = 0.6f * wnoise(P.xz * 0.45f + float2(time * 0.13f, time * 0.07f)) +
                       0.4f * wnoise(P.xz * 1.7f - float2(time * 0.21f, -time * 0.11f));
    // At most 0.7 of the lace's threshold, so even the thinnest water keeps
    // holes in it: whole, the swash's thin sheet on a gentle beach drew a
    // solid white ribbon metres wide at a grazing view.
    // Where the natural sea reaches, the band widens with the weather too, to
    // ~0.8 m of water in a gale: surf breaks over a wider strip than calm
    // water's scum.
    const float sea_w = saturate(i.gen + 1.0f) * (1.0f - dug);
    const float rough_w = sea_w * saturate(2.0f * sea_state);
    const float contact = 0.7f * (1.0f - smoothstep(0.1f, 0.3f + 0.5f * rough_w, thick)) *
                          (1.0f - smoothstep(2.0f, 6.0f, under0));
    const float surf = lerp(0.15f, 1.0f, sea_w * saturate(0.35f + 2.0f * sea_state));
    // Whitecaps where the surface FOLDS: the Jacobian of the trains' throw
    // (ww_surface_with) and the ripples' (ocean_fft.hlsl) together, 1 on
    // still water and 0 where a crest comes to a point and breaks. The wind
    // sets how far short of folding a crest may break -- none under ~4 m/s,
    // Beaufort 4 -- and the ripples break the band up, so foam sits on the
    // sharpest crests in pieces rather than on every crest's top quarter.
    // The 31 m tile also keeps the foam its compressions made, decaying over
    // a couple of seconds IN THE TILE, so it trails behind them as streaks;
    // in a strong wind that is laid over the whole sea, as windrows are.
    // The height test this replaces marked every crest's top quarter alike.
    //
    // The threshold is MEASURED: --sea-report's section 3b tabulates the
    // trains' J at the share of the sea Monahan's fit says is whitecap
    // (3.84e-6 U^3.41: 1% at 10 m/s, 10% at 20), 0.63 at 4 m/s to 0.84 at
    // 20, and this is a quadratic through it. The ripples' J only frays the
    // edges (a third of it): at full weight they made a quarter of a gale's
    // sea white with speckle, where real whitecaps cover a tenth.
    // Half the local wind's say: a gust over a developed sea breaks more of it.
    const float U = min(lerp(sea_state * 20.0f, u_here, 0.5f), 22.0f);
    const float jac = jac_g + 0.35f * (d0.z * f0 + d1.z * f1);
    const float jt = 0.527f + 0.0279f * U - 0.000623f * U * U;
    const float breaking = saturate((jt - jac) / 0.06f) * saturate((U - 3.0f) / 3.0f) * i.fade;
    const float trail = d0.w * f0 * saturate((U - 10.0f) / 10.0f);
    // The deep sea's layer only: weighted by the generated depth's first metre
    // and never in dug water. The ripples' folding and the wind's streaks are
    // in the base layer everywhere, and unweighted they laid gale whitecaps on
    // a pit dug into a beach.
    const float crest_raw = saturate(breaking * (0.55f + 0.9f * d0.w) + 0.25f * trail) * saturate(i.gen) * (1.0f - dug);

    // --- foam's fine structure, by the pixel's footprint ----------------------
    // Up close sea foam is a froth of holes of many sizes in a white film, and
    // the lace above has nothing under half a metre, so near the camera it read
    // as a smooth smear. Three octaves of BUBBLES (foam_pattern.h: a baked,
    // tileable texture of holes of varied radius), 0.5 m, 12 cm and 3 cm cells,
    // each turned its own way (no tile lines up with another) and drifting a
    // little faster than the one above, so the froth shears as it moves. Each
    // is whole while its cell spans 8 pixels of water (fp, stretched at a
    // grazing view) and gone at 4, where it is replaced by the pattern's MEAN
    // -- the mip chain filters towards the same mean -- so the foam's coverage,
    // and the far look, do not move: from a distance the foam is exactly the
    // lace, and walking up to it the holes open octave by octave with no switch
    // to see. The threshold's softness follows the same footprint: the lace's
    // 0.25 far off, down to what keeps the finest visible octave antialiased
    // (its change across a pixel) up close, so edges sharpen as they resolve.
    // A texture, not the pattern evaluated here: the procedural version
    // searched nine points an octave and cost +0.4 ms of the sea pass at 1440p
    // where foam filled the view.
    float detail = 0.0f, near_w = 0.0f, aa = 0.0f;
    float2 relief = float2(0.0f, 0.0f);
    // Only where foam can come of it: crests, or a lace that the detail's most
    // (0.4 over its mean) could lift over the shore's threshold. The gradients
    // are taken outside the branch; the reads take them explicitly.
    const float2 dpx = ddx(P.xz), dpy = ddy(P.xz);
    if (crest_raw > 0.002f || (contact > 0.002f && lace + 0.4f > 1.0f - contact)) {
        [unroll] for (int k = 0; k < 3; ++k) {
            const float cs = k == 0 ? 0.5f : (k == 1 ? 0.12f : 0.03f);
            const float amp = k == 0 ? 0.35f : (k == 1 ? 0.25f : 0.15f);
            const float ang_k = k == 0 ? 0.5f : (k == 1 ? 2.1f : 4.0f);
            const float ca = cos(ang_k), sa = sin(ang_k);
            const float2x2 rot = float2x2(ca, -sa, sa, ca);
            const float2 drift = float2(0.29f, 0.16f) * (time * (1.0f + 0.2f * float(k)));
            const float sc = 1.0f / (cs * 16.0f);   // 16 cells a tile
            const float2 uv = mul(rot, (P.xz - drift) * sc) + float2(0.13f, 0.71f) * float(k);
            const float4 t = g_foam.SampleGrad(g_wrap, uv, mul(rot, dpx * sc), mul(rot, dpy * sc));
            const float w = saturate(cs / max(fp, 1e-5f) * 0.25f - 1.0f);
            if (k == 0) near_w = w;
            detail += amp * w * (t.r - foam_mean);
            // The relief: a hole is a dimple to its rim; the film between holes
            // rises to the ridges where cells meet -- dense foam's honeycomb --
            // for the finest octave only (the coarser ridges shaded as
            // metre-wide facets). Turned back into the world's frame.
            const float hole = smoothstep(0.55f, 0.45f, t.a);
            relief += amp * w * lerp(k == 2 ? 0.35f : 0.0f, 1.0f, hole) * mul((t.gb * 2.0f - 1.0f) * 2.5f, rot);
            aa += amp * w * fp / (0.7f * cs);
        }
    }
    const float soft = lerp(0.25f, clamp(1.5f * aa, 0.03f, 0.25f), near_w);
    // DENSITY: how far over its threshold the foam is. Up close thin foam is
    // a translucent film -- the water shows through it, greyer -- and dense
    // foam an opaque white: a flat white sheet with holes punched in it read
    // as snow. Nothing where the detail has faded, so far foam is as before.
    const float shore_f = lace + detail - (1.0f - contact);
    const float shore_dens = saturate(shore_f / 0.3f);
    const float shore_foam = smoothstep(0.0f, soft, shore_f) * lerp(1.0f, lerp(0.55f, 1.0f, shore_dens), near_w) *
                             contact * surf * i.fade;
    // The crests' foam is an amount, not a threshold of the lace: up close it
    // is eroded by the same bubbles (the more foam, the smaller the holes),
    // cross-faded in by the coarse octave's weight.
    const float crest_f = 0.5f + detail - (1.0f - crest_raw);
    const float crest_dens = saturate(crest_f / 0.3f);
    const float crest_foam = lerp(crest_raw, smoothstep(0.0f, soft, crest_f) * lerp(0.55f, 1.0f, crest_dens), near_w);
    const float foam = saturate(max(shore_foam, crest_foam));
    // How the foam is lit: its bubbles' relief tilts the water's normal (at a
    // slope the same at every scale), and thin foam is darker, lit through
    // itself.
    const float3 nfoam = normalize(n - 0.6f * float3(relief.x, 0.0f, relief.y));
    const float foam_shade = lerp(1.0f, lerp(0.78f, 1.0f, max(shore_dens, crest_dens)), near_w);
    const float3 foam_light = (kSunLux * (max(kSunDir.y, 0.0f) * (0.25f + 0.75f * saturate(dot(nfoam, l))) * cls) +
                               kPi * kSkyMean) / kPi * foam_shade;

    // 7: the folding measure, white at 1 (still), black at 0 (folding).
    // 9: red dug, green the natural sea's reach (sea_w), blue the land side.
    // 10: the contact foam's inputs -- red the water's vertical thickness
    // (0-0.3 m), green the foam, blue the ray under water (0-6 m).
    // 13: the edge's fade, the view ray's length under water over 2 m, dug.
    if (debug_mode > 12.5f && debug_mode < 13.5f) return float4(edge_fade, saturate(under_len / 2.0f), dug, 0.0f);
    if (debug_mode > 9.5f && debug_mode < 10.5f) return float4(saturate(thick / 0.3f), shore_foam, saturate(under0 / 6.0f), 0.0f);
    if (debug_mode > 8.5f && debug_mode < 9.5f) return float4(dug, sea_w, i.gen < 0.0f ? 1.0f : 0.0f, 0.0f);
    if (debug_mode > 7.5f && debug_mode < 13.5f) return float4(saturate(gs * 0.5f).xxx, 0.0f);   // 8: the gusts' ripple strength, grey = mean
    if (debug_mode > 6.5f && debug_mode < 13.5f) return float4(saturate(jac).xxx, 0.0f);
    const float4 dbg = debug_view(i, n, foam, hit.a);
    if (dbg.a < 0.5f) return dbg;

    // --- the surface from below (ADR-006 stage 2) -------------------------
    // Light leaving water for air bends away from the normal, so the whole sky
    // is squeezed into a cone of about 97 degrees overhead -- Snell's window --
    // and outside it the surface is a mirror (total internal reflection) of
    // the water's own dim body. n and gn face down here, towards the eye.
    // Nothing of the AIR is laid over it: the path to it is water, which
    // post.hlsl's cs_underwater lays over the whole view after this. Only for
    // an eye under the water: from above, a pixel that sees a crest's
    // underside keeps the old shading, so no view from the air changes.
    if (below && cam_pos.y < cam_water) {
        // The water's in-scatter just under the surface, as cs_underwater has
        // it (level water's light: deep_in above took the flipped normal).
        const float3 s0 = deep_colour * (kSunLux * (max(kSunDir.y, 0.0f) * (0.25f + 0.75f * saturate(l.y)) * cls) +
                                         kPi * kSkyMean) / kPi;
        // Outside the window the surface is a mirror of the water UNDER it:
        // the seabed, where SSR finds it in the depth copy, through the water
        // along the reflected ray and the sunlight's path down to it; and the
        // water's own glow along that ray (cs_underwater's closed form). A
        // flat deep colour here drew a navy ceiling over bright sand.
        const float3 r = reflect(-v, n);
        const float bed_d = max(i.depth, cam_water - cam_pos.y);   // a pit's generated depth is none
        const float lr = bed_d / max(-r.y, 0.15f);
        const float rup = 1.0f - r.y;
        const float4 bh = ssr(P + gn * 0.05f, r, px);
        const float3 glow_r = s0 * (1.0f - exp(-ext * rup * lr)) / rup;
        // Where SSR finds nothing (the reflected ray leaves the screen, as it
        // does looking steeply up), the seabed's own authored colour, lit as
        // level ground, stands in for the hit (g_arch's last row set is the
        // seabed's material, linear).
        const float3 bed_albedo = g_arch[w_biome_count * kArchRows + 3].rgb;
        const float3 bed_lit = bed_albedo * (kSunLux * (max(kSunDir.y, 0.0f) * cls) + kPi * kSkyMean) / kPi;
        const float3 bed = lerp(bed_lit, bh.rgb, bh.a);
        float3 wcol = glow_r + bed * exp(-ext * (lr + bed_d));
        const float3 t = refract(-v, n, 1.333f);
        if (dot(t, t) > 1e-6f) {
            const float3 tt = normalize(t);
            // Fresnel on the AIR side's angle, as Schlick wants for n1 > n2.
            const float ft = 0.02f + 0.98f * pow(1.0f - saturate(dot(tt, -n)), 5.0f);
            // The sky the refracted ray meets, as the viewer would see it
            // (sky_reflection: the composited sky where it is on screen).
            const float3 sky_t = sky_reflection(float3(tt.x, max(tt.y, 0.01f), tt.z));
            // The sun through the window, spread by the ripples as the glint
            // above is: a dancing bright patch, not a disk.
            const float3 sun_t = kSunLux * ((shine + 2.0f) / (2.0f * kPi) * pow(saturate(dot(tt, l)), shine)) *
                                 (cls * step(0.0f, l.y));
            wcol = lerp(sky_t + sun_t, wcol, ft);
        }
        // Foam is a white raft on the water, lit from above through itself.
        wcol = lerp(wcol, srgb_to_linear(float3(0.86f, 0.90f, 0.93f)) * light_in * 0.35f, foam);
        return float4(min(wcol, kSceneMaxNits), 1.0f);
    }

    float3 col = lerp(under, refl, fres) + spec;
    col = lerp(col, srgb_to_linear(float3(0.86f, 0.90f, 0.93f)) * foam_light, foam);
    col = min(col, kSceneMaxNits);

    // The air between the eye and the water, as the composite does for every
    // other surface (haze.hlsli). The seabed behind already carries its own,
    // which is a little too much haze through deep clear water, and the
    // reflection carries the haze to what it hit.
    const float4 ap = aerial(uv, dist);
    col = col * ap.a + ap.rgb * lerp(cam_sky, 1.0f, saturate((dist - 64.0f) / 192.0f));
    {
        const float4 hf = height_fog(cam_pos, -v, dist);
        col = col * hf.a + hf.rgb;
    }
    col = col * mf.a + mf.rgb;
    // The scene behind, as the composite left it: haze and all, so the fade
    // is seamless.
    col = lerp(g_scene_in[int2(px)].rgb, col, edge_fade);
    return float4(col, 1.0f);
}
