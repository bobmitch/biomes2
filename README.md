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

Design only — no implementation yet. The repository tree described in the
design doc has not been created. Begin with `docs/starter-prompt.md`.
