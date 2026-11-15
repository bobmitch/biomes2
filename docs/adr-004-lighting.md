# ADR-004 — Lighting: physically based, atmosphere-lit, ray-traced probe GI

**Status:** accepted for the GI backbone (DDGI on hardware ray tracing,
behind two explicit interfaces); the rest proposed
**Date:** 2026-11-14
**Extends:** [ADR-003](adr-003-graphics-api.md) (D3D12). Touches nothing upstream of the golden hash.

## Context

What lights the world today is a set of good parts on an ad-hoc base:

| Have | Where | Limit |
| --- | --- | --- |
| Sun, one fixed direction | every scene shader | no time of day, no colour from the atmosphere |
| Cascaded sun shadows, 3 x 2048^2 to 520 m | `shadow.hlsli` | sun only |
| Screen-space contact shadows, GTAO-style AO | `post.hlsl` | screen-space only: nothing off screen, nothing behind |
| TAA, HDR targets, direct/ambient split | `scene_out.hlsli`, `post.hlsl` | tonemap is the identity below 0.8, for the crack detector |
| "Ambient" | constants in each shader: `albedo * 0.22 + (0.10, 0.13, 0.18) * sky` | the same in a meadow, under a forest canopy, and at the bottom of a dug shaft |
| Sky and fog | a clear colour, and `(d / far)^2` towards it | no atmosphere, no aerial perspective, no light in the fog |

The last two rows are the problem the brief names. A dug hole, a cave and a
forest floor are lit exactly like open ground, because nothing in the engine
knows how much sky a point can see or what light bounces into it. And
everything planned next makes that worse: torches, fires and magic are
moving local lights that must light both a meadow at noon and a sealed cave,
and the world is editable, so none of it can be baked.

Constraints that shape the answer:

- **Nothing can be precomputed per place.** The world is procedural and
  editable (dig edits, felled trees). Any GI has to be dynamic.
- **Indoor and outdoor are the same world.** A cave mouth must go from sky
  lit to torch lit continuously, without leaking daylight through rock.
- **The renderer is forward**, which suits foliage, alpha-tested cards and
  TAA, and should stay that way.
- **Quality first, with scaling.** Heavy is acceptable if it can be scaled
  down.
- **The dev GPU is a Radeon RX 7900 XT** (RDNA3, 20 GB): DXR 1.1 with inline
  ray queries, mesh shaders, SM 6.6. Its ray-tracing throughput is good, not
  top tier, so ray budgets must be tunable.

## Decision

A physically based lighting pipeline in seven layers, each its own
milestone, each measured before the next.

### 1. Units, exposure, materials

- **Radiometric units throughout.** Sun illuminance and sky radiance come
  out of the atmosphere model in real units; local lights in lumens. Physical
  camera exposure (EV100) with histogram auto-exposure and eye adaptation,
  so stepping from noon into a torch-lit cave reads the way it should.
- **A real tonemap** (AgX or a fitted ACES), replacing today's identity-below-
  0.8 curve. That curve exists only to keep the external crack detector
  valid, so the detector moves first: holes are found from the depth buffer
  (far depth below the horizon) inside the engine, not from a colour that a
  tonemap is entitled to change.
- **PBR materials**: albedo, roughness, a dielectric F0 of 0.04, metal for the
  rare case, plus a transmission term for leaves and grass. Terrain gets real
  materials per biome through the `material_id` `archetypes.json` already
  carries, blended like heights.

### 2. Atmosphere — Hillaire 2020

"A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
(Sébastien Hillaire, EGSR 2020), as shipped in Unreal: a transmittance LUT,
a multiple-scattering LUT, a per-frame sky-view LUT, and an aerial-
perspective froxel volume applied to every surface. Rayleigh, Mie and ozone,
a sun disk with limb darkening, and a **dynamic time of day**, with a moon
for night. It replaces the clear colour and the `(d / far)^2` fog, and it
is also where the sun's colour and the sky's radiance come from for every
other layer, GI included.

### 3. Local lights — clustered forward

Torches, fires and magic as many dynamic lights: froxel-clustered light
lists built in compute each frame (hundreds of lights, any mix of point,
spot and area-ish), physically based inverse-square falloff windowed to a
range, with flicker and animation driven by gameplay. **Shadowed** local
lights use a shadow atlas (cube faces for point lights, single views for
spots), with a per-frame budget assigned by screen importance, and cached
when nothing near the light moved. In the GI tiers, a cheap ray-traced
shadow can replace the atlas for small, close lights.

### 4. Volumetric lighting — froxel fog

One froxel volume (160 x 90 x 128 at High), temporally reprojected, that
integrates the sun (through the cascades), local lights (through their
clusters and shadows) and the aerial perspective, with height fog and local
media: mist in caves, smoke over fires, spell effects. God rays through the
canopy and torch light in a dusty cave are the same system.

### Two interfaces, decided before any of layers 5–7 is built

GI is split along two seams so that neither the ray source nor the GI
technique is welded to the shaders that use them.

**The tracer** — "what does a ray hit" — is the one place that knows how
the scene is represented for rays:

```hlsl
struct GiRay { float3 origin; float tmin; float3 dir; float tmax; };
struct GiHit {
    bool   hit;
    float  t;
    float3 normal;     // geometric, world space
    float3 albedo;     // for shading the hit, including transmittance for crowns
    float3 emission;   // torches, fires, lava, magic
    uint   material;
};
GiHit gi_trace(GiRay r);             // closest hit
bool  gi_occluded(GiRay r);          // any hit, for shadow rays
```

- **First implementation: DXR 1.1 inline `RayQuery`** against the RT scene
  of layer 5.
- **Planned second implementation: an SDF tracer**, sphere-tracing a sparse
  distance-field clipmap built directly from the density field and the
  brick edits, the way Brixelizer does from meshes. It is not a rejected
  alternative. It is the ray source for GPUs without RT, a cheaper source
  for far cascades, and a cross-check that DXR is not hiding a bug.

Everything that casts rays (probe updates, RT shadows, later reflections and
ReSTIR) does it through these two calls. Swapping the ray source does not
touch a probe or a material.

**The shading-facing GI call** — "how much indirect light arrives here" — is
all a surface shader ever sees:

```hlsl
float3 gi_diffuse(float3 world, float3 normal, float3 view);    // irradiance
float3 gi_specular(float3 world, float3 normal, float3 view, float roughness);
float  gi_sky_visibility(float3 world, float3 normal);          // 0 in a sealed cave
```

Its implementations: **DDGI** (layer 6), and a no-RT fallback of
atmosphere sky irradiance x AO, which is the shape of today's ambient term,
made honest. A later SDF-traced variant or ReSTIR GI slots in behind the same
three functions. Terrain, trees, foliage, water and volumetrics call these,
never a probe texture directly. That is what lets the GI backend change
without editing every shader, which, given this engine's history of
changing its mind after measuring, is the property worth paying for.

### 5. A ray-tracing scene — DXR 1.1

What the first `gi_trace` implementation traces against:

- **Terrain:** one BLAS per resident chunk, built when the chunk is meshed
  (the meshing pipeline already hands its vertices to physics; the BLAS is a
  third consumer). LOD 0–3 near, coarser further out.
- **Trees:** one BLAS per model, instanced, with crowns as opaque proxy hulls
  carrying a transmittance, not alpha-tested cards. Any-hit shaders are slow
  on RDNA3, and probe GI needs a crown's average occlusion, not its leaves.
- **Logs and props:** dynamic instances.
- **Grass:** not traced. Its contribution is folded into the ground's
  albedo.
- One TLAS, refit per frame and rebuilt when chunks swap. Inline
  `RayQuery` in compute (DXR 1.1): no separate raytracing pipeline state.

This is the single largest piece of new infrastructure. It also brings in
**DXC** and SM 6.6, which ADR-003 left open as "a per-shader choice"; the
shader cache already hashes the compiler version, so both compilers can
coexist.

### 6. Global illumination — DDGI

**Dynamic Diffuse Global Illumination** (Majercik et al. 2019; the RTXGI
SDK is MIT-licensed reference code): irradiance probe volumes that trace
rays each frame, store irradiance and depth moments per probe (octahedral,
8x8 and 16x16 texels), and blend over frames.

- **Camera-centred cascades**, about 2 m, 8 m and 32 m spacing, scrolled as
  the camera moves, so detail is near and coverage reaches the horizon.
- **Probe relocation and classification** are essential here, not optional:
  in a terrain world most of a volume is inside rock. Probes inside
  geometry are pushed out or switched off, which also reclaims their rays.
- **Visibility from the depth moments** (Chebyshev) is what stops daylight
  leaking through a cave wall or a dug tunnel's roof: the indoor/outdoor
  requirement, met by the representation rather than special-cased.
- Rays that miss sample the atmosphere's sky-view LUT, which is the sky
  light. Rays that hit shade the hit point with the sun (a shadow ray), the
  local lights (the clusters), and the previous frame's probes, which gives
  infinite bounces for free. A torch in a cave fills the cave; noon sky
  lights a forest floor dimly and green.
- **Screen-space detail stays:** GTAO becomes occlusion of the probe
  lighting only, and contact shadows stay for the sun and shadowed lights.
  Probes are coarse; these give back the creases.

**Why DDGI.** It is the most robust fully dynamic diffuse GI that ships
today. Its cost is independent of resolution and scales smoothly on rays
per probe, probe density and update rate. Its visibility term handles
indoor and outdoor in one system. And it has been shipped on hardware
like ours. The fidelity ceiling it leaves, specular GI and the finest
indirect detail, is what the upgrade path addresses.

### 7. Reflections and specular

Screen-space reflections, with a fallback to the probes (rough) and the sky
(smooth), and ray-traced reflections for water and smooth surfaces at Ultra.
The sea's reflection of the sky comes from the atmosphere, so a sunset sea
is the sunset's colour.

### Upgrade path, deliberately later

**ReSTIR**: ReSTIR DI for many-light direct shadows (hundreds of torches,
all shadowed), and ReSTIR GI or screen-space radiance caching on top of
DDGI for near-field indirect detail, as an Ultra tier. Both are the current
state of the art. Both need the RT scene of layer 5 and a denoiser, which is
why they come after DDGI rather than instead of it.

## Alternatives considered

| | What it is | Why not first |
| --- | --- | --- |
| **SDF tracing (Brixelizer-style)** | Sparse distance-field bricks, no hardware RT, tuned for AMD | **Not rejected: the planned second implementation of `gi_trace`.** Our SDF can come straight from the density field rather than from meshes. It feeds the same DDGI probes on GPUs without RT and for far cascades. DXR is first because its hits are exact at thin cave walls, where a coarse SDF leaks. |
| **Lumen-style** (screen probes + SDF + surface cache) | The current fidelity benchmark | A multi-year system to build. Its parts (surface cache, SDF tracing) would be built twice alongside DDGI. |
| **Path tracing + denoiser** | Ground truth | Not interactive on this GPU at this scene size. Kept as an offline reference mode for validating the others. |
| **Voxel cone tracing** | Voxelised scene, cone-marched | Leaks at cave walls at affordable voxel sizes, and it is superseded by both of the above. |
| **Baked lightmaps / static probes** | Precomputed | Impossible: procedural and editable. |
| **Deferred shading** | Rendering architecture | Clustered forward handles hundreds of lights with alpha-tested foliage and TAA; nothing here needs a G-buffer. A thin one (normals, roughness) may still come for SSR and denoisers. |

## Quality tiers

| | Low | Medium | High (dev default) | Ultra |
| --- | --- | --- | --- | --- |
| GI | sky irradiance x GTAO (the no-RT `gi_diffuse`) | DDGI fed by the SDF tracer | DDGI on DXR, 3 cascades, 128 rays/probe, 1/4 of probes a frame | DDGI 256 rays, all probes, + ReSTIR GI |
| Local light shadows | 2 atlas | 8 atlas | 16 atlas + RT shadows for small lights | ReSTIR DI, all lights |
| Volumetrics | 80 x 45 x 64 | 120 x 68 x 96 | 160 x 90 x 128 | 240 x 135 x 128 |
| Sun shadows | 2 x 1024 | 3 x 2048 | 3 x 2048 + contact | 4 x 4096 + contact |
| Reflections | sky | SSR | SSR + probes | + RT for water |

The current renderer is roughly "Low" with better sun shadows. Every layer
lands with a runtime switch and an A/B number, as everything in this engine
has. The ray counts, probe densities and update rates above are starting
points; tuning them per tier is a measured exercise for after L6.

## Milestones

Each one is measured with an instrument, not judged only by eye.

| | What | Instrument |
| --- | --- | --- |
| **L0** ✅ | DXC alongside FXC; check the device's DXR tier at init; crack detection moved from colour to depth | `--check-rt`; crack counts identical before and after |
| **L1** ✅ | Units, auto-exposure, tonemap, PBR materials, biome terrain materials | a histogram report; A/B stills |
| **L2** | Hillaire atmosphere, aerial perspective, time of day (`--time`, a key) | `--sky-report`: sun and sky radiance against reference values |
| **L3** | Clustered local lights, shadow atlas, a torch you can carry | `--lights-report`: lights per cluster, shadow budget, ms |
| **L4** | Froxel volumetric fog: sun, local lights, height and local media | A/B stills; ms |
| **L5** | RT scene: BLAS per chunk and per tree model, TLAS; `gi_trace` / `gi_occluded` on DXR; ray-traced sun shadow A/B against the cascades | `--rt-report`: BLAS count and memory, build ms, agreement with the CSM |
| **L6** | DDGI cascades, relocation, classification, visibility, behind `gi_diffuse` / `gi_sky_visibility`; every surface shader switched to the interface | `--gi-report`; **`--leak-test`**: dig a sealed chamber, measure its luminance with no light inside (must be ~0), then light a torch in it (must fill it) |
| **L6b** | The SDF tracer: a second `gi_trace` from the density field, A/B against DXR on the same probes | per-ray agreement with DXR; `--leak-test` on both |
| **L7** | SSR and specular, RT water reflections at Ultra | A/B stills |

`--leak-test` is the one that matters most. Leaking daylight into caves is
the characteristic failure of every probe GI, and the eye forgives it until
the day it does not.

## L0 — done, 2026-11-14

- **DXC** compiles SM 6 targets (`cs_6_5` etc.) through the same
  preprocess-and-hash path and shader cache as FXC; the build copies the
  SDK's `dxcompiler.dll` and `dxil.dll` beside `engine.exe` (dxil signs the
  DXIL; D3D12 refuses unsigned). DXC 1.6 from SDK 10.0.22621.
- **The device:** DXR 1.1, mesh shaders, and **shader model 6.5** -- the
  Windows 10 D3D12 runtime stops at 6.5 whatever the card supports. Inline
  RayQuery is 6.5, so L5 and L6 are unblocked; anything wanting 6.6 (dynamic
  resource binding, 64-bit atomics) needs the DirectX 12 Agility SDK shipped
  beside the engine.
- **`--check-rt`:** a one-triangle BLAS and TLAS built on the device, 64 rays
  from an SM 6.5 compute shader with inline RayQuery against the CPU's
  answer: 18 hits expected and reported, 0 disagreements, hit distance and
  barycentrics right to 5e-7.
- **Cracks are counted as UNDRAWN pixels below the horizon:** far depth AND
  both HDR scene targets still at their clear values, read before TAA and
  the tonemap. With post off it equals the old final-colour count exactly
  where that count is sound, and is a subset where it is not (the colour
  count also took distant terrain fogged all the way to the sky colour).
  And it showed the colour detector was ALREADY blind: on the rough hillside
  (2420, 118, 3390) the colour count read 0 with TAA on against 49 real
  pinholes with it off -- TAA and the sharpen move a one-pixel hole off the
  exact sky colour. Far depth alone is wrong too: the sea beyond the clipmap
  writes no depth and counted as 106,000 holes.

## L1 — done, 2026-11-14

- **Units.** `engine/render/lighting_units.h` holds the light once, for C++
  and HLSL alike: the sun at ~100,000 lux, the sky at a mean ~4,300 nits
  (~13,400 lux on level ground), the horizon ~5,000 nits. Shade on level
  ground gets 14% of sun, a clear day. Both scene targets hold radiance in
  nits; every authored colour is linearised before it is lit.
- **The GI interface exists before the GI.** `lighting.hlsli` defines
  `gi_diffuse`, `gi_specular` and `gi_sky_visibility` with the no-RT
  fallback (two hemispheres, sky and ground bounce, exact against the cosine
  lobe; GTAO still applied in post), and terrain, trees, grass and the sea
  all call it. L6 replaces bodies, not call sites. Trees and grass keep a
  fill multiplier (1.3) standing in for inter-leaf bounce -- L6 should
  delete it, and `--leak-test` will show whether anything else leaks.
- **BRDF:** Lambert + GGX (Smith height-correlated, Schlick, F0 0.04,
  metal), Karis's analytic environment BRDF for the ambient specular, a
  wrapped and transmitted sun for leaves and blades, normalised Blinn-Phong
  glints on the sea (the one thing clamped, at 60,000 nits, under half
  float's ceiling: an Inf would poison TAA's history).
- **Exposure:** a 256-bin log2 luminance histogram of TAA's output (every
  second pixel), the mean of its 10-90% band to middle grey 0.18, adapted
  at 3 stops' rate into light and 1 out of it; stills adapt instantly.
  The instrument is the shot report: EV100 spawn 13.7, rough hillside 14.9,
  1400 m vista 15.1, sea view 15.4 -- a sunny scene with shade to a bright
  seascape, which is where a camera would put them (sunny-16 is 15).
- **Tonemap:** AgX with the reference "punchy" look (power 1.35, saturation
  1.4). On four A/B stills base AgX was pastel everywhere and punchy matched
  a fitted ACES; AgX wins on hue for saturated highlights, which torches and
  magic will be. `--tonemap 2` keeps ACES, `--tonemap 0` the pre-L1 path.
- **Biome terrain materials:** archetypes.json `materials` (ground, rock,
  roughness; plus `seabed`), blended per vertex in the mesher with layer
  4's own weights, so material boundaries follow shape boundaries and coasts
  go sandy. The golden gate did not move: the block is presentation.
- **Debug tints bypass exposure** through a mark in the ambient alpha
  carried to the final pass; `--mesh-debug 1` is now exactly magenta.
- **Cost:** walking 2.07 -> 2.08 ms; flight 1.83 -> 2.05 ms mean (GGX over a
  terrain-filled frame, and meshing 0.089 -> 0.102 ms for the material
  blend). Crack counts identical on all four A/B views.
- **Not right yet:** the sea is duller than before (teal came from blending
  in display space); the sky is still one clear colour with a gradient only
  in reflections; the sun never moves. All three are L2's.

## Risks

- **RDNA3 ray throughput.** Mitigated by budgets: rays per probe, probes
  updated per frame, and opaque proxies instead of any-hit for foliage.
- **TLAS churn.** About 1,100 resident chunks and a stream of LOD swaps.
  Refit per frame, rebuild only on swap, and only LOD 0–3 in the TLAS.
- **Probes in rock.** Handled by classification, which has to work before
  anything else in L6 is judged.
- **DXC adoption.** A second compiler with its own includes and bugs. The
  shader cache already hashes the compiler version, and FXC stays for what
  works today.
- **Everything is presentation.** None of this reaches `worldgen/`, the
  golden hash or a save. Invariant 9 is untouched; libm is fine.
