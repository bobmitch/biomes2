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
    float  debug_mode; float3 pad0;
};

struct VSOut {
    float4 clip   : SV_Position;
    float3 world  : TEXCOORD0;
    float3 nrm    : TEXCOORD1;
    float  depth  : TEXCOORD2;   // metres of water under this point
    float  dist   : TEXCOORD3;
    float  fade   : TEXCOORD4;
    float  minwl  : TEXCOORD5;   // shortest wave this spacing can carry
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
    const float  plane_y = sea_level;

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

    float3 nrm;
    const float3 p = ww_surface(rest.x, rest.y, time, sea_state, fade, minwl, nrm);
    const float3 world = float3(p.x, sea_level + p.y, p.z);

    // The terrain, from the same field the mesher uses. Positive means there
    // is water here; negative means this vertex is inland, and the depth test
    // against the terrain already drawn is what removes it.
    const float r = sqrt(world.x * world.x + world.z * world.z) / w_radius;
    const float ground = real_terrain_height(world.x, world.z, r, 0.0f);

    o.world = world;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.nrm = nrm;
    o.depth = sea_level - ground;
    o.dist = d;
    o.fade = fade;
    o.minwl = minwl;
    return o;
}

// Sky, matching the colour the renderer clears to so the horizon joins up.
float3 sky_colour(float3 dir) {
    // The horizon end must not be BRIGHTER than the sky the renderer
    // actually clears to, or the sea near the horizon glows whiter than the
    // sky above it -- which it did, and it reads as a bug even though a
    // grazing reflection genuinely is bright.
    const float3 horizon = float3(0.44f, 0.54f, 0.68f);
    const float3 zenith  = float3(0.20f, 0.34f, 0.60f);
    return lerp(horizon, zenith, saturate(dir.y * 1.4f));
}

// Debug views, so that "the sea looks wrong" can be turned into a number
// instead of an argument. 0 is the real shading.
float4 debug_view(VSOut i, float3 n, float foam) {
    if (debug_mode < 0.5f) return float4(0, 0, 0, 0);
    if (debug_mode < 1.5f) {           // depth, 0..50 m
        return float4(saturate(i.depth / 50.0f).xxx, 1.0f);
    }
    if (debug_mode < 2.5f) {           // foam coverage
        return float4(foam, foam * 0.5f, 0.0f, 1.0f);
    }
    if (debug_mode < 3.5f) {           // normal
        return float4(n * 0.5f + 0.5f, 1.0f);
    }
    if (debug_mode < 4.5f) {           // displacement, +/- 4 m over the full range
        return float4(saturate((i.world.y - sea_level) * 0.125f + 0.5f).xxx, 1.0f);
    }
    // 5: the amplitude the LOD is actually letting through, 0..4 m.
    return float4(saturate(ww_total_amplitude_lod(sea_state, i.minwl) * 0.25f).xxx, 1.0f);
}

float4 ps_main(VSOut i) : SV_Target {
    // Inland. The terrain in front of this already won the depth test almost
    // everywhere, but the shoreline is where the two surfaces meet at a
    // grazing angle and a vertex can land a little the wrong side of it.
    clip(i.depth + 0.02f);

    // The normal is recomputed HERE rather than interpolated from the
    // vertices. A grid cell in the mid-ground is tens of metres across, so an
    // interpolated normal is flat across the whole of it and the sea reads as
    // faceted glass however good the wave function is. Evaluating per pixel
    // costs four sin/cos pairs and makes the surface continuous.
    //
    // It uses the pixel's OWN spacing limit, half the vertex's, because a
    // pixel resolves finer detail than the cell it sits in -- that is exactly
    // the detail the geometry could not carry.
    float3 pn;
    ww_surface(i.world.x, i.world.z, time, sea_state, i.fade,
               max(i.minwl * 0.5f, 0.35f), pn);
    const float3 n = normalize(lerp(pn, normalize(i.nrm), 0.25f));
    const float3 v = normalize(cam_pos - i.world);
    const float3 l = normalize(sun_dir);

    // Deeper water is darker and bluer. The shallow end is what makes a beach
    // read as a beach rather than as a line where blue stops.
    const float3 shallow = float3(0.22f, 0.52f, 0.52f);
    const float3 deep    = float3(0.016f, 0.075f, 0.16f);
    const float  dmix = saturate(i.depth / 22.0f);
    float3 body = lerp(shallow, deep, dmix * dmix);

    // Schlick, with water's real normal-incidence reflectance. The grazing
    // angle going almost fully reflective is most of why water looks wet.
    const float ndv = saturate(dot(n, v));
    // Capped below a perfect mirror. Schlick is right, but what it is
    // reflecting here is a two-colour gradient standing in for a sky, and a
    // flawless mirror of a crude sky looks like polished plastic.
    const float fres = min(0.02f + 0.98f * pow(1.0f - ndv, 5.0f), 0.90f);

    const float3 refl = reflect(-v, n);
    const float3 sky = sky_colour(refl);

    // Sun glitter. The exponent falls as the sea gets rougher: a calm sea
    // gives one hard highlight, a rough one scatters it into a broad track,
    // because the facets that catch the sun are spread over more angles.
    const float3 h = normalize(l + v);
    const float  shine = lerp(900.0f, 60.0f, saturate(sea_state));
    const float  spec = pow(saturate(dot(n, h)), shine) * lerp(1.6f, 0.7f, saturate(sea_state));

    // Foam where the water is shallow enough to break, and on the crests once
    // the sea is rough. Both are gated on `fade` so the far field does not
    // sprout foam from a normal that is not being displaced any more.
    const float shore_foam = saturate(1.0f - i.depth / 2.2f) * i.fade;
    const float wave_h = (i.world.y - sea_level) /
                         max(ww_total_amplitude_lod(sea_state, i.minwl), 0.001f);
    // The top quarter of the wave, not the top half. Measured from a camera
    // standing in the water: the displacement visible from there runs -3.34 m
    // to +1.21 m against an amplitude of 3.71, because the near crests hide
    // everything taller behind them. A threshold set from the sea's full
    // crest height therefore marks water nobody can see, which is how the
    // first two attempts both produced exactly 2.9% coverage and did not move
    // when the number changed.
    const float crest_foam = saturate((wave_h - 0.25f) * 2.6f) *
                             saturate((sea_state - 0.28f) * 2.4f) * i.fade;
    const float foam = saturate(max(shore_foam * shore_foam, crest_foam));

    const float4 dbg = debug_view(i, n, foam);
    if (dbg.a > 0.0f) return dbg;

    const float diffuse = 0.25f + 0.75f * saturate(dot(n, l));

    float3 col = lerp(body * diffuse, sky, fres) + spec;
    col = lerp(col, float3(0.86f, 0.90f, 0.93f) * diffuse, foam);

    // Into the haze, the same way the terrain does, or the sea meets the sky
    // at a hard line.
    const float haze = saturate(i.dist / far_plane);
    col = lerp(col, float3(0.40f, 0.50f, 0.66f), haze * haze);

    // Shallow water is see-through; deep water is not. Fresnel wins at
    // grazing angles either way, which is correct -- you cannot see into a
    // lake from its far shore.
    const float alpha = saturate(max(lerp(0.35f, 1.0f, dmix), fres) + foam);
    return float4(col, alpha);
}
