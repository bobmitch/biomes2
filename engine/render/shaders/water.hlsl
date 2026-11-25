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
    float  debug_mode; float cam_water; float flood_on; float pad0;   // flood_on: g_flood is bound (ADR-006 stage 3)
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
    float  frame;         float storm_turbidity;  float2 flood_origin;   // extinction x (1 + this x roughness^2); the flood mask's corner (whole metres)
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
// ADR-006 stage 3: the flood round the camera (renderer.cpp fill_flood_mask),
// a byte a 0.5 m column (the edit lattice), 256 x 256 from flood_origin, two
// planes. The first: 0 no word (the column rule stands), 1 dry air in a land
// column (never water), else 2 + the open sea's waves' share x 253 where the
// cell under sea level is flooded. The second (the fill): how far under sea
// level the water stands there now, 0.1 m (255 none in yet). The third,
// round flooded columns: the edited density at sea level (the sheet's own
// height), 128 + 64 x density (rock positive; 255 rock where not computed).
StructuredBuffer<uint> g_flood : register(t7);

uint flood_byte(int2 c, uint plane) {
    if (any(c < 0) || any(c >= 256)) return 0u;
    const uint i = plane * 65536u + uint(c.y * 256 + c.x);
    return (g_flood[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu;
}

// The flood at p, FILTERED: x how flooded (bilinear over the four columns
// round p, a column counting once the water has reached it), y their waves'
// share, z their drop under sea level (the fill), w 1 where the NEAREST
// column is dry air. Point-sampled, every one of these switched on a 0.5 m
// column's edge, and the beach's foam and swash, the forced depth and a
// rising level's sheet all stepped in squares. Filtered, the values bleed
// half a cell into the rock beside the water, which is harmless: water is
// drawn only where the real ground behind the pixel is under its level
// (edge_fade in ps_main), and never in a dry column.
float4 flood_sample(float2 p) {
    if (flood_on < 0.5f) return float4(0.0f, 1.0f, 0.0f, 0.0f);
    const float2 g = (p - flood_origin) / 0.5f;   // column centres on the integers
    const int2 c0 = int2(floor(g));
    const float2 f = g - float2(c0);
    float w = 0.0f, calm = 0.0f, drop = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 c = c0 + int2(k & 1, k >> 1);
        const float wt = ((k & 1) ? f.x : 1.0f - f.x) * ((k >> 1) ? f.y : 1.0f - f.y);
        const uint b = flood_byte(c, 0u);
        const uint lv = flood_byte(c, 1u);
        if (b < 2u || lv == 255u) continue;
        w += wt;
        calm += wt * float(b - 2u) / 253.0f;
        drop += wt * float(lv) * 0.1f;
    }
    if (w > 1e-4f) { calm /= w; drop /= w; } else calm = 1.0f;
    const float dry = flood_byte(int2(floor(g + 0.5f)), 0u) == 1u ? 1.0f : 0.0f;
    return float4(w, calm, drop, dry);
}

// How far p is from a dug wall at sea level, sideways, in metres: negative
// inside the dug air. From the edited density there (the third plane),
// bilinear over the four columns round p, divided by its SIDEWAYS gradient:
// at a dug wall the density changes fast across it, on a low beach -- rock
// at sea level by a few centimetres, its density as small as a wall's --
// it barely changes sideways at all, and reads as far from any wall. The
// density alone could not tell 30 cm from the rim from 30 cm above the sea,
// and a band set on it swallowed the whole low beach. A blend touching a
// column not computed (255) is far from any wall. y: how steep the wall is
// (the sideways gradient, per metre): a dug wall is steep, ground that meets
// sea level gently is a beach.
float2 flood_rim(float2 p) {
    const float2 g = (p - flood_origin) / 0.5f;
    const int2 c0 = int2(floor(g));
    const float2 f = g - float2(c0);
    float v[4];
    [unroll] for (int k = 0; k < 4; ++k) {
        const uint b = flood_byte(c0 + int2(k & 1, k >> 1), 2u);
        if (b == 255u) return float2(1e3f, 0.0f);
        v[k] = (float(b) - 128.0f) / 64.0f;
    }
    const float d = lerp(lerp(v[0], v[1], f.x), lerp(v[2], v[3], f.x), f.y);
    const float2 grad = float2(lerp(v[1] - v[0], v[3] - v[2], f.y), lerp(v[2] - v[0], v[3] - v[1], f.x)) / 0.5f;
    const float gl = length(grad);
    return float2(d / max(gl, 0.05f), gl);
}

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
    float4 flood  : TEXCOORD8;   // the flood here: how much it applies (0 the column rule), the water's drop under sea level, its calm (1 open sea), how flooded the columns round it are
    float3 rim    : TEXCOORD9;   // a filling hole's whole drop (0 none); 1 on the land side; inside the dug wall (0..1)
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
    float depth = sea_level - ground;
    const float depth_gen = depth;
    // ADR-006 stage 3: where the flood says the cell under sea level is the
    // sea, it is, whatever the generated column says -- a channel dug in from
    // the coast -- and the open sea's waves reach it only as far as the
    // flood's path through the water lets them (fade, which scales every
    // train). A flooded land column has no generated depth to tame the
    // waves with; 2 m stands in, the waves already calmed by the path.
    float fade_w = fade;
    float fill_drop = 0.0f;    // the fill: how far under sea level the water stands now
    float calm_f = 1.0f;       // the share of the open sea's waves that reaches here
    float drop_full = 0.0f;    // a filling hole's drop, before the rim decides where it applies
    float k_out = 0.0f;        // how much the flood applies here (0 the column rule, 1 the flood's water)
    float inside_out = 0.0f;   // inside the dug wall at sea level (flood_rim), 0..1
    const float4 fs = flood_sample(rest);
    if (fs.x > 0.0f) {
        // Blended by how flooded, so nothing switches at a column's edge.
        // Rising water is still; a flooded column takes its path's calm.
        const float still = saturate(fs.z * 10.0f);
        // The columns without a word are the column rule's: on the SEA side
        // the open sea's full waves, so a sea vertex blends towards them by
        // how flooded; on the land side, rock, which has no waves to blend
        // towards -- a land vertex takes the flood's calm whole, or the walls
        // of a calm channel wore a band of surf all the way in.
        // On the land side the flood applies only INSIDE the dug wall: the
        // zero line of the edited density at sea level (flood_rim) -- the
        // rim itself, between the lattice points, not a column's edge, at
        // the sheet's own height. Outside it the beach keeps its own surf.
        // The step is held to +-2 cm of the line: a low beach stands only
        // centimetres above sea level, and a step set 5-15 cm into the rock
        // took it for the hole and cut its surf in squares again; at the
        // cell under sea level instead, a shaft's narrower waist left a ring
        // of sea-level sheet inside the rim. On the sea side, by how
        // flooded.
        const float2 rim = flood_rim(rest);
        const float inside = saturate((0.02f - rim.x) / 0.04f);   // metres from the wall (flood_rim)
        // The calm and the forced depth switch at the wall too. A soft band
        // outside it was tried, measured as density over its sideways
        // gradient: the jump from a dug sphere's strongly negative inside to
        // the beach's slightly positive density, half a metre apart, made
        // every cell beside a dig read as "at a steep wall", and the band
        // took the surf from a patch the shape of the computed columns.
        const float inside_soft = inside;
        // The flood changes only the LAND side, where its water is: the open
        // sea is the column rule's. Blended on the sea side by how flooded
        // the columns round it were, it forced the shallows beside a flooded
        // shaft deeper in cell-shaped patches and took their surf. Where a
        // channel crosses the old shoreline the change follows that
        // shoreline (the generated depth's zero, a smooth curve).
        const float land = saturate(-depth / 0.25f);
        // Where the generated ground stands well above sea level (0.5-1.5 m
        // and more) the column rule's sheet is never seen -- no swash climbs
        // that high -- so beside flooded columns the flood's calm applies
        // right into the rock, and the grid's triangles straddling the wall
        // stay calm: falling back to the open sea's waves and foam there drew
        // a white sawtooth along a flooded pit's wall. On a LOW beach, where
        // the surf can show, only inside the wall. The CALM only: the forced
        // depth keeps to the wall (k), or a vertex in the rock took a depth
        // between the beach's and the hole's, just under sea level, and the
        // sheet's lift over the ground (12 cm, below) and the swash raised
        // grid triangles up the wall in a row of teeth.
        const float high = saturate((-depth - 0.5f) / 1.0f);
        const float k = land * inside_soft;
        const float k_calm = land * max(inside_soft, fs.x * high);
        calm_f = lerp(1.0f, fs.y * (1.0f - still), k_calm);
        k_out = k_calm;
        inside_out = inside;
        fade_w = fade * calm_f;
        // A LAND column (no generated depth to tame the waves with) stands
        // in 2 m; the open beach beside a channel keeps its own shallow
        // depth, its swash and its foam. The target is continuous through
        // the shoreline: 2 m on land, falling to the sea's own depth over
        // its first 0.4 m.
        const float target = depth < 0.0f ? 2.0f : max(depth, 2.0f - 4.0f * depth);
        depth = lerp(depth, target, k);
        // The rising level's drop: only INSIDE the dug wall (flood_rim's
        // zero line, sharpened over 5 cm), so the step from the hole's level
        // to the sea's falls on the rim itself. Faded by how flooded, or
        // taken whole on the land side, it fell where the filter's reach
        // ended, on the beach, and cut the surf there in squares.
        fill_drop = fs.z * inside * land;
        drop_full = fs.z;
    }

    float3 nrm;
    const float3 p = ww_surface(rest.x, rest.y, time, frame_sea(), depth, fade_w, minwl, nrm);
    // The swash (water_wave.h): within a metre and a half of the still-water
    // line the sheet rises and falls with each arriving set, so the water
    // runs up the beach and back; the terrain in front hides it where the
    // beach is higher. Where it is wet it never sinks below the bed: a storm
    // trough over half a metre of water fell under the seabed and bared it.
    const float reach = ww_swash_reach(depth);
    const float swash = reach > 0.0f ? ww_swash(rest.x, rest.y, time, frame_swash()) * reach : 0.0f;
    float y = p.y + swash;
    // A little past the edge too, 12 cm over the ground out to 30 cm beyond
    // it: the edge is cut per pixel (edge_fade, in the pixel shader), which
    // can only cut away water that was drawn.
    if (depth + swash > -0.3f) y = max(y, 0.12f - depth);
    const float3 world = float3(p.x, sea_level + y - fill_drop, p.z);

    o.world = world;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.nrm = nrm;
    o.depth = depth + swash;   // the water actually over the bed here, for the clip and the shading
    o.dist = d;
    o.fade = fade_w;
    o.minwl = minwl;
    const float3 loc = ww_local(frame_sea(), depth, fade_w, minwl);
    o.hs = loc.z / max(fade_w, 1e-4f);
    o.cap_q = loc.xy;
    o.flood = float4(k_out, fill_drop, calm_f, fs.x);
    o.rim = float3(drop_full, depth_gen < 0.0f ? 1.0f : 0.0f, inside_out);
    return o;
}

// Debug views, so that "the sea looks wrong" can be turned into a number
// instead of an argument. 0 is the real shading. Returned with alpha 0, the
// post chain's debug mark (post.hlsl): ps_final shows the colour as it is,
// unexposed. They used to go through the exposure and came out solid white.
float4 debug_view(VSOut i, float3 n, float foam, float ssr_conf) {
    if (debug_mode < 0.5f || debug_mode > 13.5f) return float4(0, 0, 0, 1);
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
    // Inland. The terrain in front of this already won the depth test almost
    // everywhere, but the shoreline is where the two surfaces meet at a
    // grazing angle and a vertex can land a little the wrong side of it.
    // Only a coarse cut: depth interpolated from the grid's vertices steps
    // with the grid. The edge itself is made per pixel, below (edge_fade).
    // Not inside a dug wall, nor on a triangle that crosses one (i.rim.z,
    // flood_rim's zero line at sea level): water under a roof is drawn, and
    // the rock hides it where it is covered, so its edge is the terrain's.
    // Clipped there as land, the water under an undercut stopped on its
    // columns' edges in steps. It was gated on the flood's FILTER instead
    // (i.flood.w), whose reach ends on 0.5 m column edges: beside a shaft
    // the beach's surf was cut on one side of that edge and drawn over the
    // hole on the other -- a straight-edged notch in the rim.
    const bool noclip = debug_mode > 11.5f && debug_mode < 12.5f;   // 12: as 11, with none of the flood's cuts
    // 14: the real shading, but every flood cut PAINTS its pixel instead of
    // cutting it -- red the coarse land cut, green a filling hole's, blue a
    // dry column's, yellow a straddling triangle's, magenta "no word" --
    // so a straight edge in the water can be traced to the cut that made it.
    uint cut_by = 0xffffffffu;
#define CUT(x, id) { if ((x) < 0.0f) { if (debug_mode > 13.5f) { if (cut_by == 0xffffffffu) cut_by = (id); } else clip(-1.0f); } }
    if (i.rim.z < 0.02f && !noclip) CUT(i.depth + 0.3f, 0);
    // The water's own level here: sea level, or where the fill has risen to.
    const float lvl = sea_level - i.flood.y;

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
    ww_surface_with(i.world.x, i.world.z, time, sea, i.depth, i.fade,
                    max(i.minwl * 0.5f, 0.35f), i.cap_q.x, i.cap_q.y, pn, jac_g);
    float3 gn = normalize(i.nrm);
    const float3 base = normalize(lerp(pn, gn, 0.25f));
    const float3 v = normalize(cam_pos - i.world);
    const float3 l = normalize(sun_dir);
    const float2 px = i.clip.xy;
    const float2 uv = px * inv_size;
    const float dist = length(i.world - cam_pos);
    const float vz = max(dot(i.world - cam_pos, cam_fwd), near_plane);

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
    const float4 d0 = f0 > 0.0f ? g_detail.SampleLevel(g_wrap, float3(i.world.xz / od_tile[0], 0.0f), 0) : float4(0, 0, 0, 0);
    const float4 d1 = f1 > 0.0f ? g_detail.SampleLevel(g_wrap, float3(i.world.xz / od_tile[1], 1.0f), 0) : float4(0, 0, 0, 0);
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
    const float u_here = wind_v.z * wind_gust(i.world.xz, wind_v, wind_drift, time);
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
    const float cls = clouds_on > 0.5f ? cloud_shadow_at(g_cloud_shadow, g_linear, cloud_shadow, l, i.world) : 1.0f;
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
    float zb = g_depth_in[int2(px)];
    float under_len = zb < 1.0f ? max(linear_depth(zb) - vz, 0.0f) * dist / vz : 1e6f;
    // The water's edge, per pixel, from the GROUND behind this pixel (the
    // depth copy) and the swash evaluated right there (water_wave.h): water is
    // present where that ground lies under sea level plus the swash, and fades
    // into the scene over its last 8 cm of depth. Both are smooth per pixel,
    // so the edge follows a true contour of the beach.
    //
    // Two attempts drew a staircase. On the beach the sheet sits a few
    // centimetres over the ground, nearly coplanar with it, and where two such
    // surfaces cross follows the triangles of both meshes; the vertex-
    // interpolated clip stepped with the grid too. Fading by the thickness
    // between the SHEET and the ground only softened the steps: near the shore
    // the sheet's height is itself interpolated across grid cells, clamped to
    // the ground at its vertices, so the fade inherited the grid's shape.
    // Against the water's OWN level (lvl): in a hole the water is rising in,
    // its edge is where that level meets the dug walls, not where sea level
    // would. Rising water has no swash. px_depth: the water over the ground
    // behind this pixel, for the shore's foam where the flood has a word --
    // so the foam follows the dug ground's real edge, not the columns'.
    float edge_fade = 1.0f;
    float px_depth = i.depth;
    {
        const float3 gp = cam_pos - v * (dist + min(under_len, 1e4f));
        const float gdepth = lvl - gp.y;
        // Blended by how flooded, never switched: a threshold drew a line.
        if (zb < 1.0f) px_depth = lerp(i.depth, min(i.depth, max(gdepth, 0.0f)), saturate(i.flood.x));        // Beside a filling hole, on the land side: a pixel of sheet still at
        // the SEA's level whose ground lies deeper than the hole's water is
        // hanging over the hole, and is not drawn. The density's zero line
        // (flood_rim) puts the step within centimetres of the dug wall, but
        // the mesher puts the wall by its own rule within a cell, and the
        // gap between them showed as slivers of sheet on the shaft's wall.
        // The depth copy is where the wall IS.
        if (zb < 1.0f && i.rim.x > 0.01f && i.rim.y > 0.5f && i.flood.y < 0.05f && gdepth > i.rim.x + 0.1f)
            if (!noclip) CUT(-1.0f, 1);
        // Never over a dry column -- a pit, or a pocket the sea does not
        // reach -- however far the flood's filter bled (flood_sample): where
        // the ground behind is well under the sheet, the sheet is over the
        // dry air. Not where the ground is close: dry air trapped under a
        // dug lip lies under the beach, and clipping every pixel over such a
        // column cut the surf on the sand above it in squares.
        if (zb < 1.0f && flood_on > 0.5f && gdepth > 0.3f &&
            flood_byte(int2(floor((i.world.xz - flood_origin) / 0.5f + 0.5f)), 0u) == 1u)
            if (!noclip) CUT(-1.0f, 2);
        // The grid's triangles that straddle the switch at a dug wall slope
        // from the sea's sheet to the flood's, at neither: they lie on the
        // wall itself, so they are not drawn and the rock shows. Drawn, they
        // were a sawtooth between the surf and the rim (keeping their surf
        // side by a per-pixel wall test brought the teeth back: those pixels
        // are on the slope too). The cost: the surf's edge at a dug wall
        // follows the water grid, steps of a few centimetres.
        // The wall test ALONE: the flood's weight also fades with the land
        // over the beach's first 25 cm above sea level, metres wide on a
        // gentle beach, and testing that cut a band out of a channel where
        // it crossed the old shoreline.
        // Only while a hole is FILLING (a drop to step down): at one level
        // the straddling triangles are harmless and the wall makes the edge,
        // while cut whole they took stair-shaped bites out of a flooded pit's
        // edge -- a grid cell is half a metre and more at 30 m.
        // Or where the triangle runs from the flood's calm to the open surf
        // (a low beach, i.flood.x under 1): its outer end has waves and foam,
        // and drawn it was a row of white teeth up a channel's walls. On high
        // ground the calm reaches into the rock (k_calm, the vertex shader)
        // and the triangle is calm end to end: kept.
        if (i.rim.y > 0.5f && i.rim.z > 0.02f && i.rim.z < 0.98f && (i.rim.x > 0.01f || i.flood.x < 0.98f) &&
            !noclip)
            CUT(-1.0f, 3);
        // And where the flood has no word at all: on the land side the
        // column rule's sheet is the swash, never more than ~1.6 m over its
        // own ground (ww_swash_reach). Deeper ground behind it is DUG ground
        // -- a hole wider at the beach than the cell under sea level the
        // flood labels by, or one the sea does not reach -- and the sheet
        // would hang over it.
        if (zb < 1.0f && i.rim.y > 0.5f && i.flood.w < 0.01f && gdepth > 1.6f && !noclip) CUT(-1.0f, 4);
        if (zb < 1.0f && gdepth < 1.6f) {
            const float sw = ww_swash(gp.x, gp.z, time, frame_swash()) * ww_swash_reach(gdepth) *
                             (1.0f - saturate(i.flood.y * 10.0f));
            edge_fade = saturate((gdepth + sw) / 0.08f);
        }
        // Inside a flooded hole the edge is a dug WALL, steep, and the water
        // meets it wherever the grid's triangles and the straddle cut (below)
        // leave it: steps a grid cell long. There the last 1.2 m of the view
        // ray under water fades out, so the wall shows through its own first
        // few centimetres of water and the steps dissolve into it (0.4 m left
        // them showing on `--flood-pit`'s far wall). Only where the flood
        // applies: from above, a natural beach would clear over its shallows.
        if (zb < 1.0f)
            edge_fade = min(edge_fade, lerp(1.0f, smoothstep(0.0f, 1.2f, under_len), saturate(i.flood.x)));
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
    const float down = below ? 0.0f : max(lvl - seabed_p.y, 0.0f);
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

    // Foam where the water is shallow enough to break, and on the crests once
    // the sea is rough. Both are gated on `fade` so the far field does not
    // sprout foam from a normal that is not being displaced any more.
    // At the shore: a drifting lace that thickens towards the waterline,
    // whole at the edge and in patches by two metres. It was a smooth function
    // of depth, which with the shallows now clear read as a white smear.
    // The last 0.6 m of water, not 2.2: with the swash a sheet runs up the
    // beach over a few tens of centimetres, and a band keyed to 2.2 m turned
    // the whole sheet white. Foam at its leading edge, clear water behind.
    const float shallow = saturate(1.0f - px_depth / 0.6f);
    const float lace = 0.6f * wnoise(i.world.xz * 0.45f + float2(time * 0.13f, time * 0.07f)) +
                       0.4f * wnoise(i.world.xz * 1.7f - float2(time * 0.21f, -time * 0.11f));
    // Surf needs breaking waves: where the flood calms the water (a channel,
    // a flooded hole) shore foam goes faster than the waves (calm squared,
    // i.fade carries one), so a channel's mouth has some and its calm inner
    // reach none -- at the waves' own rate its walls wore a band of white
    // fifteen metres in.
    const float shore_foam = smoothstep(1.0f - shallow, 1.25f - shallow, lace) * shallow * i.fade * i.flood.z;
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
    const float crest_foam = saturate(breaking * (0.55f + 0.9f * d0.w) + 0.25f * trail);
    const float foam = saturate(max(shore_foam, crest_foam));

    // 7: the folding measure, white at 1 (still), black at 0 (folding).
    // 9: the flood -- red how flooded, green a filling hole's drop (0-2 m), blue the land side.
    // 10: the shore foam's inputs -- red the calm, green the depth it sees (0-1 m), blue the fade.
    // 11: red how flooded the columns round it are, green inside the dug wall (half), blue the land side (quarter).
    // 13: the edge's fade, the view ray's length under water over 2 m, and
    // how much the flood applies.
    if (debug_mode > 13.5f && cut_by != 0xffffffffu) {
        const float3 cc[5] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0), float3(1, 0, 1)};
        return float4(cc[min(cut_by, 4u)], 0.0f);
    }
    if (debug_mode > 12.5f && debug_mode < 13.5f) return float4(edge_fade, saturate(under_len / 2.0f), saturate(i.flood.x), 0.0f);
    if (debug_mode > 10.5f && debug_mode < 13.5f) return float4(saturate(i.flood.w), 0.5f * saturate(i.rim.z), 0.25f * i.rim.y, 0.0f);
    if (debug_mode > 9.5f && debug_mode < 13.5f) return float4(saturate(i.flood.z), saturate(px_depth), saturate(i.fade), 0.0f);
    if (debug_mode > 8.5f && debug_mode < 13.5f) return float4(saturate(i.flood.x), saturate(i.rim.x * 0.5f), i.rim.y, 0.0f);
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
        const float bed_d = max(i.depth, 0.0f);
        const float lr = bed_d / max(-r.y, 0.15f);
        const float rup = 1.0f - r.y;
        const float4 bh = ssr(i.world + gn * 0.05f, r, px);
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
    col = lerp(col, srgb_to_linear(float3(0.86f, 0.90f, 0.93f)) * light_in, foam);
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
