# Handover

Where the project actually is, what is open, and what the numbers were when
this was written. Read [`CLAUDE.md`](../CLAUDE.md) first — the invariants and
the traps live there and are **not** repeated here. This file is the state of
play; that file is the law.

Written 2026-11-13, at `generator_version` 8.

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
| Generator gate | `build\validate.exe` | ALL CHECKS PASSED, 8 seeds at version 8 |
| Land / islandness | same | 35.2% mean, 7.2 landmasses/seed, spawn mass 73.9% |
| Volumetrics | same | 1.72% of subsurface is cave air, 11.6% of land columns overhang |
| CPU vs GPU field | `engine --probe-field` | worst 0.000006 over densities spanning 18 m |
| Frame cost | `engine --flight 10` | 77 fps, 2.32 ms meshing, 946k triangles, 0 overflows |
| Slot headroom | same | worst chunk wanted 2342 of 4096 vertices, 13152 of 24576 indices |
| Walking | `engine --walk 20` | 120 m, feet 3.8 mm from the field, LOD 0 for all 1103 judged frames |
| Persistence | `engine --dig-save` then `--dig-verify` | −0.0000 m, SURVIVED |
| Sea | `engine --sea-report` | 0.20 m calm to 7.51 m storm, mean holds sea level to 2.4 mm |
| Residency | `engine --band-report` | 1129 chunks wanted, band clears the surface by 34 m everywhere |
| Normals | `engine --normal-check` | 0.0° at every voxel, control carries 161° at 64 m |

`engine.exe` is a GUI binary: a shell does not wait on it. Use
`Start-Process -Wait`, or read the last run's file by mistake.

**Startup takes 15–20 seconds** before the first frame — it compiles the field
shaders and builds the whole seed. The process burns a core with no window up
and looks exactly like a hang. It is not one. Do not debug it; wait.

## What is open, in the order I would take it

### 1. The sea has no physics

`physics_world` and `streamer.h` contain no reference to it — checked, not
assumed. A character walks into the ocean and keeps walking along the seabed.

This is first not because the sea is newest but because **the generator is
validated against sea travel the engine cannot perform.** `validate` asserts
`boat connect — 0 seeds with a biome unreachable by sea` on every seed, and
land fraction was deliberately tuned below the percolation threshold to make
an archipelago: 7.2 landmasses a seed with the spawn mass holding 74% of the
land. About a quarter of the world is across water by design. Until something
can cross it, the world being measured as good and the world being played are
different worlds.

Keep water **out of residency** when you do this. `--band-report`'s "worst
miss" compares the band against `worldgen::terrain_height`; a water surface
feeding residency would leave that check passing while measuring something
else. The waterline is a physics question, not a streaming one.

### 2. Cap MeshShape at the fine LOD levels

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

### 4. Caves are unreachable in the engine

Layers 6 and 7 are built and asserted: 23.8 shafts a seed, all reaching cave
air, 0 mouths sealed. But residency stops 64 m below the surface and the
shafts break into cave air at 86 m, so the chunks are not there — walk into an
entrance and you fall out of the world. Two whole layers of generator work
have never been looked at by a human.

This is the expensive one. The 64 m is a literal in `WorldGpu::column_band`
and widening it is not the answer; a vertical residency scheme is. Noted in
the source at the place that will pay for it.

### 5. The dark patches on distant terrain — unexplained

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
