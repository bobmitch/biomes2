// ---------------------------------------------------------------------------
// CANONICAL field function, layers 0 and 1 (ADR-001).
//
// Same dialect rules as noise.h: compiles unmodified as C++ and as HLSL,
// tools/wgslgen emits the WGSL. Everything that evaluates per sample lives
// here. The compute kernel that calls it is hand-written per backend and is
// NOT part of the dialect.
//
// Parameters are passed as scalars because the dialect has no structs. That is
// liveable at two layers and will need revisiting when layer 4 arrives with a
// per-biome height profile -- see ADR-001's open questions.
// ---------------------------------------------------------------------------
#ifndef WORLDGEN_FIELD_H
#define WORLDGEN_FIELD_H
#include "noise.h"

// Layer 0 -- continent mask and edge taper. r is normalised radius, 0 at the
// world centre and 1 at the rim. Past r = 1 the caller drops terrain into the
// void; that is a world-bounds decision, not a field one.
//
// The home-landmass guarantee is a FLOOR, not a lift. An additive dome of
// A*(1 - smoothstep(0, 0.25, r)) puts +3.1 standard deviations of the
// continent noise at the centre, which made the world's highest point sit
// within 640 m of the origin on 59 of 64 seeds. Flooring instead leaves relief
// to the noise now and to the layer-4 biome profiles later, which is where it
// belongs. Measured: peaks inside r < 0.25 fall from 64/64 to 2/64, while the
// central disc gets MORE reliably land (90.6% -> 95.6%).
//
// The floor is blended out across r rather than driven to zero, because
// wg_smax(base, 0, k) is a soft ReLU and would clamp every ocean in the world
// to a uniform shelf -- interior ocean depth collapses sevenfold if you do.
// The radial half of layer 0: the centre floor and the edge taper, applied to
// a base that already carries its offset. Split out so the land knee can
// bisect the offset against precomputed noise and still run this exact code.
WG_FN float layer0_shape(float base, float r, float centre_floor, float edge_drop) {
    const float t = wg_smoothstep(0.0f, 0.40f, r);
    const float lifted = wg_smax(base, centre_floor, 0.01f) * (1.0f - t) + base * t;
    return lifted - edge_drop * wg_smoothstep(0.80f, 1.0f, r);
}

WG_FN float layer0_height(float x, float z, float r, float freq, int octaves, float centre_floor, float edge_drop, float land_offset, uint seed, float voxel) {
    const float base = fbm_lod(x * freq, z * freq, octaves, 2.0f, 0.5f, seed, 1.0f / freq, voxel) + land_offset;
    return layer0_shape(base, r, centre_floor, edge_drop);
}

// --- layer 0, PROTOTYPE: land as a lottery with fences ----------------------
// docs/worldgen-design.md, "Layer 0, proposed". Every switch here defaults to
// off, and off is today's mask bit for bit: the regime is not evaluated, the
// fine noise is not evaluated, and the base is the coarse fbm plus the offset
// in the same order of operations as layer0_height above.
//
// The regime field is a low-frequency fbm, scaled by kRegimeNorm to read in
// standard deviations (a 3-octave fbm's std is 0.248, measured). It sets two
// things locally: an offset on the mask (regime_mean + regime_amp * regime),
// and the weight of a FINE continent noise against the coarse one
// (fine_weight - fine_regime * regime: low regime, more islets). The two
// noises' OUTPUTS are blended, never their frequencies, and divided by
// sqrt((1-w)^2 + w^2) so the blend keeps the noise's spread -- otherwise the
// mix would change how much land a region holds as well as its shape.
//
// A frequency that is constant for a whole seed (the regime's, drawn per seed)
// is not the "never blend noise parameters" mistake; that is about varying
// them across space within one world.
static const float kRegimeNorm = 4.0f;

struct Layer0Params {
    float freq;            // coarse continent noise, 1 / feature size
    int   octaves;
    float centre_floor;
    float edge_drop;
    float land_offset;     // this SEED's offset: the knee may have lowered it
    float fine_freq;       // fine continent noise
    int   fine_octaves;
    float fine_weight;     // weight of the fine noise at regime 0
    float fine_regime;     // how much the regime moves that weight, per std
    float regime_freq;     // 1 / regime feature size (drawn per seed, or fixed)
    int   regime_octaves;
    float regime_mean;     // mask offset where the regime is average
    float regime_amp;      // mask offset per std of the regime
    int   regime_on;       // 0: the regime is never evaluated (and reads 0)
    float shelf_depth;     // metres the shelf levels at under sea level
    float shelf_width;     // mask units below sea level the shelf spans; 0 is off
    float shelf_regime;    // how much the regime widens the shelf, per std
    uint  seed;
    uint  seed_fine;
    uint  seed_regime;
};

// Layer 0's noise before the radial shape and the offset: n is the blended
// continent noise with the regime's offset added; regime is the regime in
// standard deviations (0 when it is off).
struct Layer0Noise {
    float n;
    float regime;
};

WG_FN Layer0Noise layer0_noise(float x, float z, Layer0Params p, float voxel) {
    Layer0Noise o;
    float n = fbm_lod(x * p.freq, z * p.freq, p.octaves, 2.0f, 0.5f, p.seed, 1.0f / p.freq, voxel);
    float regime = 0.0f;
    if (p.regime_on != 0) {
        regime = kRegimeNorm * fbm_lod(x * p.regime_freq, z * p.regime_freq, p.regime_octaves, 2.0f, 0.5f, p.seed_regime, 1.0f / p.regime_freq, voxel);
    }
    const float wf = wg_saturate(p.fine_weight - p.fine_regime * regime);
    if (wf > 0.0f) {
        const float fine = fbm_lod(x * p.fine_freq, z * p.fine_freq, p.fine_octaves, 2.0f, 0.5f, p.seed_fine, 1.0f / p.fine_freq, voxel);
        const float wc = 1.0f - wf;
        n = (n * wc + fine * wf) / sqrt(wc * wc + wf * wf);
    }
    if (p.regime_on != 0) {
        n = n + (p.regime_mean + p.regime_amp * regime);
    }
    o.n = n;
    o.regime = regime;
    return o;
}

WG_FN float layer0_mask(float x, float z, float r, Layer0Params p, float voxel) {
    const Layer0Noise o = layer0_noise(x, z, p, voxel);
    return layer0_shape(o.n + p.land_offset, r, p.centre_floor, p.edge_drop);
}

// The shelf's width where the regime reads `regime`. Never below a tenth of
// the set width, so a shelf that is on stays continuous at the coast.
WG_FN float layer0_shelf_width(Layer0Params p, float regime) {
    if (p.shelf_width <= 0.0f) return 0.0f;
    const float w = p.shelf_width * (1.0f - p.shelf_regime * regime);
    const float lo = 0.1f * p.shelf_width;
    return w > lo ? w : lo;
}

// Metres of water over the seabed `u` mask units below sea level, with a
// shelf. Rational (invariant 9): a quick rise to `depth` (half of it a tenth
// of the way out), then the old slope `scale` coming in as t^4 / (1 + t^4),
// t = u / width. Flat near the coast, the old gradient far out, deeper than
// the old seabed by `depth` there. Positive for every u > 0, so the shelf
// never reaches sea level: it creates no land and moves no coastline.
WG_FN float shelf_depth_at(float u, float scale, float depth, float width) {
    const float t = u / width;
    const float t2 = t * t;
    const float t4 = t2 * t2;
    return depth * t / (t + 0.1f) + scale * u * t4 / (1.0f + t4);
}

// Layer 1 -- the danger field, defined on normalised AREA (r squared), never
// on normalised radius. Disc area grows as r squared, so using radius here
// silently skews every biome quota downstream.
WG_FN float layer1_danger(float x, float z, float r, float w, float freq, int octaves, uint seed) {
    const float r2 = r * r;
    return wg_saturate(r2 + w * fbm(x * freq, z * freq, octaves, 2.0f, 0.5f, seed));
}

// --- layer 4: height composition -------------------------------------------
//
// Blend the resulting HEIGHTS across a cell boundary, never the noise
// parameters. Interpolating octave counts and lacunarity produces mush at
// exactly the place the player is looking, so each neighbouring cell's profile
// is evaluated in full and only the metres are mixed.
//
// The per-biome shaping parameters travel as a BiomeProfile struct. They were
// eleven loose scalars while a WGSL generator forbade structs; ADR-002
// retired that, and layer 6's cave parameters would have made it thirteen.

// How far a ridged crest is rounded, in noise units: |n| becomes
// sqrt(n*n + k*k) near the crest and stays |n| away from it.
//
// The ridged noise's 1 - 2|n| is a knife-edge, and a voxel grid samples a
// knife-edge as zigzag teeth: the "toothed ledges" along every ridge line,
// 2.7% of a rough hillside (engine --mesh-debug 2). Rounding the crest cut
// that to 1.8% at this k and the teeth went with it. It also lowers each
// crest's tip by up to 2k in ridged units, so ridged land reads slightly
// more weathered; that trade was chosen by eye at k = 0.05 over 0.1.
//
// sqrt(n*n + k*k) - k would keep the tips and lift EVERYTHING else by 2k --
// whole ridged hillsides rose many metres in the A/B. Do not "fix" it to that.
// sqrt only: invariant 9.
static const float kCrestRound = 0.05f;

WG_FN float crest_abs(float n, float k) {
    return k > 0.0f ? sqrt(n * n + k * k) : abs(n);
}

// fBm with a warp from rolling to ridged. ridge_blend 0 is plain fBm; 1 is
// ridged peaks. Not a blend of billow against ridged -- those are mirror
// images of each other and cancel to a flat field at the midpoint.
WG_FN float fbm_shaped(float x, float y, int octaves, float lacunarity, float gain, float ridge_blend, uint seed, float base_period, float voxel) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o = o + 1) {
        const float n = simplex2(x * freq, y * freq, seed + asuint(o) * 0x9E3779B1u);
        const float ridged = 1.0f - 2.0f * crest_abs(n, kCrestRound);
        const float v = n * (1.0f - ridge_blend) + ridged * ridge_blend;
        const float w = octave_weight(base_period / freq, voxel);
        sum = sum + amp * w * v;
        norm = norm + amp;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    if (norm > 0.0f) { return sum / norm; }
    return 0.0f;
}

// A rational bias curve, NOT pow(t, flatness). Same shape -- identity at
// flatness 1, pushes towards the base elevation above it and towards peaks
// below it, endpoints fixed at 0 and 1 -- but built from multiply and divide
// only, so it is bit-identical on every platform.
//
// pow is a libm function and is not exactly specified: glibc and the MSVC CRT
// disagree by an ULP. Layer 4 absorbed that, because its output is continuous.
// Layer 5 does not: the drainage network is a DISCRETE function of this
// heightfield, so one ULP flips a depression-fill comparison and the river
// moves. It cost three of eight golden seeds across platforms before CI caught
// it. The denominator is flatness*(1-t) + t, positive for any flatness > 0.
WG_FN float flatness_curve(float t, float flatness) {
    return t / (flatness * (1.0f - t) + t);
}

// The shaping half of a BiomeArchetype, which is all layer 4 needs. Kept here
// rather than taking the archetype directly so worldgen/shaders/ stays
// independent of the table format, and so the same struct can be packed into
// a constant buffer for the HLSL path.
struct BiomeProfile {
    float base_elevation;
    float amplitude;
    int   octaves;
    float lacunarity;
    float gain;
    float ridge_blend;
    float flatness;
    float detail_amp;    // metres of the shared micro-relief (layer4_detail)
    float detail_fine_amp;  // ... and of the fine band, a second field of its own
    // layer 6
    float overhang_strength;
    float cave_density;
    float cave_depth_min;
    float cave_depth_max;
};

// One biome's profile, in metres.
WG_FN float layer4_profile(float x, float z, float freq, BiomeProfile p, uint seed, float voxel) {
    const float n = fbm_shaped(x * freq, z * freq, p.octaves, p.lacunarity, p.gain, p.ridge_blend, seed, 1.0f / freq, voxel);
    const float t = wg_saturate(n * 0.5f + 0.5f);
    const float shaped = flatness_curve(t, p.flatness);
    return p.base_elevation + p.amplitude * (shaped * 2.0f - 1.0f);
}

// Layer 4's micro-relief: one field for every biome, which each scales by its
// own detail_amp before the heights are blended. Faded by voxel like every
// other octave, so LOD levels agree.
WG_FN float layer4_detail(float x, float z, float period, int octaves, float gain, uint seed, float voxel) {
    const float f = 1.0f / period;
    return fbm_lod(x * f, z * f, octaves, 2.0f, gain, seed, period, voxel);
}

// --- layer 5b: flat sites ---------------------------------------------------
// A site pulls the ground to `level` over its pad and eases back to the
// terrain across its shoulder. Sites never overlap (world.cpp places one in
// the middle of each candidate cell), so a point is inside at most one.
WG_FN float flat_apply(float h, float x, float z, float cx, float cz,
                       float radius, float shoulder, float level) {
    const float dx = x - cx;
    const float dz = z - cz;
    const float d2 = dx * dx + dz * dz;
    const float outer = radius + shoulder;
    if (d2 >= outer * outer) return h;
    float t = 1.0f;
    if (d2 > radius * radius) t = 1.0f - wg_smoothstep(radius, outer, sqrt(d2));
    return h + (level - h) * t;
}

// A cell's blend weight, from its distance dk, the nearest cell's d0 and the
// FIFTH nearest's d5. Shared by every caller on both backends.
//
// The blend only ever looks at the four nearest cells, and a weight that is
// non-zero out to `width` beyond the nearest can be carried by a fifth. Where
// the fourth and fifth trade places -- a straight bisector -- that weight
// vanished in one step. Measured over 300 random 3 km lines on one seed:
// 791 steps of more than a metre in the heightfield, the largest 63 m, and
// the same cut multiplied by layer 6 drew 1.2 m overhang walls along the
// bisector that rendered as a dark serrated seam.
//
// Subtracting the weight the fifth cell WOULD have makes every weight reach
// zero exactly as its cell ties with the fifth, so the swap is continuous.
// Wherever the fifth is beyond `width`, which is most of the world, the
// subtraction is zero and the weight is what it always was.
WG_FN float blend_weight(float dk, float d0, float d5, float width) {
    float t5 = 1.0f - (d5 - d0) / width;
    if (t5 < 0.0f) t5 = 0.0f;
    const float t = 1.0f - (dk - d0) / width - t5;
    return t > 0.0f ? t * t : 0.0f;
}

// What a land-biome blend divides by. `wland` is the summed weight of the
// LAND cells, `wall` of every cell including ocean ones (which carry no
// profile).
//
// Dividing by wland alone renormalises over land, which is the design -- a
// land biome's profile reaches all the way to its coast. But where the only
// land cell among the four is the one leaving, its weight falls to zero while
// its renormalised SHARE stays 100%, and the value drops off a cliff when it
// goes: v9's continuous weights still left a 63 m height step and 351 overhang
// walls over a metre, every one with three ocean cells nearest.
//
// Flooring the divisor at 5% of the total weight fades the land profile out
// across that fringe instead. Anywhere land holds 5% of the weight the result
// is exactly the old one. The floor is RELATIVE to the total because near a
// five-way junction every weight shrinks together, and an absolute floor
// would flatten real land there.
WG_FN float blend_divisor(float wland, float wall) {
    const float floor_w = 0.05f * wall;
    return wland > floor_w ? wland : floor_w;
}

// Weighted mix of up to four neighbouring cells' heights. Weights come from
// smoothed distance to the cell boundary and are the caller's business: which
// cells are near a point is an index question, not a field one. `wall` is the
// total weight including ocean cells, whose h and w the caller passes as 0.
WG_FN float layer4_blend(float h0, float w0, float h1, float w1, float h2, float w2, float h3, float w3, float wall) {
    const float wsum = w0 + w1 + w2 + w3;
    const float div = blend_divisor(wsum, wall);
    if (div > 0.0f) {
        return (h0 * w0 + h1 * w1 + h2 * w2 + h3 * w3) / div;
    }
    return h0;
}

// Final ground height in metres: the biome profiles laid over the shared
// continental base, faded out as the continent mask drops to sea level so the
// coastline does not step.
WG_FN float layer4_height(float continent, float continent_scale, float blended, float sea_level) {
    const float land = wg_smoothstep(sea_level, sea_level + 0.12f, continent);
    return continent * continent_scale + blended * land;
}

// layer4_height with layer 0's shelf under the sea. shelf_width 0 is exactly
// layer4_height. Below sea level the biome relief is already faded to zero,
// so the seabed is the mask's mapping alone and the shelf replaces it there.
WG_FN float layer4_height_shelf(float continent, float continent_scale, float blended, float sea_level, float shelf_depth, float shelf_width) {
    if (shelf_width > 0.0f && continent < sea_level) {
        return sea_level * continent_scale - shelf_depth_at(sea_level - continent, continent_scale, shelf_depth, shelf_width);
    }
    return layer4_height(continent, continent_scale, blended, sea_level);
}

// --- layer 5: rivers --------------------------------------------------------
//
// Same split as layer 4. WHICH segments are near a point is a spatial index
// question and belongs to the caller; the geometry and the channel profile are
// here, so both backends agree on the shape of the water. That keeps arrays
// out of the dialect, which is the only reason the generator is still small.

// Distance from a point to a segment, in the plane.
WG_FN float river_segment_distance(float px, float pz, float ax, float az, float bx, float bz) {
    const float vx = bx - ax;
    const float vz = bz - az;
    const float wx = px - ax;
    const float wz = pz - az;
    const float len2 = vx * vx + vz * vz;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = wg_saturate((wx * vx + wz * vz) / len2);
    }
    const float dx = wx - vx * t;
    const float dz = wz - vz * t;
    return sqrt(dx * dx + dz * dz);
}

// Carve the channel out of the surface. Subtracted from the height rather
// than faked with noise: noise-faked rivers are cheaper and read as fake the
// moment the player has seen real drainage.
WG_FN float river_carve(float h, float d, float width, float depth) {
    const float t = wg_saturate(1.0f - d / width);
    const float profile = t * t * (3.0f - 2.0f * t);
    return h - depth * profile;
}


// --- layer 6: 3D density ----------------------------------------------------
//
// density(p) > 0 is rock, < 0 is air, and the sign change is the surface.
// Everything above this layer produces a heightfield; this is where the world
// stops being 2.5D.

// Ridged 3D noise. The creases are what make an overhang: a plain fBm
// displacement just makes the surface wobble.
//
// `crest` rounds the crest (kCrestRound for the overhang term). The caves pass
// 0, which keeps the exact abs: cave_field thresholds the crest with a
// smoothstep up to 0.97, so a rounded crest topping out at 0.9 would narrow
// every tunnel -- a different change from the one that was decided.
WG_FN float ridged3(float x, float y, float z, int octaves, float lacunarity, float gain, uint seed, float base_period, float voxel, float crest) {
    const float n = fbm3_lod(x, y, z, octaves, lacunarity, gain, seed, base_period, voxel);
    return 1.0f - 2.0f * crest_abs(n, crest);
}

// Tunnels, as the intersection of two ridged fields. Each field's crest is a
// sheet through the volume; two sheets meet in a line, and a line is a tunnel.
// The design doc also offers a gyroid, which is unavailable: a gyroid is sin
// and cos, and invariant 9 forbids libm upstream of the golden hash.
WG_FN float cave_field(float x, float y, float z, float freq, uint seed, float voxel) {
    const float bp = 1.0f / freq;
    const float a = ridged3(x * freq, y * freq, z * freq, 2, 2.0f, 0.5f, seed, bp, voxel, 0.0f);
    const float b = ridged3(x * freq, y * freq, z * freq, 2, 2.0f, 0.5f, seed + 0x9E3779B1u, bp, voxel, 0.0f);
    const float ta = wg_smoothstep(0.62f, 0.97f, a);
    const float tb = wg_smoothstep(0.62f, 0.97f, b);
    return ta * tb;
}

// Caves live in a depth band below the surface. Without the lower edge they
// open onto cliff faces wherever the ground happens to cut through one, which
// reads as holes punched in the terrain rather than as a cave mouth.
WG_FN float cave_depth_mask(float depth, float dmin, float dmax) {
    const float lo = wg_smoothstep(dmin, dmin + 12.0f, depth);
    const float hi = 1.0f - wg_smoothstep(dmax - 24.0f, dmax, depth);
    return lo * hi;
}

WG_FN float layer6_density(float x, float y, float z, float height, BiomeProfile p, float overhang_freq, float overhang_scale, float cave_freq, float cave_carve, uint seed_overhang, uint seed_cave, float voxel) {
    float d = height - y;
    if (p.overhang_strength > 0.0f) {
        const float bp = 1.0f / overhang_freq;
        const float rg = ridged3(x * overhang_freq, y * overhang_freq, z * overhang_freq,
                                 3, 2.0f, 0.5f, seed_overhang, bp, voxel, kCrestRound);
        d = d + p.overhang_strength * overhang_scale * rg;
    }
    if (p.cave_density > 0.0f) {
        const float m = cave_depth_mask(height - y, p.cave_depth_min, p.cave_depth_max);
        if (m > 0.0f) {
            const float c = cave_field(x, y, z, cave_freq, seed_cave, voxel);
            // Blend towards air rather than subtracting from rock. density is
            // height - y, which grows without bound with depth, so a fixed
            // subtraction stops winning a few metres down and the cave simply
            // never opens. Measured that way: 0 of 19200 subsurface samples
            // were air. cave_density widens the tunnel instead of deepening
            // the cut.
            const float open = wg_saturate(c * (0.5f + 2.0f * p.cave_density) * m);
            d = d * (1.0f - open) - cave_carve * open;
        }
    }
    return d;
}

// --- layer 7: cave entrances ------------------------------------------------
//
// Layer 6 produces a cave system that is sealed. cave_depth_mask forbids
// carving within cave_depth_min of the surface, which is what stops the
// terrain reading as swiss cheese -- and also what stops any cave reaching
// daylight. Measured over 12 seeds: 43% of land columns had air beneath them,
// 83% of those under 25 m or more of rock, and the shallowest rind anywhere
// was 1 m. Never zero. The few near-breaches were not cave mouths either,
// just places where the layer-6 overhang term happened to drop real rock
// below the heightfield the mask is measured against.
//
// Loosening the mask is the wrong lever: it would open holes everywhere the
// noise crests near grade. So access is its own layer. Shafts are traced on
// the CPU at world init -- downwards from a point on the surface until the
// walk breaks into cave air layer 6 already put there -- and stored as 3D
// polylines with a per-vertex radius. A shaft exists only if its walk
// terminated in air, so surface-to-cave connectivity holds by construction,
// the way priority flood makes rivers reach the sea.
//
// Same split as layer 5: WHICH segments are near a sample is a spatial index
// question and belongs to the caller; the geometry is here. The carve
// deliberately ignores cave_depth_mask -- breaching the surface is the whole
// point of this layer.

// Distance from a point to a shaft segment, less the shaft radius there:
// negative inside the tube, zero on its wall. The layer-5 distance is planar
// because a river is; a shaft is not, so this one carries y.
//
// The radius is interpolated at the same t the distance uses, which is why
// this is one function rather than a distance plus a separate lookup.
WG_FN float entrance_tube_sd(float px, float py, float pz, float ax, float ay, float az, float ra, float bx, float by, float bz, float rb) {
    const float vx = bx - ax;
    const float vy = by - ay;
    const float vz = bz - az;
    const float wx = px - ax;
    const float wy = py - ay;
    const float wz = pz - az;
    const float len2 = vx * vx + vy * vy + vz * vz;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = wg_saturate((wx * vx + wy * vy + wz * vz) / len2);
    }
    const float dx = wx - vx * t;
    const float dy = wy - vy * t;
    const float dz = wz - vz * t;
    return sqrt(dx * dx + dy * dy + dz * dz) - (ra + (rb - ra) * t);
}

// Open the shaft out of the rock. Blends towards air rather than subtracting
// from it, for the reason layer 6's cave carve does: density is height - y,
// which grows without bound with depth, so a fixed subtraction stops winning a
// few metres down and the shaft closes under its own overburden. falloff
// metres of feathering outside the wall keep the mouth from reading as a
// drilled hole.
WG_FN float entrance_carve(float d, float sd, float falloff, float carve) {
    const float open = 1.0f - wg_smoothstep(-falloff, 0.0f, sd);
    if (open <= 0.0f) { return d; }
    return d * (1.0f - open) - carve * open;
}

// Shaft radius at u along the trunk, 0 at the mouth and 1 at the cave it
// breaks into: a wide bowl at the surface, a narrow throat below it, chambers
// at intervals, and a taper into the cave so the join does not read as a pipe
// ending in a wall.
//
// The chambers are a triangle wave off floor() rather than a cosine. This
// value is baked into the stored radii and so reaches the golden hash, which
// puts it under invariant 9: + - * / sqrt floor and comparisons only.
WG_FN float entrance_radius(float u, float mouth, float throat, float bowl, float bowls) {
    float flare = 0.0f;
    if (u < 0.22f) { flare = 1.0f - u / 0.22f; }
    float r = throat + (mouth - throat) * flare * flare;
    const float f = u * bowls;
    const float frac = f - floor(f);
    float tri = 2.0f * frac - 1.0f;
    if (tri < 0.0f) { tri = -tri; }
    tri = 1.0f - tri;
    r = r + bowl * tri * tri;
    float tail = 0.0f;
    if (u > 0.88f) { tail = (u - 0.88f) / 0.12f; }
    return r * (1.0f - 0.35f * tail);
}

// Placed caves (layer 7 since v19) roughen their capsule walls with one
// octave of 3D simplex: sd plus amp x noise, so a tunnel reads as rock rather
// than a drilled pipe. Only evaluated near a wall (sd < amp), faded out with
// the voxel like any octave so coarse LODs see the smooth capsule. amp 0
// returns sd unchanged, which is what keeps a world without placed caves
// (the shafts, whose amp is 0) bit-identical.
//
// It also moves the carve's feathering OUTSIDE the wall (less `falloff`):
// entrance_carve is fully open only `falloff` inside sd = 0, and under 20-30 m
// of rock a partly open blend loses to the overburden, so a 1.8 m tunnel
// pinched shut wherever the noise thinned it. Placed caves always have
// amp > 0 (the table requires it), so the shift rides on the same switch.
WG_FN float cave_wall_sd(float sd, float x, float y, float z, float amp, float freq, uint seed, float voxel, float falloff) {
    if (amp <= 0.0f) { return sd; }
    if (sd > amp + falloff) { return sd; }
    const float fade = octave_weight(1.0f / freq, voxel);
    return sd - falloff + amp * fade * simplex3(x * freq, y * freq, z * freq, seed);
}

// --- layer 8: trees ---------------------------------------------------------
//
// One candidate per cell of a square grid. The cell's hash decides everything
// about its tree -- where in the cell, which species, how tall, which way it
// faces -- so a tree is a pure function of its cell, its cell is its identity,
// and a save marks a felled tree by naming the cell. Nothing is stored.
//
// Same split as layers 5 and 7: which biomes are near a point, and how high the
// ground is there, are the caller's questions. The arithmetic is here, under
// invariant 9 like everything else that reaches the hash.

// A uniform in [0, 1) from 24 bits of a hash.
WG_FN float tree_unit(uint h) {
    return float(h >> 8u) * (1.0f / 16777216.0f);
}

// The k-th independent hash of a cell, k = 0.. for each decision.
WG_FN uint tree_hash(int i, int j, uint seed, uint k) {
    return pcg(hash2i(i, j, seed) ^ (k * 0x68E31DA4u));
}

// Where a patch's noise threshold sits for a given cover. The patch noise is a
// 3-octave fBm whose distribution is close to normal and NOT uniform, so cover
// c is not 1 - 2c. These are its measured quantiles at c = 0, 0.1 .. 1 over
// 200 seeds (validate --tree-quantiles), interpolated linearly.
WG_FN float tree_cover_threshold(float cover) {
    float q[11];
    q[0]  =  0.680f; q[1]  =  0.3346f; q[2]  =  0.2328f; q[3]  =  0.1497f;
    q[4]  =  0.0736f; q[5]  =  0.000f; q[6]  = -0.0736f; q[7]  = -0.1497f;
    q[8]  = -0.2328f; q[9]  = -0.3346f; q[10] = -0.680f;
    const float f = wg_saturate(cover) * 10.0f;
    int k = int(floor(f));
    if (k > 9) k = 9;
    const float t = f - float(k);
    return q[k] + (q[k + 1] - q[k]) * t;
}

// 0 in a clearing, 1 inside a patch, thinning over `edge` noise units at the
// boundary so a wood's margin is sparse rather than a wall.
WG_FN float tree_patch(float x, float z, float freq, float cover, float edge, uint seed) {
    if (cover <= 0.0f) return 0.0f;
    if (cover >= 1.0f) return 1.0f;
    const float n = fbm(x * freq, z * freq, 3, 2.0f, 0.5f, seed);
    const float thr = tree_cover_threshold(cover);
    return wg_smoothstep(thr - edge, thr + edge, n);
}

// How well a species suits an altitude: 0 outside its band, rising linearly
// over `ramp` metres inside each end, 1 in the middle. Broadleaves give way to
// conifers uphill through these ramps, and the top of the highest band is the
// treeline.
WG_FN float tree_altitude_fit(float h, float lo, float hi, float ramp) {
    const float a = wg_saturate((h - lo) / ramp);
    const float b = wg_saturate((hi - h) / ramp);
    return a < b ? a : b;
}

#endif
