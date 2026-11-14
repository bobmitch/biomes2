# biomes2

Procedural voxel world generation for a bounded disc world — 10–20 km radius,
tapering to ocean and then void at the edge, with every biome present in every
seed in a known proportion of land area, ordered calm-to-hostile by distance
from the world centre.

Terrain is a pure function, never stored. Only player edits persist.

## Start here

| Document | What it is |
| --- | --- |
| [`docs/worldgen-design.md`](docs/worldgen-design.md) | The design. Read this first. |
| [`docs/handover.md`](docs/handover.md) | Where the project is now, what is open, and the baseline numbers. |
| [`docs/starter-prompt.md`](docs/starter-prompt.md) | Bootstrap prompt for the first implementation session. Historical. |
| [`CLAUDE.md`](CLAUDE.md) | Invariants and gotchas. Short on purpose. |

## Status

**Both tracks are delivered, and so is the merge between them.** Layers 0–7
exist — continent mask, danger field, region cells, quota assignment, height
composition, rivers, 3D density with caves and overhangs, and cave entrances.
The engine walks that world: D3D12, GPU surface nets, an eight-level clipmap
reaching ±7.2 km, Jolt physics, digging that persists, and a sea with
runtime wave height.

The world is pinned at `generator_version` 8. The 200-seed gate runs in CI on
Linux and Windows, and since M2 the eight golden seeds are checked by
`cmake --build` itself — changing a generator parameter fails the build until
the version is bumped.

One roadmap milestone is unticked: **A6**, the archetype tuning UI.
[`docs/handover.md`](docs/handover.md) has the current state, the open work in
priority order, and the baseline numbers to diff against.

| What | Where | Run it |
| --- | --- | --- |
| Field function | `worldgen/shaders/` | ordinary C++ that also compiles as HLSL |
| Quota assignment | `worldgen/assign/` | portable C++, no host deps |
| Validation harness | `tools/validate/` | `tools\validate\build.bat` then `build\validate.exe` |
| World viewer | `tools/view/` | `build\view.exe` from the repo root (Windows) |
| Voxel engine | `engine/` | `build\engine.exe --sea-shot` (Windows; ~20 s to the first frame) |

Decisions taken against the original design are recorded rather than silently
applied: [ADR-001](docs/adr-001-field-function-authority.md) inverts the field
function's language authority,
[ADR-002](docs/adr-002-drop-wgsl-native-viewer.md) retires WGSL and the browser
prototype for a native viewer, and
[`tools/validate/README.md`](tools/validate/README.md) documents what measurement
changed about layer 3 — including a quota bug that missed its target on every
seed, and a danger field that reads as a dartboard at the documented `w`.
