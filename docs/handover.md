# Handover

Where the project actually is, what is open, and what the numbers were when
this was written. Read [`CLAUDE.md`](../CLAUDE.md) first — the invariants and
the traps live there and are **not** repeated here. This file is the state of
play; that file is the law.

Written 2026-11-13, at `generator_version` 8. The numbers were updated at
version 9 (the continuous cell blend), version 10 (rounded ridged
crests), version 11 (caves off), version 12 (trees) and version 13 (the
trees' river exclusion, which v12 evaluated differently on MSVC and GCC),
with older values kept where they moved.

## Where it stands

Both tracks and both merge milestones are delivered. The roadmap in
[`worldgen-design.md`](worldgen-design.md) §Milestones has one unticked row,
**A6**, the archetype tuning UI.

- **Track A** (worldgen) — layers 0 through 7: continent mask, danger field,
  region cells, quota assignment, height composition, rivers, 3D density with
  caves and overhangs, cave entrances. 200-seed gate green on Linux and
  Windows.
- **Track B** (engine) — D3D12, GPU surface nets, an eight-level clipmap
  reaching ±7.2 km, Jolt physics with a heightfield and mesh-shape path, a
  brick edit overlay that digs and persists, and a sea.
- **M1** — the engine evaluates the real generator. The field function needed
  no change: `worldgen/shaders/field.h` compiles as HLSL unmodified. All the
  cost was on the caller's side of ADR-001's line, the spatial indices, which
  is `engine/field/`.
- **M2** — the golden gate runs inside `cmake --build`. Changing a generator
  parameter fails the build, and `--bless` refuses to re-pin a moved world
  under an unchanged `kGeneratorVersion`.

## Baseline, so a regression is visible

Taken on a clean build at the commit that added this file. Re-run these before
and after anything structural; a number that moves and should not is the point
of having them.

| Check | Command | Value |
| --- | --- | --- |
| Generator gate | `build\validate.exe` | ALL CHECKS PASSED, 8 seeds at version 13 |
| Trees | same | trees/ha forest 213, taiga 189, wetland 70, highland 36, meadows 31, badlands 13, ashlands 13, glacier 2, rift 1 (edges only); rank vs configured 1.00 (v12 on Windows: forest 230 -- river banks were not being kept clear) |
| Tree cost | `engine --flight 10 [--no-trees]` | 0.76 vs 0.39 ms at spawn, 1.03 vs 0.47 ms with `--flight-from -704 60 -448` (densest forest); 1000 m draw distance 0.94 ms |
| Ground cover | `engine --walk 15 --uncapped [--no-foliage]` | 1.83 vs 1.54 ms a frame walking through a meadow; a woodland-edge still draws ~24k near blades, 13k far, 13k cards, 219 bushes, 0 overflowed |
| Cracks | `engine --shot ... ` then `<shot>.txt` | default view 0 undrawn; rough hillside `--cam 2420 118 3390 0.6 -0.25` 0 (was 49: a skirt edge-on to a wall, fixed by leaning it outward); 1400 m vista ~73,800 (the void past the world's rim) |
| Ray tracing | `engine --check-rt` | DXR 1.1, SM 6.5, DXC 1.6; 64 rays, 0 disagreements with the CPU |
| Ray tracing (L5) | `engine --rt-report`; `--shot ... --rt-shadows` | hillside: primary vs depth 93.1% agree, 0.5% nearer; shadow vs CSM 94.4%; woods 76-79%; normals 2.5-3.2 deg near; BLAS+TLAS 0.2 ms, shadow 0.29-1.4 ms |
| Far forest (canopy shell) | `--cam -900 260 -900 0.8 -0.25 --time 16`, `--tree-distance 2000 --no-forest-shell` for the reference; `--no-forest-shell` | forest map baked per seed from layer 8's expectation (1024^2, ~380 ms); far terrain lifted to the canopy and shaded as leaves from 75-100% of the tree distance, in every terrain pass so it casts; colour matched to the real trees within 4/1/8 (rgb); crowns are jittered domes + leaf clusters faded by footprint; the shell's sun is the far terrain shadow (no trees: the cascades double-darkened a band past the trees); the trees' fade dither walks per frame; flight +0.14 ms. `BIOMES_FOREST_CHECK=1` compares the map with layer 8: rivers were the wrong patches (now excluded), 23 -> 3 of 1558 texels. Trees and rocks cull and fade by HORIZONTAL distance (levels still by 3D): a 3D draw distance left bare ground under a high camera. Clearing edges: the lift follows the unsharpened cover (slopes, not mesas) and a slope above its foot shades as canopy with its own face normal |
| Low-sun ovals | `--cam -704 34 -448 0.8 -0.45 --time 18.1` | golden unshadowed ovals on a dark forest floor in the cascades: the shadow lookup was skipped (visibility 1) where geometric N.L <= 0, but the scans' normal map still lit those slopes. Fixed in mesh.hlsl and rocks.hlsl: no sun there, fade over 6 deg |
| Rocks (layer 9) | `validate` (rocks check); `--shot ...` and `--walk` (`rocks:` line, collision probe); `--no-rocks` | v15: boulders 0.4-4 m on 8 m cells, outcrops 4-10 m bedrock shelves on 48 m cells in slopes, set below the footprint's lowest ground; procedural meshes, terrain root signature, rock scans + parallax; cascades, local-light faces, heightfield GI, RT scene (RT shadows, DDGI); static Jolt colliders within 40 m; cost within noise. Grass through rocks and LOD pop left for later |
| Surface scans | `python -I tools/textures/fetch_scans.py`; `--shot ...`, A/B by moving `assets/textures/packed` aside | 22 CC0 ambientCG sets (19 terrain, 3 bark), retinted to the authored colours, height-blended triplanar on terrain; walking +0.06-0.17 ms, startup +132 ms, 247 MB. Parallax on top (`--no-pom`, `P`): depth per set, reach 300 x depth (10-45 m), self-shadow, no silhouettes; +0.12 ms walking |
| Shadows | `--shot ...`; `--walk 15 --uncapped [--no-shadows]` | four cascades to 1.6 km (was 520 m), fade 1.36-1.6 km (was 468-520 m); PCSS (nearest blocker, sun's 0.53 deg); per-cascade res 2048/2048/4096/4096, far two time-sliced; walking 2.41 -> 2.51 ms. Past 1.6 km the far terrain shadow (`--no-far-shadow`; `BIOMES_FAR_DBG=1` shows it everywhere): 8 m heightmap over 8 km marched to the sun, cost within noise walking, +0.15-0.3 ms on a rebuild frame. Tree casters per cascade matched to the real leaves (`BIOMES_TREE_SHADOW_PATH=1|2`): wood floor luma 81.3 real / 79.5 cards (was 100) / 82.2 impostors (was 54) |
| Heightfield GI (L6.5) | `--shot ... [--no-hfgi]`; `engine --leak-test` | badlands shade red 63 -> 70 (DDGI 81); wood floor 58.1 -> 55.1 (DDGI 44.8); sealed chamber without RT 0.001 nits (bare fallback 566); +0.05 ms walking; the default |
| Screen-space bounce (L6.4) | `--shot ... --ssgi`; `--post-debug 3` | shade +1-2% (wood 58.1 -> 59.5, badlands red 63 -> 66 v DDGI 81) for +0.3 ms walking; off by default |
| DDGI (L6.3) | `engine --leak-test`; `--gi-report`; `--shot ... [--no-gi]` | sealed chamber 0.001 nits, its 19 probes active (fallback 566); torch bounce 1.08x (albedo 0.07); DDGI/fallback hillside 1.03, wood 0.72, spawn 0.87; up-facing sky B/G 1.54 v 1.50; update 1.1-1.9 ms; walking +2.8 ms |
| Canopy map (L6.2) | `engine --rt-report` (canopy line); `--shot ... [--no-canopy]` | map vs traced crowns: wood 0.37/0.32 (err 0.066), spawn 0.48/0.51 (0.077), bare 1.00/1.00; shaded woodland floor 82.6 -> 62.2 at EV100 13; +0.06 ms walking |
| Canopy shade (L6.1) | `engine --rt-report` (sky lines); `--shot ... --rt-sky [--no-crowns]` | sky visibility wood 0.90 -> 0.50 with crowns, spawn 0.93 -> 0.59, hillside 0.86 unchanged; shaded woodland floor luma 82.6 -> 58.5 at EV100 13; +1.5 ms in a dense wood |
| Mist (L4) | `engine --shot ... [--time h] [--no-fog]` then `<shot>.txt` | gpu line: inject 0.095 ms day, 0.142 night with lights; integrate ~0.02; shafts A/B at --ev100 13: luma 152 / 176 / 195 (none / shadowed / unshadowed) |
| Local lights (L3) | `engine --lights-report` | night, ground: none 2.33 ms; torch carried walking 2.51; 64 campfires 2.98 / 3.55 shadowed; 255 campfires 4.42, 46.5 per lit cluster, 0 overflowed |
| Sky (L2) | `engine --sky-report` (CPU only) | vs IES clear sky, sun 20 deg up and over: direct x1.02-1.07, diffuse x1.04-1.11; evaluate() 0.18 ms |
| Sky GPU vs CPU (L2) | `engine --shot ... [--time h]` then `<shot>.txt` | 192 texels: noon mean 0.04% worst 0.12%; sunset worst 0.52%; night worst 0.23% |
| Exposure (L1) | `engine --shot ...` then `<shot>.txt` | EV100 spawn 13.7, rough hillside 14.9, `--altitude 1400` 15.1, `--sea-shot` 15.4; histogram printed per stop |
| Lighting cost (L1) | `engine --walk 15 --uncapped`, `--flight 10 --settle` | walk 2.07 -> 2.08 ms mean; flight 1.83 -> 2.05 ms mean, meshing 0.089 -> 0.102 ms/frame |
| Post chain | `engine --walk 15 --uncapped [--no-post]` | 2.14 vs 2.05 ms (effects); 1.83 ms before the HDR frame existed. 60 m/s vsync flight 99th 23.0-23.4 ms against 22.3 with foliage and post off (one 31 ms outlier run) |
| Shadows | `engine --flight 10 [--no-shadows]` | spawn 1.66 vs 0.78 ms; densest forest (`--flight-from -704 60 -448`) 2.66 vs 0.97 ms |
| Felling | `engine --fell --fell-save`, then `--fell-verify` | walk stopped 0.55 m from the axis (bark + capsule 0.54), 9 of 9 swings, log tipped 72 degrees and rested in 9.3 s; reloaded 0.0000 m from rest, tree not standing |
| Blend seams | same | 0 seeds over 5 cm; worst 0.014 m base, 0.036 m 3D, over 398,211 crossings (v8: steps up to 178 m) |
| Land / islandness | same | 35.2% mean, 7.2 landmasses/seed, spawn mass 73.9% |
| Volumetrics | same | caves off: 0.00% cave air, 2.3% of land columns overhang (v10 with caves: 1.72% and 10.6% -- most of what the overhang metric counted was cave air near the surface) |
| CPU vs GPU field | `engine --probe-field` | worst 0.000006 over densities spanning 18 m |
| Frame cost | `engine --flight 10` | 0.37 ms a frame, 2,684 fps, 1.1 M triangles, 0 overflows. Every earlier figure (74–77 fps) was CPU-bound on recomputing `column_band`; see CLAUDE.md |
| LOD holes | `engine --flight 15 --vsync --settle --holes [--flight-speed 15]` | 0 frames with a hole at 15 and 60 m/s (`--no-lod-hold`, the old behaviour: 8.5% and 85.3%) |
| LOD latency | same | 60 m/s, 90th percentile: LOD 0 3 frames, LOD 3 7 frames (hold without priority: 27 and 209); 99th-percentile frame 21.5 ms |
| Tab after flight | `engine --tab-test` | lands on the ground, 133 terrain bodies (was 0 bodies and a fall to -334 m) |
| Slot headroom | same | worst chunk wanted 2440 of 4096 vertices, 12816 of 24576 indices, but a 15 s, 60 m/s flight hit **3948** |
| Walking | `engine --walk 20` | 120 m, feet 1.6 mm mean from the field, worst single-frame step 0.020 m, LOD 0 for all 1110 judged frames (v8: a 0.53 m step, which was a blend seam) |
| Persistence | `engine --dig-save` then `--dig-verify` | −0.0000 m, SURVIVED |
| Sea | `engine --sea-report` | 0.20 m calm to 7.51 m storm, mean holds sea level to 2.4 mm; waterline inversion misses by 0.05 mm |
| Swimming | `engine --swim 40` | 0 of ~611 deep-water frames on the seabed, feet 1.34 m under (balance 1.35), eye never under, back on land |
| Swimming, storm | `engine --swim 40 --sea-state 1` | 0 of ~513 on the seabed, feet 1.29 m under, eye under 10.9% of frames (worst 0.18 m) |
| Residency | `engine --band-report` | 736 chunks wanted (1129 with the caves' 64 m of padding), band clears the surface by 34 m everywhere |
| Normals | `engine --normal-check` | 0.0° at every voxel, control carries 161° at 64 m |

`engine.exe` is a GUI binary: a shell does not wait on it. Use
`Start-Process -Wait`, or read the last run's file by mistake.

**Startup is about 1 s warm, about 8 s cold.** Shaders are cached under
`build/shader_cache/` by a hash of their preprocessed source, so only an
edited shader (or anything it includes, field.h among them) recompiles. A
cold start is a fresh checkout or an edit to the field. It had grown to 45 s,
40 of it one kernel; `build/startup.txt` splits every start by phase.

## What is open, in the order I would take it

### 1. ~~The sea has no physics~~ — done 2026-11-13

The character floats, swims, wades and walks back out. The waterline is
`ww_height_at` in `water_wave.h`, the function the shader draws. Water stays
out of residency (`--band-report` is byte-identical before and after). See
CLAUDE.md's sea section for the three traps it hit.

What remains, in order:

- **No underwater rendering.** A submerged camera sees the seabed lit as dry
  land, with the surface overhead as a translucent sheet. In a storm this is
  10.9% of swimming frames, so it is visible. It needs fog and tint in
  `mesh.hlsl` when the eye is below `cam_water`, which the water constants
  already carry.
- **One `--swim` run came back short of the beach.** It had 1522 frames where
  every other run had about 2390. It was not reproduced in three re-runs, and
  it predates the step-clamp accounting the report now prints. If it recurs,
  read "simulated time lost" first.
- **Nothing has yet swum from one landmass to another.** `--swim` goes about
  50 m out and back. A crossing is the test that would close the gap between
  `boat connect` and the engine. It needs a route between two masses, which
  `validate` already computes and the engine does not.
- No breath or drowning, no boat, and the sea moves no one sideways.

### 1a. ~~Overhang collision fails~~ — it never did; the test was wrong

The capsule slid along an inclined 1 m ledge and off its edge, and the test
judged by height alone. Fixed in the test: verdict by where the head crossed
the underside, plus a dug cavity (phase B) that a working collision must
stop. It stops, and a no-mesh-shape mutant fails. The collision mesh is now
read back for LOD 0–2 only, so an overhang chunk is a solid heightfield for
less time (backlog peak 271 → 45). CLAUDE.md has the detail.

### 1b. ~~Toothed ledges~~ — creases, rounded in v10

NOT LOD seams: the debug tint that said so counted creases as curtain. With
an exact detector (`--mesh-debug 1`/`2`), the rough hillside showed curtain
on 0.48% of the frame and creases on 2.71%. The creases were the ridged
noise's knife-edge crest; v10 rounds it (k = 0.05, chosen by eye), and
creases are 1.8%. Caves are deliberately left sharp. CLAUDE.md has the
detail and the wrong variant to avoid.

### 1c. ~~Rough terrain runs the slot pool dry~~ — fixed

Pool 768 × 4608 → 1536 × 6144. On a 60 m/s rough-terrain flight the old pool
never settled and had 1654 starved frames and 384 early retires; the new
one settled in 3 s with none. `--slot-report` and the flight's starved-frame
count are the instruments.

### 1d. Trees -- what is not done

Placement, drawing, the slider and felling are in, measured above. Open, in
the order I would take it:

- **Shadows are in** (three cascades, terrain and trees; CLAUDE.md has the
  design). What is left: they end at 520 m, so a mountain's shadow on a far
  valley is not drawn; the sea neither casts nor receives; and the far
  cascade could be refreshed every other frame if 1.7 ms over forest ever
  matters.
- **A rare long frame in the 60 m/s vsync flight**: worst 48-79 ms over
  three runs, with or without shadows, while the 99th percentile stays at
  22.5 ms. It was 24 ms in the first tree measurement, so something in the
  tree path hitches occasionally -- a tile batch landing, the instance
  rebuild, the 1 MB re-upload. Unmeasured; start with timing `update_trees`.
- **Occlusion culling.** Culling is frustum and distance only. A Hi-Z pass
  from the terrain depth would drop trees behind hills; it has not been
  needed at ~1 ms a frame.
- **Close-up art for conifers and pines.** Broadleaves read well at every
  distance. Pine tufts up close are blades, not needles, and fir sprays
  could be denser.
- **Logs into materials.** A log is a rigid body with a stable id and a pose
  in the save; splitting it is the hook the user asked for, not built.
- view.exe and the contact sheet do not draw trees.

### 2. Cap MeshShape at the fine LOD levels

**Partly done on the CPU side:** `sync_physics_terrain` now hands physics
LOD 0–2 only (`kPhysicsMaxLod` in `main.cpp`). That cut bodies from 524 to
133 in `--tab-test`. The GPU still classifies and stages mesh shapes for
coarse chunks that are then dropped, so the `cs_heights` gate below would
still save that work.

529 of 1895 chunks (27.9%) are flagged as needing a collision mesh, and 534 of
694 physics bodies are mesh shapes — at coarse voxels where a mesh vertex sits
up to 328 m from the surface it claims to represent.

The character stands on LOD 0 for **every** judged frame, so this is cost and
latent risk, not a measured failure. Be honest about that. B4 measured the
real hazard — 44% of a walk spent on LOD 3 and 4 with feet 1.16 m out — and
XZ-only residency already fixed it.

The fix belongs in `cs_heights` in `engine/meshing/surface_nets.hlsl`, which
already sets the multi-surface flag and already knows `voxel`: gate the
`InterlockedAdd` on a voxel threshold and nothing coarse is ever flagged, so
nothing is staged, so no renderer or `main.cpp` change at all. 2 m (LOD 0–2)
is defensible; 64 m plainly is not. It needs a before/after on `--walk`'s
"standing on LOD n" table and the mesh-shape body count.

### 3. A6 — archetype hot-reload and tuning UI

The last unticked milestone: "someone who is not the author can retune a
biome". Nothing is blocked on it — the contact sheet and relief sheet already
let you judge a retune — so it is third despite being the only one on the
roadmap. What is missing is hot-reload and sliders, in `tools/view`.

### 4. ~~Caves are unreachable in the engine~~ — caves are OFF since v11

Every `cave_density` is 0 in `archetypes.json` (the v10 values are in its
`_note_caves`). Layer 6 carves no tunnels, layer 7 builds no shafts
(`worldgen::caves_enabled`), and the engine's band drops the 64 m it padded
below the surface for them: 1129 chunks wanted became 736. `validate`'s
`cave entrances` check inverts and asserts zero shafts. The code is all still
there; restoring the densities and bumping the version brings it back.

When they return they need better seeded placement AND a fix for what made
them unreachable: residency stopped 64 m down while the shafts met cave air at
86 m. Widening the literal in `WorldGpu::column_band` is not the answer; a
vertical residency scheme is.

### 4a. `--overhang` phase A falls out of the world at the v11 roof

v11's world moved phase A's fallback roof to (-1288, 1069), and walking in
towards it the capsule falls to about -719 m before the test re-places it.
NOT caused by the shallower band: restoring the 64 m pad gives -715 m. Phase
B (the dug dome, the decisive test) still STOPS. Phase A's report does not
flag a fall: it prints "feet while under it: min -719 m" and carries on. Find
where the walk-in leaves the ground, and make the report call a fall a fall.

### 5. The dark patches on distant terrain — narrowed, not explained

`--shot --altitude 1400 --pick ...` put world positions on eight of them. All
eight are on LOD 6–7, and at six the true surface faces UP. So the dark is the
coarse mesh, not the terrain. CLAUDE.md has the detail and the untested next
guess. The near-field dark SEAMS were a different bug, the blend seams, fixed
in v9.

Not a blocker, but do not let anyone call it settled. **Two confident
explanations have already been disproved.** What is ruled out by measurement:
holes, NaN normals, inverted normals, and the normal formula. What is left is
that they are geometry — zoomed in they have a lit sunward rim and a dark
shadow face. Identifying *which* geometry needs a world position for one of
them, which nothing currently reports. See CLAUDE.md for the discriminator
table; it is reusable.

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
