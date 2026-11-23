# biomes2

Procedural voxel world generation for a bounded disc world — 10–20 km radius,
tapering to ocean and then void at the edge, with every biome present in every
seed in a known proportion of land area, ordered calm-to-hostile by distance
from the world centre — and a D3D12 engine that walks it.

Terrain is a pure function, never stored. Only player edits persist.

## Start here

| Document | What it is |
| --- | --- |
| [`docs/worldgen-design.md`](docs/worldgen-design.md) | The design. Read this first. |
| [`CLAUDE.md`](CLAUDE.md) | The invariants, the gotchas, and how every engine system works and what it measured. The current reference. |
| [`docs/gaps.md`](docs/gaps.md) | What is built but not finished, and what has been seen but not fixed. |
| [`docs/handover.md`](docs/handover.md) | The state of play at v17: the baseline numbers to diff against, and what is open in the order to take it. |
| ADRs | [001](docs/adr-001-field-function-authority.md) field function authority, [002](docs/adr-002-drop-wgsl-native-viewer.md) retire WGSL for a native viewer, [003](docs/adr-003-graphics-api.md) graphics API, [004](docs/adr-004-lighting.md) lighting, [005](docs/adr-005-clouds-and-rain.md) clouds and rain. |
| [`docs/starter-prompt.md`](docs/starter-prompt.md) | Bootstrap prompt for the first implementation session. Historical. |

## Status

**The world** is pinned at `generator_version` 17. These layers are built:
1. Layers 0–5: continent mask, danger field, region cells, quota assignment, height composition (with shared micro-relief on calm land), and rivers.
2. Layer 5b: flat sites to build on.
3. Layers 6–7: 3D density with overhangs, and cave entrances. **Caves are off since v11**: the code is intact, and every biome's `cave_density` is 0.
4. Layer 8: trees.
5. Layer 9: rocks.

The 200-seed gate runs in CI on Linux and Windows. `cmake --build` checks the eight golden seeds itself, so changing a generator parameter fails the build until the version is bumped.

**The engine** (Windows, D3D12) evaluates the same field function on the GPU:
- **Terrain**: GPU surface nets over an eight-level clipmap reaching ±7.2 km, with scanned PBR materials, hex-tiling and parallax.
- **Physics and editing**: Jolt physics, digging that persists, and trees you can fell.
- **The player**: a third-person animated character.
- **Vegetation and rocks**: trees with impostors and a far canopy shell, ground cover grown on the GPU, and rocks.
- **Water and weather**: a sea with weather-driven waves, ripples and swash. Volumetric clouds, rain, a wet world and lightning.
- **Lighting**: the atmosphere and time of day in physical units, cascaded sun shadows, and local lights (torches, campfires) with shadows. Diffuse GI comes from a heightfield GI in the open and near-field probes under the ground.
- **UI**: Lua-scripted RmlUi menus and a live tuning panel.

The one unticked roadmap milestone is **A6**, archetype hot-reload and sliders: `archetypes.json` is still edited by hand.

| What | Where | Run it |
| --- | --- | --- |
| Field function | `worldgen/shaders/` | ordinary C++ that also compiles as HLSL |
| Quota assignment | `worldgen/assign/` | portable C++, no host deps |
| Validation harness | `tools/validate/` | `build\validate.exe` |
| World viewer | `tools/view/` | `build\view.exe` (Windows) |
| Voxel engine | `engine/`, `game/` | `build\engine.exe` (Windows) |

## Build and run

Everything runs from the repository root, which is where the executables
read their data from.

```powershell
git submodule update --init
```

```powershell
tools\validate\build.bat
```

```powershell
build\validate.exe
```

`build.bat` wraps cmake and builds everything; with no submodules the
engine is skipped and the generator and harness still build. To drive cmake
directly: `cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake`.

The engine's surface scans and the player's character are not in git. Fetch
the scans once (about 180 MB of CC0 sets from ambientCG, checked against
`assets/textures/scans.json`):

```powershell
python -I tools/textures/fetch_scans.py
```

The character pack is built from the CC0 Quaternius kits placed in
`assets/models/`:

```powershell
python -I tools/models/pack_character.py
```

Without the scans the terrain uses procedural detail. Without the character
pack you walk with no body drawn. `build/startup.txt` says which is missing
and what to run.

```powershell
build\engine.exe
```

The window opens over spawn, the calmest ground near the centre. First start
compiles shaders (~18 s); after that the cache makes it ~1 s. `--res 2560
1440` sets the size; every mode renders at 1280 x 720 otherwise.

Some keys:

| Key | Does |
| --- | --- |
| Tab | walk (third person) / fly |
| Esc | pause menu (options, controls, tuning) |
| F1 or ` | tuning panel over the running game |
| Mouse wheel | camera distance; all the way in is first person |
| Left / right mouse | swing the axe (fells trees) / dig |
| `,` `.` | dig radius |
| F / V / C | carry a torch / drop a campfire / clear them |
| Page Up / Page Down, H | step the clock / run it |
| `[` `]`, `\` | storm dial down / up, back to automatic weather |
| F6 / F9 | save / load edits and felled trees |
| F8 | snapshot: the view, the edits and the flags to stand here again |

The engine's headless modes (`--shot <path>`, `--walk <s>`, `--flight <s>`,
`--pose-sheet` and many more) render, measure, write a report under
`build/`, and exit. CLAUDE.md lists them. `engine.exe` is a GUI-subsystem
binary, so a shell does not wait for it: use `Start-Process -Wait`.

```powershell
Start-Process -Wait build\engine.exe -ArgumentList '--shot build/shot.bmp'
```

Decisions taken against the original design are recorded rather than silently
applied, in the ADRs above, and
[`tools/validate/README.md`](tools/validate/README.md) documents what
measurement changed about layer 3 — including a quota bug that missed its
target on every seed, and a danger field that reads as a dartboard at the
documented `w`.
