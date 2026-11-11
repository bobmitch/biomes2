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

Track A is at **A4**. Layers 0–5 exist — continent mask, danger field, region
cells, quota assignment, height composition and rivers. 3D density and caves
(layer 6) do not. The world is pinned at `generator_version` 5 and the
200-seed gate runs in CI on Linux and Windows.

| What | Where | Run it |
| --- | --- | --- |
| Field function | `worldgen/shaders/` | ordinary C++ that also compiles as HLSL |
| Quota assignment | `worldgen/assign/` | portable C++, no host deps |
| Validation harness | `tools/validate/` | `tools\validate\build.bat` then `build\validate.exe` |
| World viewer | `tools/view/` | `build\view.exe` from the repo root (Windows) |

Decisions taken against the original design are recorded rather than silently
applied: [ADR-001](docs/adr-001-field-function-authority.md) inverts the field
function's language authority,
[ADR-002](docs/adr-002-drop-wgsl-native-viewer.md) retires WGSL and the browser
prototype for a native viewer, and
[`tools/validate/README.md`](tools/validate/README.md) documents what measurement
changed about layer 3 — including a quota bug that missed its target on every
seed, and a danger field that reads as a dartboard at the documented `w`.
