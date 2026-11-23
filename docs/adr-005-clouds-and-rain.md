# ADR-005: Volumetric clouds and rain

Status: accepted, 2026-11-19. C1 (the clouds), C2 (cloud shadows on the land, the sea and the mist; overcast sky light), C3 (rain) and C4 (the wet world: wetness and puddles from the weather's own rain history, porosity-driven wet surfaces, puddles in real hollows with ripples, drips) and C5 (lightning: a seeded strike schedule, the flash on the land, the mist and inside the clouds, the bolt, exposure held through it, and storms dark enough for it to read) done. C4's puddles reflect the scene in screen space: a puddle mask written by every scene shader, the reflected ray marched in the composite against the depth buffer, the colour from last frame's TAA history. C3 differs from the plan in one place: the canopy test is ADR-004 L6's canopy map (vertical transmittance), not a new top-down depth, so the rain thins under a wood as its sky light does rather than stopping at a hard edge.

## Context

The weather (engine/render/weather.h) drives wind, sea, swell, gusts, the
trees' sway and the shore's swash -- and the sky stays clear through a gale.
The atmosphere (ADR-004 L2) is a clear-sky model: a gale's sea lies under the
same blue as a calm day's. The target is the fidelity of Guerrilla's Decima
skies (Horizon Zero Dawn, Horizon Forbidden West): clouds as a lit volume,
built from noise by type and coverage, moving with the weather, casting
shadows and greying the light; then rain that wets, puddles and ripples.

## Decision

Follow the published Nubis approach (Schneider, "The Real-time Volumetric
Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015; "Nubis: Authoring
Real-Time Volumetric Cloudscapes with the Decima Engine", SIGGRAPH 2017),
with Hillaire's energy-conserving integration and multiple-scattering
approximation (SIGGRAPH 2016), as a raymarched volume in a spherical shell
over the planet the atmosphere already uses (kAtmGround). Forbidden West's
later Nubis^3 (voxel cloudscapes, fly-through superstorms) is NOT the target
of the first phases: it pays for flying inside clouds, which this game's
camera rarely does; it is the upgrade path if that changes.

### The cloud field

- **Noise, generated on the GPU at startup**: a 128^3 tileable Perlin-Worley
  volume (R the base shape, GBA Worley octaves) and a 32^3 Worley detail
  volume. Nothing on disk.
- **A weather map**, regenerated every frame on the GPU over +-64 km round
  the world's centre: coverage, cloud type (stratus 0 .. cumulus 0.5 ..
  cumulonimbus 1) and precipitation, from weather.h's storm dial and a 2D
  noise advected by the clouds' own drift (the wind aloft, faster than the
  ground's). The weather drives the sky; the sky does not drive the weather.
- **Density** (Schneider): base shape remapped by the low-frequency Worley
  octaves, times a height profile chosen by type, coverage applied by remap,
  edges eroded by the detail volume -- billowy at the bottom, wispy on top --
  sheared downwind with height.
- **Layer**: 1.5 km to a type-dependent top (stratus ~0.4 km thick,
  cumulus ~1.5, cumulonimbus to 8 km). Tunable in archetypes.json `clouds`.

### Lighting

- Per sample: a 6-tap cone march towards the key light (sun, or the moon)
  for optical depth; light reaching the cloud from the transmittance table
  (the atmosphere's own) at the sample's altitude; a dual-lobe Henyey-
  Greenstein phase (strong forward, weak back) for the silver lining;
  Hillaire's octaves for multiple scattering (no "powder" hack needed for
  dark edges at that point, but Schneider's powder kept for sun-facing
  billows, as Decima does); ambient from the sky above and the ground below
  by height in the cloud.
- **Integration**: Hillaire's energy-conserving step; early out at
  transmittance 1%; coarse steps through empty space.
- **Distance**: clouds fade into the atmosphere's horizon colour over tens
  of kilometres, as the Decima skies do.

### Rendering

- Quarter resolution each axis, a temporally jittered march, reprojected
  into a history by each pixel's cloud distance and clamped to the new
  frame's neighbourhood (TAA for clouds), then upsampled in the post
  composite: over the sky, and over any surface farther than the cloud.
  Decima renders 1/16 of the full-resolution pixels a frame and reprojects
  the rest; that is the cost upgrade once the look is right.
- The sea's reflections read the composited sky, so clouds reflect for free.

### Phases

| | | done when |
| --- | --- | --- |
| C1 | noise, weather map, march, temporal, composite | clouds of every type drawn, driven by the storm dial, stable in motion |
| C2 | cloud shadows on the ground (a top-down transmittance map, read in shadow.hlsli); overcast light (the key light and the sky's radiance dimmed and greyed by coverage, so the scene is lit as under cloud) | a gale's land lies in grey light with cloud shadows sweeping it |
| C3 | rain: streaks round the camera (GPU, wind-slanted, occluded under canopy by a top-down depth), splashes, the mist thickening | it rains when precipitation is up, not under trees |
| C4 | wet world: surfaces darken and gloss over time with rain, puddles in hollows (noise x flatness), ripples on puddles and the sea | a storm visibly soaks the land and dries after |
| C5 | lightning in cumulonimbus (flash lights the cloud from inside and the land), thunder timing | -- |

## Alternatives considered

- **2D cloud layers / skydome textures**: no parallax, no lighting from the
  sun's angle, no shadows that match: the opposite of the target.
- **Froxel-volume clouds** (extending the mist, L4): the froxels stop at
  6 km and are view-aligned; clouds live at 1.5-10 km altitude and out to
  60 km. Separate march.
- **Nubis^3 voxel clouds** from the start: costs authoring and memory for
  fly-through that the camera does not need. Upgrade path.
- **Physics-driven weather** (clouds deciding wind and rain): the weather is
  already a pure function of seed and clock for saves and replays; clouds
  follow it.
