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
| [`docs/starter-prompt.md`](docs/starter-prompt.md) | Bootstrap prompt for the first implementation session. |
| [`CLAUDE.md`](CLAUDE.md) | Invariants and gotchas. Short on purpose. |

## Status

Track A is at **A1**. The field function (layers 0 and 1) and quota assignment
(layer 3) exist; region cells, rivers and 3D density do not.

| What | Where | Run it |
| --- | --- | --- |
| Field function, canonical | `worldgen/shaders/` | compiles as C++ and HLSL; `tools/wgslgen` emits the WGSL |
| Quota assignment | `worldgen/assign/` | portable C++, no host deps |
| Validation spike | `tools/spike/` | `tools\spike\build.bat` then `build\quota_spike.exe` |
| Map view (A1) | `tools/mapview/` | `cd tools/mapview && npm install && npm run dev` |

Two decisions were taken against the original design and are recorded rather
than silently applied: [ADR-001](docs/adr-001-field-function-authority.md)
inverts the field function's language authority, and
[`tools/spike/README.md`](tools/spike/README.md) documents what measurement
changed about layer 3 — including a quota bug that missed its target on every
seed, and a danger field that reads as a dartboard at the documented `w`.
