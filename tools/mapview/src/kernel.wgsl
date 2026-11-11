// ---------------------------------------------------------------------------
// HAND-WRITTEN compute kernel. Outside the dialect, by ADR-001 scope.
//
// Workgroup memory, atomics and storage textures have no CPU counterpart and
// never need one, so this is written per backend rather than generated. It
// calls the field function; it does not define any of it. Nothing in here may
// compute terrain.
// ---------------------------------------------------------------------------

struct Params {
    radius: f32,
    continent_freq: f32,
    centre_bias: f32,
    edge_drop: f32,
    land_offset: f32,
    danger_w: f32,
    danger_freq: f32,
    sea_level: f32,
    continent_octaves: i32,
    danger_octaves: i32,
    layer: i32,
    resolution: i32,
    seed_continent: u32,
    seed_danger: u32,
    pad0: u32,
    pad1: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var out_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read_write> counts: array<atomic<u32>, 2>;

var<workgroup> wg_land: atomic<u32>;
var<workgroup> wg_disc: atomic<u32>;

fn ocean_colour(depth: f32) -> vec3<f32> {
    let d = clamp(depth * 1.2, 0.0, 1.0);
    return mix(vec3<f32>(0.18, 0.31, 0.51), vec3<f32>(0.05, 0.10, 0.24), d);
}

fn land_colour(h: f32) -> vec3<f32> {
    let t = clamp(h * 0.9, 0.0, 1.0);
    if (t < 0.33) {
        return mix(vec3<f32>(0.42, 0.60, 0.33), vec3<f32>(0.30, 0.47, 0.25), t / 0.33);
    }
    if (t < 0.70) {
        return mix(vec3<f32>(0.30, 0.47, 0.25), vec3<f32>(0.56, 0.47, 0.34), (t - 0.33) / 0.37);
    }
    return mix(vec3<f32>(0.56, 0.47, 0.34), vec3<f32>(0.92, 0.94, 0.97), (t - 0.70) / 0.30);
}

// Blue through to red. The danger field is the one thing on this page whose
// shape is a feel judgement, so it gets a ramp that makes banding obvious.
fn danger_colour(d: f32) -> vec3<f32> {
    if (d < 0.5) {
        return mix(vec3<f32>(0.16, 0.34, 0.62), vec3<f32>(0.85, 0.82, 0.45), d / 0.5);
    }
    return mix(vec3<f32>(0.85, 0.82, 0.45), vec3<f32>(0.70, 0.17, 0.16), (d - 0.5) / 0.5);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>,
        @builtin(local_invocation_index) lid: u32) {
    if (lid == 0u) {
        atomicStore(&wg_land, 0u);
        atomicStore(&wg_disc, 0u);
    }
    workgroupBarrier();

    let res = u32(P.resolution);
    let inside_image = gid.x < res && gid.y < res;

    var colour = vec3<f32>(0.04, 0.05, 0.07);   // void past the terminator
    if (inside_image) {
        let R = P.radius;
        let step = 2.0 * R / f32(res);
        let x = -R + (f32(gid.x) + 0.5) * step;
        let z = -R + (f32(gid.y) + 0.5) * step;
        let r = sqrt(x * x + z * z) / R;

        if (r <= 1.0) {
            let h = layer0_height(x, z, r, P.continent_freq, P.continent_octaves,
                                  P.centre_bias, P.edge_drop, P.land_offset, P.seed_continent);
            let is_land = h > P.sea_level;

            atomicAdd(&wg_disc, 1u);
            if (is_land) { atomicAdd(&wg_land, 1u); }

            if (P.layer == 0) {
                if (is_land) { colour = vec3<f32>(0.38, 0.55, 0.32); }
                else { colour = vec3<f32>(0.13, 0.22, 0.38); }
            } else if (P.layer == 1) {
                let d = layer1_danger(x, z, r, P.danger_w, P.danger_freq,
                                      P.danger_octaves, P.seed_danger);
                colour = danger_colour(d);
            } else if (P.layer == 2) {
                let n = fbm(x * P.continent_freq, z * P.continent_freq,
                            P.continent_octaves, 2.0, 0.5, P.seed_continent);
                colour = vec3<f32>(n * 0.5 + 0.5);
            } else {
                if (is_land) { colour = land_colour(h - P.sea_level); }
                else { colour = ocean_colour(P.sea_level - h); }
            }
        }
    }

    workgroupBarrier();
    if (lid == 0u) {
        atomicAdd(&counts[0], atomicLoad(&wg_land));
        atomicAdd(&counts[1], atomicLoad(&wg_disc));
    }

    if (inside_image) {
        textureStore(out_tex, vec2<i32>(i32(gid.x), i32(gid.y)), vec4<f32>(colour, 1.0));
    }
}
