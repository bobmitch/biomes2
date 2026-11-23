# Handover

Where the project actually is, what is open, and what the numbers were when
they were taken. Read [`CLAUDE.md`](../CLAUDE.md) first — the invariants and
the traps live there and are **not** repeated here, and so does the
detail of every engine system. This file is the state of play; that file is
the law. [`gaps.md`](gaps.md) is the full list of what is open, by area.

Rewritten 2026-11-22 at `generator_version` 17, on branch
`near-field-cascades` (PR bobmitch/biomes2#1, not yet merged). The first
version was written 2026-11-13 at version 8. Its ray-tracing, DDGI and
traced-canopy rows went with RT itself: `git show rt-last:docs/handover.md`.

## Where it stands

Both tracks and both merge milestones are delivered. The roadmap in
[`worldgen-design.md`](worldgen-design.md) §Milestones has one unticked row,
**A6**, archetype hot-reload and a tuning UI.

- **Track A** (worldgen), layers 0 through 9:
  - continent mask, danger field, region cells, quota assignment;
  - height composition, with shared micro-relief on calm land (v17);
  - rivers, and flat sites to build on (5b, v17);
  - 3D density with overhangs, and cave entrances;
  - trees (8) and rocks (9).

  **Caves are off since v11**: the code is intact and every `cave_density` is 0. The 200-seed gate is green. Since v16 the world is an archipelago (32% land) and spawn is in the central meadows (`worldgen::find_spawn`).
- **Track B** (engine), D3D12 on an RX 7900 XT:
  - **Terrain and physics**: GPU surface nets over an eight-level clipmap reaching ±7.2 km, with scanned PBR materials, hex-tiling, parallax and a depth prepass. Jolt physics, digging that persists (trees and rocks settle into the hole), and felling.
  - **Things in the world**: a third-person animated character, trees with impostors and a canopy shell past the draw distance, ground cover grown on the GPU, and rocks.
  - **Water and weather**: a weather-driven sea with ripples, whitecaps and swash. Volumetric clouds, rain, a wet world and lightning.
  - **Lighting**: the atmosphere and time of day in physical units, four cascades plus a far terrain shadow, and local lights with shadow cubes. Diffuse GI is a heightfield GI in the open and near-field probes under the ground. Mist, layered exploration fog, and dark vision.
  - **UI**: Lua-scripted RmlUi menus and a live tuning panel.
- **Hardware ray tracing was built and removed** (tag `rt-last`): ~1.2 ms of a 1440p frame and BLAS upkeep for every future dynamic thing. The cascades and the heightfield GI replaced it.
- **M1** — the engine evaluates the real generator. `worldgen/shaders/field.h` compiles as HLSL unmodified. The cost was on the caller's side of ADR-001's line, `engine/field/`.
- **M2** — the golden gate runs inside `cmake --build`, and `--bless` refuses to re-pin a moved world under an unchanged `kGeneratorVersion`.

## Baseline, so a regression is visible

**The generator**: one run of `build\validate.exe` at v17 on 2026-11-22,
ALL CHECKS PASSED in 37 s.

| Check | Value |
| --- | --- |
| Golden seeds | 8 pinned at version 17 |
| Land / islandness | 32.1% mean (seeds 27.6-38.1%); 8.1 landmasses a seed (max 18); the spawn mass holds 66.9% of the land (min 12.8%) |
| Quota | max 6.2% off target (tolerance 10%) |
| Trees /ha | forest 215, taiga 188, meadows 123, wetland 91, highland 37, badlands 13, ashlands 13, glacier 3, rift 1; rank vs configured 0.98 |
| Rocks /ha (boulders / outcrops) | highland 14.4 / 0.47, ashlands 9.8 / 0.28, rift 8.8 / 0.49, badlands 7.6 / 0.17, taiga 5.5 / 0.04, glacier 4.3 / 0.14, forest 2.5 / 0.01, meadows 1.3 / 0.00, wetland 0.8 / 0.01 |
| Flat sites | 19.1 a seed (meadows 9.8, wetland 6.7, forest 2.5), mean radius 26 m; pads level to 0.000 m, steepest shoulder 0.49 |
| Blend seams | 0 seeds over 5 cm; worst 0.019 m base, 0.036 m 3D, over 398,211 crossings |
| LOD agreement | worst 0.39 of a voxel |
| Volumetrics | caves off: 0.00% cave air, 2.6% of land columns overhang |
| Rivers | 356 segments a seed, all descending to the sea |
| Slope budget | worst seed 77.8% of land under 30 degrees |
| Throughput | 187 ns a sample, scalar CPU reference |

**The engine**: these were measured when each piece landed, not together.
CLAUDE.md's section named in brackets has the context. `engine.exe` is a
GUI binary, so a shell does not wait on it: use `Start-Process -Wait`, or
you will read the last run's file. Resolution matters — per-pixel passes
scale 4x from 720p to 1440p.

| Check | Command | Value |
| --- | --- | --- |
| CPU vs GPU field | `--probe-field` | worst 6 um over densities spanning 18 m; a flat site's pad exact, its shoulder 4 um [The engine's own checks; Flat sites] |
| Normals | `--normal-check` | 0.0 deg at every voxel [Gotchas: gradient arm] |
| Cracks | `--shot ...` then `<shot>.txt` | default view 0 undrawn; rough hillside `--cam 2420 118 3390 0.6 -0.25` 0 [Cracks] |
| LOD holes and latency | `--flight 15 --vsync --settle --holes [--flight-speed 15]` | 0 frames with a hole at 15 and 60 m/s; 60 m/s 90th percentile LOD 0 in 3 frames, LOD 3 in 7 [Gotchas: holding] |
| Residency | `--band-report` | 736 chunks wanted (1129 with caves' padding) |
| Tab after flight | `--tab-test` | lands on the ground, 133 terrain bodies |
| Walking at 1440p | `--walk 15 --uncapped --res 2560 1440` | 7.9 ms mean at 23:00 with a torch and two campfires (2026-11-22); 8.37 ms by day after the near field's live list |
| Trees at 4K | `--shot ... --gpu-profile` at `--cam -704 33.1 -448 0 0 --res 3840 2160` | 12.3 ms a frame (24.1 before the prepass work); 1440p 7.9; spawn at 1440p 6.9 [What a wood costs at 4K] |
| Terrain at 4K | `--snapshot build/snapshots/20261122-175849 --shot ... --res 3840 2160 --gpu-profile 60` | the lit pit: terrain 3.49 ms with the prepass (8.05 before the array fix), frame 13.7 [Surface texture] |
| Startup | `build/startup.txt` | ~1 s warm, ~18 s cold (foliage `cs_bake` 7.8 s, `cs_vertices` 4.1 s) [Startup] |
| Digging | `--dig-save` then `--dig-verify`; `--dig-settle` | -0.0000 m, SURVIVED; a tree drops 3.87 m in 0.88 s, a 3.48 m boulder 3.35 m in 0.83 s (free fall 0.89 / 0.83); a dug chunk is redrawn the next frame [Digging] |
| Felling | `--fell --fell-save`, then `--fell-verify` | 9 of 9 swings, the log rests; reloaded 0.0000 m from rest, the tree not standing |
| Overhang | `--overhang` | phase B (a dug dome) STOPS; a no-mesh-shape mutant goes THROUGH [Gotchas: overhang collision] |
| Sea | `--sea-report` | the mean holds sea level to 4.5 mm; deep-water Hs 0.26 m at 3.5 m/s, 0.77 at 6, 1.98 at 10, 3.96 at 20 [Weather] |
| Swimming in a gale | `--swim 40 --wind 20` | 0 frames standing in deep water, eye under 2.3% of frames [Weather] |
| Whitecaps | `--sea-report` (3b, the threshold); a gale still's lower half | 8.3% of a gale frame against Monahan's 10.5% (10.6% before the gusts) [Ripples and whitecaps] |
| Sky | `--sky-report` (CPU only) | vs the IES clear sky from 20 deg up: direct within 2-7%, diffuse within 4-11% [The atmosphere] |
| Sky GPU vs CPU | every `--shot`'s `<shot>.txt` | mean 0.04%, worst 0.12% at noon, 0.52% at sunset |
| Leaks | `--leak-test` | sealed chamber 0.001 nits unlit, 0.053 with a torch; the bare fallback 424, outside 709 [Heightfield GI; Near field] |
| Dark vision | the dead-end tunnel (CLAUDE.md, Dark vision) | adapted EV100 20 m in unlit 1.5 (the floor), with a torch -0.9; moonlit meadow at spawn, 23:00, -0.48 |
| Near field | `--adapt-test --torch`; `--clock-jump`; `BIOMES_NF_PLAY=60 --gpu-profile` | the torch walk's 90th percentile 0.234-0.294 nits (sd 0.010); stairwell 14:00 -> 23:00 within 10% of night in 1 frame; 0.104 ms at 1440p [Near field] |
| Local lights | `--lights-report` (1280 x 720, 23:00) | none 4.07 ms; torch held 4.30; 64 campfires with 24 shadowed 7.74 [Local lights] |
| The body in light shadows | `--pose-sheet --time 23 [--torch] [--fire-ground 0.4 1.7]`; `BIOMES_NO_BODY_LIGHT_SHADOW=1` | walking at 1440p with a torch and two fires 7.92 / 7.98 ms against 7.90 / 7.92, 99th +0.3 ms [The player character] |
| Shadows | `--walk 15 --uncapped [--no-shadows]` | four cascades to 1.6 km for +0.1 ms; wood floor under casters luma 81.3 real leaves / 79.5 cards / 82.2 impostors [Shadows] |
| Canopy shade | `--shot ... [--no-canopy]` | shaded woodland floor 82.6 -> 62.2 at EV100 13; +0.06 ms walking [Canopy shade] |
| Clouds | every `--shot`'s `cloud shadow:` line | sunlit share at noon: coverage 0.17 85%, 0.45 60%, 0.68 20%, closed from 0.93; +0.1 ms walking [Volumetric clouds] |
| Wet world, lightning | `--walk 15 --uncapped --res 2560 1440`, wet against dry under the same weather; a storm with flashes | wet +0.4 ms (reflections ~0.2); lightning +0.05 ms [Volumetric clouds, rain ...] |
| Height fog | every `--shot`'s `height fog:` line | 2% contrast at 20 m above sea: 1.8 km noon, 0.7 km at 07:00; 37 km at 300 m; +0.06 ms walking [The exploration fog] |
| Ground tiling | `tools/profile/tiling.py` on straight-down stills | lattice autocorrelation from 20 m up 0.416 -> 0.001 [Bare ground] |

## What is open, in the order I would take it

[`gaps.md`](gaps.md) has every open item by area, each pointing at its
CLAUDE.md section. These are the ones I would take first.

### 1. Land the branch

PR bobmitch/biomes2#1 carries everything since `main`, including world versions 16 and
17. Merge it only after both jobs of the two-OS matrix pass. Only that matrix catches libm and evaluation-order divergence (invariant 9 and the out-parameter gotcha), and the hashes were re-pinned twice on this branch.

### 2. Play what was judged headless

A great deal of this branch was measured in stills and scripted runs and
never played:
- **Input**: the UI's mouse and keyboard paths, and switching to borderless and back.
- **Settings judged by numbers**: dark vision's floors and time constants were set from timed runs.
- **Motion**: the character against TAA (which has no motion vectors), the swash in motion, and the body's shadow from a torch while walking.
An hour of play with F8 snapshots of anything that looks wrong is the cheapest next step, and each snapshot reproduces headless.

### 3. Underwater

A submerged camera sees the seabed lit as dry land, and rain still draws
under the sea. In a gale the swimmer's eye is under 2.3% of frames, which is
visible. The water constants already carry `cam_water`.

### 4. A6 — archetype hot-reload and tuning

The last roadmap milestone: "someone who is not the author can retune a
biome". The tuning panel (Lua and RmlUi) now exists, but only for engine
tunables, and it does not save. A route through the panel looks shorter now than one through `tools/view`, which is where this file used to put it. It would need three things:
- archetype values as tunables;
- a "write to `archetypes.json`" action;
- a re-bake of what is baked once per seed: the forest map, the fog map, the foliage tiles, and the world itself if a hashed value changes. A hashed value moves the golden hashes, so it is a version bump.

### 5. The 4K budget

All of these are measured, none fixed:
- A dense wood is ~5.5 ms of a 4K frame. `gaps.md` has six ranked candidates.
- The near field's per-pixel reader costs ~1 ms at 4K on a screen of tunnel wall.
- Three campfires in a pit cost ~1.6 ms at 4K.

### 6. Caves, when they come back

They need two things: better seeded placement, and VERTICAL residency. Residency stopped 64 m down while the shafts met cave air at 86 m, so widening the literal in `WorldGpu::column_band` is not the answer. The near field's live list will also need a priority past 8,192 covered probes.

### 7. Older items, not re-checked on this branch

- **`--overhang` phase A** fell to about -719 m walking in towards v11's fallback roof, and its report does not call a fall a fall. Phase B is the decisive test and still stops. Unchecked since v11; the world has moved twice since.
- **The dark patches on distant coarse-LOD terrain** are geometry, at LOD 6–7, over ground whose true surface faces up. Their cause is not settled, and two confident explanations have already been disproved. CLAUDE.md has the discriminator table.
- **Collision mesh at coarse LODs**: mesh readback and physics take LOD 0–2 only (`kCollisionMaxLod`). `cs_heights` still flags coarse chunks; gating its `InterlockedAdd` on the voxel would stop the flagging too. A small saving.
- **A rare long frame in the 60 m/s vsync flight** (48-79 ms against a 22.5 ms 99th percentile), suspected in the tree path. Unmeasured since.
- **Nothing has swum from one landmass to another.** `validate` computes the routes; the engine does not.
- **Trees**: no occlusion culling; pine and fir close-ups are blades, not needles; logs do not split into materials; view.exe and the contact sheet draw no trees.

## How this session worked, and the one thing worth copying

Two Claude sessions shared this checkout, splitting by file. That found three
bugs neither would have found alone — a skirt lip baked into the collision
mesh, a gradient arm that scaled with the voxel, and a nine-commit Windows CI
outage.

The reason is worth stating precisely, because the obvious version of it is
wrong. It was not that we checked each other's work: two sessions running the
same checks would have caught none of them. It was that each of us held an
**instrument built for a different question** — a crack detector that counted
sky pixels below the horizon, a shoreline that comes from the field rather
than from the mesh. A crack detector cannot see a two-voxel collision error
and a shoreline cannot see sky pixels. The redundancy that paid was in the
tooling, not in the checking.

The corollary cost us a day: each change broke the *other* session's
instrument silently. The sea made the crack detector report 5787 cracks. The
lip fed a two-voxel error into a buffer neither of us thought of as shared.
Both were invisible to whoever caused them and obvious from the other end.

If you work alone here, the substitute is to keep building instruments that
answer different questions, and to distrust a proxy when it moves. The lip bug
spent a day diagnosed as a persistence failure because `--dig-verify` reports
a capsule's resting height, and a resting height moving tells you something
moved — not what.
