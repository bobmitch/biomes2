# CLAUDE.md

Procedural voxel world generator for a bounded disc world. Full design in
`docs/worldgen-design.md` — read it before changing anything in `worldgen/`.

## Layout

| Path | What |
| --- | --- |
| `worldgen/shaders/` | The field function. Ordinary C++ that also compiles as HLSL. |
| `worldgen/world.{h,cpp}` | Cell scatter, per-feature seeding, world build, carved terrain. |
| `worldgen/assign/` | Quota assignment (layer 3). Portable C++, no host deps. |
| `worldgen/data/` | `archetypes.json` — the biome table. All tuning lives here. |
| `worldgen/golden/` | Pinned seed hashes. Changes must be deliberate. |
| `engine/trees/` | Tree meshes (procedural, made here), the CPU tile streamer over layer 8, GPU culling and impostors, felling. |
| `engine/foliage/` | Ground cover -- grass, flowers, bushes -- generated on the GPU each frame (`foliage.hlsl`), and `trample.{h,cpp}`, the map of where something has stood in it. |
| `engine/ui/` | The UI layer: RmlUi's D3D12 renderer, input routing, and the `engine` Lua module (tunables). |
| `game/` | What the UI and scripts ARE: `scripts/*.lua` (entry `main.lua`) and `ui/*.rml`, `*.rcss`. |
| `engine/character/` | The player's character: the packed model, the procedural animator, the skinned draw. |
| `tools/models/` | `pack_character.py`: the rigged glTF characters and the animation library in `assets/models/` (not in git) to `assets/models/packed/` (`*.chr`, `ual.anim`). |
| `engine/rocks/` | Layer 9's rocks: procedural meshes, the CPU tile streamer, instanced drawing on the terrain's root signature, static colliders. |
| `engine/props/` | Things to pick up: the caves' glowing mushrooms (`glow_mushrooms.h`), and layer 10's forage plants (`forage.h`): streaming in 64 m zones, picks and regrowth, drawing, picking with E. |
| `engine/player/` | The player's survival: items (`items.h`), the 8 x 4 inventory, health / stamina / food / the game clock (`survival.h`), item icons painted from data (`icons.h`), the player as heard and seen (`stealth.h`). No GPU. |
| `engine/nav/` | The navmesh (ADR-007): `nav_tile.h` builds one Recast/Detour tile from the field's solid spans, the water and the obstacles; `nav_world.h` streams tiles round the player on workers and answers queries. Recast/Detour is the `third_party/recastnavigation` submodule (v1.6.0, zlib). |
| `engine/mobs/` | Creatures (ADR-007, being built): `perception.h`, the arithmetic of hearing (dB) and sight (cone, range, light); `creature_table.h` (`game/data/creatures.json`); `creatures.h`, the zones that spawn, freeze, save and thaw them; `locomotion.h`, how they move; `minds.h`, what they notice (perception on a think tick) and the Lua host for their behaviours (`game/scripts/creatures/*.lua`); `combat.h`, blows both ways. No GPU. |
| `game/data/` | `items.json`: every item, the player's numbers, the clock, the start inventory. Gameplay data, outside the hash. |
| `tools/validate/` | Headless N-seed harness. CI entry point. |
| `tools/view/` | Native world viewer (Win32), Windows only: seeds as maps plus their gameplay numbers. See "The world viewer". |
| `tools/shared/` | Presentation (`render.h`) and gameplay measures of a seed (`seed_metrics.h`), shared by the harness and the viewer. |
| `assets/textures/` | `scans.json`, the CC0 surface scans' manifest. Downloads and packs are fetched, never committed. |
| `tools/textures/` | `fetch_scans.py`: downloads the scans and packs them for the engine. |
| `engine/` | Track B runtime (D3D12, `docs/adr-003-*`). Since M1 it evaluates the REAL field: `engine/field/real_field.hlsl` is the caller's side of ADR-001's line (which cells, rivers and shafts are near a point), and `world_gpu.{h,cpp}` uploads them. |

Open gaps -- things built but not finished, and things seen but not fixed
-- are gathered in `docs/gaps.md`; add to it when a section here grows a
"Not done".

**`worldgen/` is everything upstream of the golden hash; `tools/` is
everything downstream.** That line is what makes invariant 9 checkable — the
metrics legitimately use `atan`, and they are on the far side of it.

## Build

```bash
tools\validate\build.bat && build\validate.exe
```

```bash
build\view.exe
```

The engine's surface scans are not in git; fetch them once (about 180 MB from
ambientCG, checked against the manifest's sha256). Without them the terrain
and bark fall back to their procedural detail.

```bash
python -I tools/textures/fetch_scans.py
```

The player's character is packed the same way, from the CC0 kit in
`assets/models/` (not in git either); without a pack the engine walks with
no body drawn, and `build/startup.txt` says what to run.

```bash
python -I tools/models/pack_character.py
```

```bash
build\engine.exe
```

`build.bat` is a thin wrapper over cmake and builds everything; there is no
`CMakePresets.json`, and the `cmake --preset dev` line that used to be here had
never worked. To drive cmake directly:

```bash
cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake
```

**Spawn** is `worldgen::find_spawn`: the point nearest the centre that
stands well inside the CALMEST biome (lowest `danger_preferred`; weight
>= 0.75 there and a majority on a 60 m ring), on dry, gentle ground, clear of
rivers and trunks. The engine opens the window over it, and `--walk`,
`--flight`, `--fell`, `--tab-test`, `--lights-report` and `--shot`'s default
vantage (70 m behind it, 22 m up) all start from it; `build/startup.txt`
names it. `validate --spawns` lists it per seed (v18: 200 of 200 strict,
median 240 m from the centre, max 1.9 km, ~5 ms). It moves nothing in the
golden hash. Before v16 every one of those started at a fixed (0, 0) or
(10, -70), which on the default seed was wetland. On v18's default seed it
is (736, 41.6, -288).

**v18 moved the world, and this file's coordinates with it.** Layer 0 became
an archipelago (below), so every position, `--cam` line, snapshot and
measured view in this file that predates v18 -- the badlands hillside, the
spawn tunnels at (-96, -440), the densest wood -- was taken on v17's world
and now lands somewhere else, often in the sea. The measurements stand as
records of what was found; re-find a view before reusing it (`--pick`,
`validate --tree-sites`, the viewer's inspector).

**Layer 0** ("land as a lottery with fences", docs/worldgen-design.md)
ships in part since v18: no centre floor, a 7 km regime field (0.028 mask
per std) and a fine 1564 m continent noise weighted 0.63 - 0.34 x regime,
chosen by eye in the viewer -- 34 landmasses a seed, the spawn's holding
22% of the land, 29.4% land, 2.8 flat sites (rare on purpose: building
sites are meant to be dug). The per-seed draws, the shelf and the knee are
still prototype, off by default. `view.exe` has a slider for each part
("Layer 0 prototype..."); `validate` takes a flag for each and prints the
readings under `islandness` (reach on foot, biomes on foot, shelf area,
knee, draws); the engine builds a variant from
`BIOMES_L0="centre_floor=0,regime_amp=0.1,shelf_width=0.06,knee=1,..."`.
One parser, `worldgen::apply_param_spec`, reads that string for the engine
and the viewer (which starts from it if it is set); the viewer's Copy
command writes it. Feature sizes are `*_feat` in metres.

`validate.exe`, `view.exe` and `engine.exe` all read data by paths relative to
the repository root, so run them from there. `engine.exe --shot <path>` renders
one frame at a fixed camera and writes a BMP without opening a window.
Every mode renders at 1280 x 720 unless given `--res W H` (borderless, so
2560 x 1440 fits a 1440p screen exactly); measure costs at the resolution
that will be played -- per-pixel passes scale 4x from 720p to 1440p.

The engine's headless modes all write a text file under `build/` and exit:
`--shot`, `--flight <s>`, `--walk <s>`, `--overhang`, `--dig`, `--dig-save`,
`--dig-verify`, `--dig-stress`, `--check-dialect`, `--probe-field`,
`--band-report`, `--sea-report`, `--swim`, `--dig-water`, `--blend-report`, `--tab-test`,
`--mob-test` (creatures: spawning, freezing, saving, thawing; see Creatures and stealth),
`--nav-report` (the navmesh round the spawn: tiles, coverage, trunks, paths, a swim, a log, a dig, and a map),
`--move-test` (creatures walking, running, swimming, handed from the mesh to physics, frozen mid-walk),
`--mimic-test` (the mimic's script: heard, unheard, seen, chased, stared, lost, by moonlight and torchlight),
`--combat-test` (blows both ways: a windup that lands or is stepped out of, the player's axe, a death),
`--clock-jump` (with `--shot`: the light under the ground after the clock
jumps or runs, `build/time_jump.txt`; see The near field),
`--sky-report` (CPU only, no device: the atmosphere against clear-sky
references), `--pose-sheet` (the character in nineteen fixed poses from fixed
cameras -- idle, walk, jog, sprint, jump, swim, torch, the axe, crouch -- `build/pose_*.bmp` and `pose_sheet.txt`), `--shot ... --gpu-profile [frames]` (per-pass GPU times,
pipeline statistics and the trees' overdraw, `<shot>.gpu.txt`; see Trees),
`--leak-test`
(ADR-004 L6: a sealed chamber, unlit and torch-lit, the heightfield GI against
the bare fallback), `--lights-report` (ADR-004 L3: local lights at night, six
configurations timed), `--fell` (with `--fell-save` / `--fell-verify`), `--dig-settle` and
`--survival-test` (the player's rules and the forage layer; see Survival).
Every mode takes `--seed <n>` (which world; 0 by default, and every view
measured in this file is seed 0), `--character male|female` (which pack; male by default) and
`--time <hours>` (ADR-004 L2: 14 by default, which is the
pre-L2 sun), the weather's `--sea-state <0..1>`, `--wind <m/s>`,
`--wind-dir <deg>` and `--swell <m>` (automatic weather otherwise), `--torch` (carry one), `--no-light-shadows`, `--no-fog` and
`--fog-density <k>` (a multiplier on the authored mist, 1 by default), `--no-far-shadow`, `--no-pom`,
`--aniso <1|2|4|8|16>` (the scans' filtering), `--no-terrain-prepass`, `--no-rocks`,
`--no-forest-shell`, `--no-clouds`, `--no-rain`, `--rain <0..1>`, `--wet <0..1>`,
`--no-lightning`, `--no-height-fog` and `--height-fog <k>` (scales the
exploration fog, on by default, and turns it on); `--shot` also
takes `--lights <n>` (campfires round the camera), `--lightning <dist m>
<bearing deg> [1]` (a strike for the still, at the peak of its first stroke,
the bearing from the camera's forward; 1 in the cloud) and `--ground` (the
`--cam` x and z at eye height on the ground). `--flight` also takes `--settle`, `--holes` and
`--flight-speed <m/s>` and `--flight-from x y z`; `--shot` takes
`--slot-report` and `--shake <frames>` (a still camera through that many
frames with the clock running, then eight consecutive raw frames,
`build/shake_0..7.bmp`, to measure whether the image moves); every mode takes `--mesh-debug 1|2` (curtain, creases) and
the streaming A/B switches `--no-lod-align`,
`--no-lod-hold`, `--no-prioritise`, `--adaptive-budget` and
`--lod-fade <frames>`, and the tree switches `--tree-distance <m>`,
`--tree-debug 1|2|3|4` (tint by level; only LOD 0 / LOD 1 / impostors),
`--tree-lod0 <m>`, `--no-trees`, `--no-shadows`, `--no-foliage`,
`--foliage-debug 1`, and the post chain's `--no-taa`, `--no-ao`,
`--no-contact`, `--no-post` and `--post-debug 1|2|4|5` (AO alone, contact
shadows alone, how each puddle pixel found its reflection, the clouds' shadow on the key light), and exposure's `--ev-comp <stops>`, `--ev100 <v>` (fixed,
no meter), `--tonemap 0|1|2` (raw pre-L1 path / AgX / fitted ACES),
`--agx-sat` and `--agx-power`. `--shot` also takes `--ui "<lua>"`: bring up the UI, run the code
(`menu.open()`, `menu.show_screen('options')`, `tuning.toggle();
tuning.select('Fog')`, `engine.set('time', 19)`) and draw the UI into the
still -- without it a shot holds no UI. `--sea-shot` only PLACES the camera: pair it with `--shot`,
or it falls through to the window loop with no window and never exits. `--walk` takes `--uncapped` to time frames at ground
level, where the grass is, and `--cam-dist <m>` (the third-person camera's
distance, 0 for first person). It is a GUI subsystem binary, so a shell does not wait
on it: run it with `Start-Process -Wait` or the file you read will be the
last run's.

## Invariants — breaking one of these is a bug, not a tradeoff

1. `worldgen/` imports nothing from `tools/` or `engine/`.
2. The field function is ORDINARY C++ that also compiles as HLSL behind
   `worldgen/shaders/field_types.h`. Every function carries `WG_FN` — `inline`
   in C++, empty in HLSL — because more than one translation unit includes
   these headers. Structs are fine and preferred over long scalar argument
   lists. Only the builtins declared in `field_types.h`, which invariant 9
   explains. Kernel code (`surface_nets`, anything with workgroup memory,
   atomics or barriers) stays hand-written per backend: it never needs CPU
   evaluation, so it gains nothing from being shared.
   There is no WGSL and no generator. That was ADR-001's one bet, taken given
   a browser prototype; `docs/adr-002-*` retires it along with the prototype.
   ADR-001's other decisions stand, including why a GPU can never anchor a
   golden hash.
3. Noise is an analytic integer hash. Never a permutation texture — those are
   not bit-reproducible across our two backends.
4. Per-feature seeds: `hash64(world_seed ^ FEATURE_ID)`. Never thread one RNG
   through the layers in order.
5. Base terrain is never stored. The edit store holds player edits only, as
   ABSOLUTE density + material, never as deltas against the base function.
6. World coordinates are centred on the origin: ±R, never 0..2R.
7. No biome names in shader or engine code. The generator reads
   `archetypes.json` and knows nothing about meaning.
8. Never compile the field function with fast-math. FMA contraction breaks
   CPU/GPU parity. The C++ build (`/fp:precise`, `-ffp-contract=off`) is the
   determinism authority: golden hashes come from it, and CPU-to-GPU agreement
   is a TOLERANCE comparison, never a hash. Shader languages permit
   reassociation and fusion at the implementation's discretion — WGSL §15.7.5
   says so outright, and HLSL needs per-expression `precise` to pin anything —
   so an exact check against a GPU is impossible whichever backend ships.
9. EVERYTHING THAT FEEDS THE GOLDEN HASH uses only `+ - * /`, `sqrt`, `floor`
   and comparisons — the field function AND the layer-2 scatter AND layer 3.
   No `pow`, `exp`, `log`, `sin`, `cos` or `atan`: libm is not exactly
   specified and glibc disagrees with the MSVC CRT by an ULP.
   It is not a theoretical worry. `cos`/`sin` in the cell scatter moved cell
   positions by an ULP, which was far too little to change which cell is
   nearest but enough to perturb the layer-4 blend weights, which moved a
   height across a quantum, which moved a river. Three golden seeds in eight,
   and versions 1 and 2 had passed CI *by luck* because the heights happened
   not to straddle a quantum. Only the two-OS matrix ever catches this.
   For directions use rejection sampling normalised with `sqrt`; for shape
   curves use a rational like `flatness_curve` in `worldgen/shaders/field.h`.
   Metrics that never reach the hash (the slope check's `atan`) are exempt.

## Gotchas that have bitten us

- Radial quantities are defined on normalised AREA (r²), not normalised radius.
  Disc area grows as r², so equal-width rings are not equal-area rings
  (`r_k = R * sqrt(k/N)`). Using radius silently skews every biome quota.
- Octave count must fall with voxel size, at the Nyquist limit, with the last
  surviving octave faded rather than cut. Otherwise LOD levels disagree and
  terrain visibly morphs as you approach it.
- Blend heights across biome boundaries, never noise parameters. Interpolating
  octave counts and lacunarity produces mush exactly where the player is looking.
- Changing any generator parameter changes the world. If golden hashes move,
  either revert or bump `generator_version` — saves store edits anchored to the
  base terrain, and a silent change corrupts them.
- A D3D12 buffer bound through a ROOT UAV must be created with
  `ALLOW_UNORDERED_ACCESS`. Root descriptors are not bounds checked and the
  debug layer is not installed on this machine, so the whole of M1's world
  upload missing that one flag showed up as `DXGI_ERROR_DEVICE_HUNG` —
  indistinguishable from an infinite loop in the field function, which is
  where two hours went. A flat-constant shader hanging too is what separates
  the two: that rules out the field and leaves the binding.
- Residency over the real field needs a PER-COLUMN band, and the band must
  sample the FULL height, not layer 0 padded by the biggest biome amplitude.
  That padding made the band 610 m tall over a column whose surface moves
  1.6 m, so the slot pool filled with air and the ground was never reached.
  `engine.exe --band-report` prints the band, the relief, the resulting chunk
  count and whether the sampling misses the surface, at every LOD.
- CI's `actions/checkout` does NOT fetch submodules. A `FATAL_ERROR` in
  CMakeLists for a missing Jolt submodule killed the windows-latest job for
  nine commits while the Linux job stayed green — and the two-OS matrix is the
  only thing that catches the libm divergence invariant 9 exists for. A
  missing submodule now SKIPS the engine instead. Check `gh run list` after
  anything that touches the build, because a half-red matrix looks like a
  working one from here.
- **The view is right-handed: +yaw turns LEFT on screen.** `Camera::basis`
  gives right = (-cos yaw, 0, sin yaw), and the view matrix puts that on
  screen right, so at yaw 0 screen right is -x. Mouse look added dx to yaw
  and walk mode strafed along (+cos, -sin), so both were mirrored, while fly
  mode's A/D, which use `basis()`, were right. Take sideways directions from
  `basis()`, never from a hand-written sin/cos pair.
- **Never read an out-parameter in the expression that writes it.**
  `nearest_river(..., rw, rd) < rw + 1.0f` reads `rw` beside the call that
  sets it, and C++ leaves the operands' order unspecified. MSVC read `rw`
  first (still 0) and kept trees in river channels; GCC called first and
  kept them out. Every terrain sample and every river segment agreed bit
  for bit between the OSes and v12's trees did not: 95 trees in a tile on
  Windows, 42 on Linux. Not floating point at all -- the two-OS matrix
  caught it anyway, which is the case for keeping it. The gate's failure
  report now prints per-layer hashes, per-tile tree counts with reject
  reasons, and the river buffer's hash; one CI run each took it from
  "8 of 8 moved" to the line. Fixed in v13.
- **`SV_VertexID` does NOT include `BaseVertexLocation` on this driver.**
  Every tree model was drawn with its own base vertex and every one read
  model 0's vertices through its own indices. LOD 0 trunks lined up by
  accident -- every species emits a 10-sided trunk first -- so trees looked
  plausible and crowns merely "sparse"; LOD 1 trunks twisted, caps joined to
  tube vertices drew lines to the middle of the screen, and every impostor
  was baked from an oak. The terrain never met it: its indices were always
  absolute. Make indices absolute and pass base vertex 0, so the answer
  does not depend on the driver. `--tree-debug 2|3|4` isolated it to one
  level in one run.
- **An upload heap the GPU reads when the frame RUNS needs one per frame in
  flight.** The mesher's brick header table and brick staging were single
  buffers with two frames in flight. A dig burst too big for one frame's 64
  brick uploads spilled into the next, whose CPU then overwrote the staging
  and the headers the previous frame had not executed yet: bricks copied
  into the wrong pool slots and marked uploaded, chunks reading the wrong
  brick lists -- a channel dug through a beach drawn as untouched sand,
  for good, while physics and the edit store had it. Found by
  a flood test (since removed) that dug 54 bricks at once; 16 at once never
  showed it.
  Both are halved by `begin_frame_bricks(ring)` now. Next to it,
  `push_brick_headers` returned 0 when the table was full and the chunk read
  the first chunk's bricks as its own; `plan_work` now checks the room first
  and holds the chunk a frame (`header_deferrals`), and a capture throws
  chunks away over a brick whose upload was deferred, as the frame loop did.
- **Never read back from an upload heap.** It is write-combined, uncached
  memory. The impostor mip chain averaged each level by reading the one
  above out of the upload buffer it had just written: a 20 s startup for
  work that takes 180 ms in ordinary memory.
- **HLSL's `&&` and `||` do NOT short-circuit** (before HLSL 2021): both
  sides run. `cheap_test && costly_call(...)` pays for the call on every
  pixel and discards it. The near field's reader cost the terrain 0.33 ms
  at 1440p that way with no pixel using the result; a magenta debug paint
  showed none taking the path, which is what made it hard to see. Put
  anything costly inside an `if`.
- **Never index a shader's local array by a value known only at run
  time.** The terrain's `scan_surface` kept its four material candidates in
  arrays (`sc[4]`, `wc[4]`, `tint[4]`, `rscale[4]`) and picked the heaviest
  and the hex-tiled one BY INDEX; FXC kept all four in dynamically indexed
  temps (`dcl_indexableTemp`, 32 float4), which this GPU's compiler puts in
  scratch memory. Written as four named variables: terrain at 4K in a lit
  pit 8.1 -> 6.0 ms, the image the same to 2 levels in 255. It looked like
  "ALU cost": mip bias, fewer fetches and a quarter of the bytes all barely
  moved it. Read the assembly before guessing -- `fxc /T ps_5_1 /E ps_main
  /O3 /Ges /I . engine/render/shaders/mesh.hlsl /Fc out.asm` from the root,
  and look for `dcl_indexableTemp` and `x0[r...]`. An array indexed only by
  `[unroll]` loop counters (`top_two`'s `wts[12]`) is fine.
- **A dual-compile header's OUT parameters must be spelled per language.**
  `T out[1]` decays to a pointer in C++ and works; in HLSL an array parameter
  is passed BY VALUE and the write never reaches the caller. The wave normal
  was uninitialised for the whole first version of the sea, and it did not
  look broken — Fresnel saturated and the water came out exactly the colour of
  the sky at every depth, which reads as a shading choice. Sampling three
  pixels at three different depths and getting one identical value is what
  caught it. Use `WW_OUT_F3` (`T&` / `out T`).
- A D3D12 root signature holds **64 DWORDs**. Root constants cost one each, a
  descriptor costs two. 44 floats of camera state plus a CBV plus ten root
  UAVs is 66, `CreateRootSignature` returns `E_INVALIDARG`, and renderer init
  fails. Put anything bigger than a handful of values in a CBV.
- **Never put a modal dialog on a headless path.** That root-signature failure
  raised a message box behind every other window, and a `--shot` that should
  have failed in two seconds hung for ten minutes with the real error already
  written to `build/engine_error.txt`. Init failures now only box when a
  window is going to be shown.
- A threshold must be normalised against what the LOD actually delivers, not
  against the ideal. The whitecap test compared crest height to the sea's full
  amplitude while the grid was carrying two thirds of it, so foam covered 2.9%
  of a storm and did not move when the threshold changed — twice. Measure the
  numerator before tuning the constant.
- Most of the clipmap meshes to nothing — nine chunks per LOD 0 column and the
  surface crosses one. A chunk that meshes to zero vertices gives its slot back
  and keeps its key: dropping the key would requeue it every frame. Any code
  path that meshes outside the frame loop has to reclaim too: `capture()` did
  not, and from 1400 m up it spent the whole pool on the air around the camera
  and returned an empty sky over 483,590 triangles it never reached.
- There was no skirt pass for five years' worth of commits, while a comment in
  `streamer.h` said there was. Eight LOD levels took a ground shot from 71
  cracks to 569. The first fix was a LIP: the outermost ring of cells dropped
  two voxels. **The lip was itself the V-shaped seam bug.** A chunk's ghost
  cell is its neighbour's cell 0, so the one vertex row two same-LOD
  neighbours share was dropped by BOTH, and nothing covered it. Every chunk
  border was a V groove two voxels deep (1 m at LOD 0, 8 m at LOD 3), with a
  V notch wherever it crossed a ridge. Lip-at-0 A/B made the Vs vanish and let
  1147 crack pixels through. Before that, the lip had also put a character
  two voxels low at chunk edges via the collision mesh, which read as 0.53 m
  of fake non-persistence in `--dig-verify`.
  **Now a real skirt, and surface vertices are never moved.** Same-LOD
  neighbours weld exactly at their shared row. Each border cell gets a second
  vertex two voxels into the solid along −normal and one voxel OUTWARD across
  the chunk face along the surface (never upward), and `cs_quads` hangs a
  double-sided curtain from every mesh-border edge on an X/Z face
  (`border_side`). A skirt vertex's normal is stored at HALF length; shading
  normalises it, and `is_skirt_vertex` tests the length exactly. Do not
  normalise normals in the kernel. The physics patch is compacted without
  skirt vertices, so nothing downstream needs a predicate. It costs +12%
  vertices and +19% triangles, and slots are now 4608 vertices.
  `g_cell_vertex` packs the surface index (low 16 bits) and the skirt index
  (high 16 bits).
- **The toothed ledges are CREASES, not LOD seams.** They were called seams
  for a whole commit, on the strength of a debug tint that marked pixels
  whose interpolated normal was short. A sharp crease shortens it too, so
  the tint counted ridge lines as curtain. `--mesh-debug 1` now flags the
  curtain exactly: the vertex shader passes a 0/1 skirt flag, non-zero only
  on curtain triangles. `--mesh-debug 2` shows creases (short normal, flag
  zero). On the rough hillside at (2420, 118, 3390): curtain 0.48% of the
  frame, creases 2.71%. The creases are the ridged noise's `1 - 2|n|`, a
  knife-edge crest in layer 4's ridged octaves and in layer 6's 3D term,
  which a voxel grid samples as zigzag teeth.
  Two seam fixes built on the wrong diagnosis were measured and judged:
  - **Seam conformance** (blend a ring-edge chunk's density to the coarser
    level's field) moved the curtain from 0.48% to 0.47%. Removed: two
    kernel passes and a remesh-on-ring-move mechanism for nothing.
  - **Ring alignment** is kept. A 7-wide camera-centred ring's edge fell
    mid-way across a coarser chunk about half the time, and the coarser
    level keeps chunks only partly covered, so two meshes of the same ground
    were drawn in one place. Rings are now 8 wide on the coarser grid
    (`--no-lod-align` for A/B). It costs ~2% of the uncapped frame and 22%
    more resident chunks, and removes the double-drawn ground by
    construction. The curtain view cannot see that benefit, so it is
    unmeasured.
  **Crests are rounded since v10** (`kCrestRound` = 0.05 in `field.h`,
  `crest_abs`), in layer 4's ridged octaves and layer 6's overhang term.
  Hillside creases went 2.70% → 1.80% (k 0.1 gave 1.25%), and the teeth went
  with them. It lowers every ridge tip slightly, so ridged land reads a
  little weathered; 0.05 over 0.1 was chosen by eye. The caves pass 0 and
  keep the exact `abs`: `cave_field` thresholds the crest with a smoothstep
  up to 0.97, and a rounded crest topping out at 0.9 would narrow every
  tunnel. `sqrt(n*n + k*k) - k` is WRONG: it lifts the whole ridged field by
  2k, and the A/B camera ended up inside a hill.
- **Rough terrain ran the slot pool dry; it is 1536 × 6144 now.**
  `--shot --slot-report` writes per-chunk vertex and index counts against
  the caps. On the hillside the 768 × 4608 pool held geometry in every slot
  with 96 chunks queued, and 4 chunks were over the cap (max 5250). A
  60 m/s `--flight ... --flight-from 2420 140 3390` never finished settling
  (91 queued after 30 s), had 1654 frames with work and no free slot, and
  retired 384 held chunks early (each a hole). The hole metric saw only 3
  frames of it, because a chunk with no slot is invisible to a metric that
  looks for surface chunks without cover. With the bigger pool: settled in
  3 s, 0 starved frames, 0 early retires, 0 overflows, same frame time.
  ~440 MB against a 20 GB card. The flight report now counts starved frames.
  A variable-size allocator would recover the ~65% of each slot a typical
  chunk leaves empty, if memory ever matters.
- **The biome blend looks at four cells, and its weight reached further than
  four.** `blend_width` (500 m) let a FIFTH cell carry weight, and where the
  4th and 5th traded places along a straight bisector that weight vanished
  in one step. Through v8 that was 791 heightfield steps over 1 m per 900 km
  of transect, up to 63 m. Layer 6 multiplied the same cut into overhang
  walls a metre high, which rendered as the dark serrated seams: a straight
  45° step drawn at 0.5 m voxels zigzags. Two halves fixed it, both in
  `field.h`. `blend_weight` subtracts the weight the 5th cell would carry, so
  every weight reaches zero as its cell ties with the 5th. `blend_divisor`
  floors the land-only normaliser at 5% of the total weight, ocean included.
  Without the floor, a lone land cell among ocean cells kept a 100% share
  until the instant it left. `validate`'s **blend seams** check asserts no
  step over 5 cm, and was mutation-tested against each half.
  **The golden hash could not see any of it** — one sample per 156 m against
  a seam a few metres wide — which is how it lasted eight versions. The walk
  had been stepping over one: worst single-frame step 0.53 m on v8, 0.02 m
  on v9.
  `engine.exe --blend-report` surveys it; `--shot --cam x y z yaw pitch
  --pick px py` names what is at a pixel; `--no-overhang` A/Bs layer 6.
- **The river carve steps where two segments of different widths meet**, up to
  0.37 m, found while building the seam check and NOT fixed: the carve takes
  the nearest segment's width, and that switches on the bisector between them.
  All 5 cases seen were within 35 m of an axis, which is unexplained.
- **A LOD change must not retire the old chunk before its replacement is
  drawn.** It did: retirement was immediate and replacement waited on an
  8-chunk-per-frame queue, so at 60 m/s 85% of frames had ground missing
  (`--flight 15 --vsync --settle --holes`). Now an unwanted chunk is HELD,
  drawn, until no CORE chunk queued overlaps its footprint. It then
  dither-fades out over 16 frames while its replacements fade in on the
  same clock, with complementary 4×4 thresholds, so each pixel is drawn by
  exactly one. Holes went to 0 at 15 and 60 m/s. Physics still drops the
  chunk at once. A capture settles every fade first, so `--shot` shows no
  fade. `--no-lod-hold` restores the old behaviour, and `--lod-fade <frames>`
  sets the length.
  **Measure holes from a settled world:** without `--settle` a flight spends
  its whole length on the initial load (a 1200-chunk backlog) and counts
  that as holes at every level.
- **Holding traded holes for latency until the queue learned what matters.**
  The first hold waited for a coarse chunk's WHOLE footprint, and 84% of what
  gets meshed is air or solid rock in the band's overhang and cave padding.
  LOD 0 detail at 60 m/s took 27 frames at the 90th percentile, up to 55.
  The queue now meshes CORE chunks first (inside the column's sampled
  surface range, from `column_band`'s `core_lo/core_hi`), nearest first, with
  a boost for chunks that complete a held swap. Holds wait on core chunks
  only. Result: 3 frames, max 5. LOD 3 went from 209 to 7, with holes still
  0. `--flight --holes` prints latency and deficit per LOD;
  `--no-prioritise` restores distance-only order for A/B. The cost is the
  99th-percentile frame, 20.1 → 21.3 ms under vsync, consistent over runs.
  Overhang-displaced surface outside the core band is meshed after the core
  and does not hold a swap, so it can show a late patch. The hole metric
  probes the 2.5D height and cannot see those.
- **`column_band` made the engine CPU-bound for its whole life.** 25 full
  height evaluations per column, ~392 columns, every frame, for a static
  world: about 13 ms of a 13.6 ms frame. Every fps figure before this,
  including the handover's 77, was measuring that loop. Cached per column,
  the uncapped `--flight 10` frame is 0.37 ms (2,684 fps), with the same
  1.1 M triangles. A/B'd by disabling only the cache lookup.
- **The per-frame mesh budget is wired to more than the queue.**
  `kReadbackChunksPerFrame` must be at least the budget: a chunk meshed past
  the readback's capacity never reports its heights, so physics never gets
  it, and an EMPTY chunk is only noticed through that readback, so its slot
  is never returned and the pool fills with air. It was 8 = 8. Both now come
  from `kMeshBudgetMax`. `--adaptive-budget`, which grows the budget while
  GPU meshing is under 4 ms, is opt-in: it bought a frame or two of latency
  and took the 99th-percentile frame to 27 ms through some CPU cost that
  grows with chunks per frame and is NOT in the meshing timestamps. Not
  found yet; the height-residual check was tried and capped, and was not it.
- **Physics must receive terrain every frame, walking or not.** A chunk's
  heights reach physics once, in the frame its readback lands, and
  `Renderer::render` clears those events at the start of the next frame. The
  window loop synced only while walking, so Tab after any flight dropped the
  character into a physics world with no ground: 0 bodies, feet at -334 m
  six seconds later. `step_window_physics` syncs every frame, and
  `--tab-test` drives that same function. Physics now takes LOD 0–2 only
  (`kPhysicsMaxLod`): the character is always inside the LOD 0 ring, and
  bodies at coarse LODs cost about 1 ms a frame in flight for ground nobody
  can reach. 524 bodies became 133.
- **Overhang collision was never broken: the test misjudged a slide.**
  v9's terrain gave `--overhang` its first real ceiling, a 3.75 m slab, and
  the test said the capsule "passed straight THROUGH". A ray cast up at the
  launch hit a LOD 0 mesh shape 1.62 m over the head. Logging the feet showed
  why it still got past: the capsule hit the inclined underside and SLID
  sideways along it (0.2 → 2.5 m in ten frames, as collide-and-slide should),
  cleared the edge of a roof only 1 m wide, and rose past it. The verdict had
  been judged by height alone. It now takes WHERE the head crossed the
  underside: inside the roof's footprint is a failure, outside is a slide.
  The search prefers a roof at least 3 m wide and level underneath, and says
  when it had to fall back (no such roof within 1.8 km of spawn on v9).
  **Phase B digs its own ceiling** so the verdict cannot depend on the
  world: a 3.5 m sphere 7 m under flat ground, the capsule launched from its
  floor. A dome funnels any slide back to its apex, so a working collision
  MUST stop it. It does: the head peaked 0.43 m under the apex. With mesh
  shapes withheld (the mutant), phase B says THROUGH.
  The real weakness was latency. Until its mesh shape lands, an overhang
  chunk is a heightfield, which is SOLID up to its top surface, and the 2-a-
  frame mesh readback was spent on coarse chunks physics then discarded. Now
  only LOD 0–2 are read back (`kCollisionMaxLod`): the backlog peak went
  from 271 chunks to 45.
  `PhysicsWorld::probe_up` (rays up, with and without back faces, plus the
  kind of body hit) and `chunk_shape` are the instruments.
- **A gradient arm must not scale with the voxel.** `field_normal` took a
  central difference at half a voxel, which is 0.25 m at LOD 0 and 32 m at
  LOD 7 — a difference between two unrelated pieces of terrain, not a
  gradient. 14.8° of mean error at a 64 m voxel and 6% of normals pointing
  more than 90° from the surface's own. The density function already fades
  octaves by `voxel`, so the field is smooth at coarse levels and a SHORT arm
  is the correct gradient there. `engine.exe --normal-check` measures it, with
  the CPU at the shader's own arm as a control: a large error with a zero
  control means the formula is wrong rather than the GPU.
- A short gradient arm inside a fully carved cave finds **no gradient at
  all** — layer 6 pins density at exactly `-cave_carve` — and `normalize(0)`
  is NaN, which shades to pure black. Guard it.
- **The dual-compile field function is expensive to inline, and the budgets
  bite without warning.** `field_normal` holds six whole field evaluations;
  twelve is `error X4505: Sum of temp registers ... exceeds limit of 4096`.
  FXC inlines everything, so a second call to the field is not a small change.
- A chunk's index cap must track its vertex cap at about **6 per vertex** — a
  closed quad surface has roughly as many quads as vertices. A flat 16384
  against 4096 vertices overflowed on chunks the vertex cap was happy with,
  and an overflow is dropped geometry, which is a hole.
  `--flight` prints what the worst chunk actually wanted.

- **A normal map must not light what the geometry turns away from the
  sun.** Under a low sun, golden ovals of unshadowed light lay on a dark
  forest floor in the cascades but not under `--rt-shadows`. The terrain
  (and rock) shader skipped the shadow lookup where the GEOMETRIC N.L was
  <= 0 and used visibility 1 -- harmless while the shading normal was the
  mesh's, but the scans' normal map still caught a 6-degree sun on every
  gentle dip facing away from it. Found with a temporary shadow-debug
  view: the cascade index lit the ovals as "past the last cascade", and a
  distance view showed them at "over 50 m" where the floor around was
  20 -- both were the skip's 1, not a distance. Now no direct light where
  the geometric N.L <= 0, faded in over its first six degrees
  (`saturate(lam * 10)`). Daylight barely moves: the badlands at 14:00
  differ on 1% of pixels, mean luma 98.46 -> 98.57.
- **`--ground` can put the camera inside the terrain** where the 2.5D
  height is not the real surface (overhangs, the badlands): 233k-520k
  undrawn pixels in a still. Give the eye height explicitly -- `--shot ...
  --pick 640 360` from above names the ground under the centre.
- **TAA shook the picture by up to a pixel, worst far from the origin.**
  Two bugs in one reprojection. It rebuilt a pixel's world position with
  the JITTERED inverse, so a still camera fetched history from
  `uv - jitter` and the accumulated image moved by each frame's jitter. And
  it inverted a float view-projection whose translation is kilometres from
  the origin: the inverse's error changed with every jittered matrix, so
  the shake grew as you travelled outward and "started late in a run". Now
  the CPU composes ONE matrix in double, this frame's unjittered clip space
  to last frame's (the identity for a still camera, wherever it is), and
  `cs_taa` multiplies clip by it. `--shake` measured it with phase
  correlation between consecutive raw frames: the badlands hillside, 4 km
  out, moved up to 1.0 px a frame, more than the jitter itself (0.81 px);
  now at most 0.014 px there and at spawn. The mist's own reprojection
  (`fog.hlsl`) still goes through float world space; it is low-frequency
  and has not shown it.

### Startup, shaders and the cache

- **Shaders are cached** under `build/shader_cache/<hash>.cso`. The hash covers
  the preprocessed source with its includes, the entry point, target, flags
  and compiler version, so editing a shader or anything it includes (field.h)
  recompiles it and nothing else. Warm start ~1 s, cold ~18 s, most of it
  foliage's `cs_bake` (7.8 s) and the mesher's `cs_vertices` (4.1 s).
  `--no-shader-cache` forces compiles; `--check-dialect` always compiles,
  because compiling is the thing it checks. `build/startup.txt` times every
  shader and init phase on every start.
- **How a field call is written decides compile time.** Startup had grown to
  45 s, 40 of it `cs_vertices`, because `field_normal` wrote its six field
  evaluations out longhand and FXC compiled six whole copies of the field.
  The same six calls in a `[loop]` compile in 3.3 s with bit-identical normals
  (`--normal-check` 0.0° at every voxel). `[unroll]` on that loop does NOT
  compile (X3511, too large), so a "faster unrolled" variant is not
  available.
- **A failed compile looked like a cache miss.** The log line was written
  before the result was checked, and a failed compile is never cached, so
  each run "recompiled". It now logs FAILED. That run also hung: the report
  modes (`--band-report`, `--probe-field`, `--normal-check`,
  `--blend-report`) were missing from the headless list, so the failure put
  up the invisible modal box the gotcha above warns about. They are headless
  now.
- **The stale-report trap caught this session too.** A flight that failed at
  init left the PREVIOUS run's `flight.txt` in place, and its numbers were
  read as the failed variant's. Delete the report before the run and check
  `build/engine_error.txt` after; numbers identical to the digit to an earlier
  run are the tell.

### Snapshots: standing exactly where someone was (F8, `--snapshot`)

F8 in the window writes `build/snapshots/<yyyymmdd-hhmmss>/`: `edits.bin`
and `trees.bin` (F6's saves), `shot.bmp` (the view as a still) and
`snapshot.txt`, whose `args:` line is command-line flags -- `--cam`,
`--time`, the weather fixed as it was (`--sea-state --wind --wind-dir
--swell`), `--wet` (the ground's wetness is the weather's HISTORY: fixing
the dial alone replayed a soaked tunnel dry, and dry soil is twice as
bright), `--ev-comp`, `--torch`, `--rain`, and `--snap-walk` with the feet
and camera distance when walking -- plus the exposure the still adapted
to. `engine.exe --snapshot <dir>` appends those flags to its own command
line (anything typed first wins), loads the edits and trees, and opens
there (walking again once the ground has streamed in, 90 frames);
`--snapshot <dir> --shot <path>` makes the still. `--shot ...
--write-snapshot` writes one headless (its folder in
`build/last_snapshot.txt`). Campfires (V) are kept as `--fire x y z`,
one each; `--fire-ground x z` puts one on the edited ground under a point
(a pit's floor, for a test). V itself now lands a fire on the EDITED
ground, found down from eye height: it used the generated height, so a
fire dropped into a pit floated where the surface had been. Not kept:
logs' poses beyond trees.bin, the clock's own weather.

### UI and scripting: Lua and RmlUi

Esc opens the pause menu (resume, options, controls, tuning panel, quit) and
stops the game clock, physics and the camera; the frame keeps drawing. F1 or
` toggles the tuning panel over the running game. Submodules: `third_party/
lua` (5.4, compiled as C++ so errors unwind), `RmlUi` (6.3, static, with its
Lua plugin), `freetype` -- all built inside the engine block only; worldgen
and validate do not see them.

- **C++ routes, Lua decides.** `engine/ui/ui.cpp` feeds Win32 input to RmlUi
  and calls `game.on_key` (Esc, F1, `) and `game.on_update`; what any key or
  button DOES is in `game/scripts`. While paused the UI takes every key and
  click; otherwise it takes the mouse only while uncaptured, and a click on
  no element falls through to the game (and captures).
- **Tunables are the API.** `main.cpp`'s `register_tunables` lists named
  numbers with a range, getter and setter; `engine.tunables()` hands them to
  Lua, which builds the options screen (group "Graphics", saved to
  `build/settings.lua`) and the tuning panel (every other group, live, NOT
  saved) from the list. A new knob is one line there and appears in the UI
  with no script change. The fog layers are shown as sea-level visibility
  (3.912 / sigma) and thickness.
- **Display**: windowed (client size) or borderless (a popup over the
  monitor, the swapchain at the chosen size and stretched by DXGI; WM_SIZE
  keeps the chosen size). `--res` on the command line beats saved settings.
- **The renderer** (`ui_render_d3d12.cpp`) is one pipeline, drawn last onto
  the back buffer through its own descriptor heap via `Renderer::set_overlay`;
  `capture()` skips it unless `--ui`. No clip masks: never centre with
  `transform` -- an overflow box inside a transformed element then does not
  clip (the options list spilled over its Back button until the menu was
  centred with flexbox).
- Script errors and RmlUi warnings go to `build/ui_log.txt` (no console). A
  UI that fails to start leaves the engine as before (Esc releases the mouse,
  then quits) and says why in `build/engine_error.txt`.
- Fonts are RmlUi's sample Lato and Roboto Mono (OFL), read from the
  submodule; the family is `LatoLatin`, not `Lato`.

### Surface texture

`T` in the window, or `--texture 0|1|2|3`: flat, the scans (default), a
metric grid (1 m lines, heavier every 10 m), scans and grid. `M` toggles
wireframe. The window title shows both.

**The scans** are CC0 PBR sets from ambientCG -- albedo, normal, roughness,
occlusion, height -- listed in `assets/textures/scans.json` and fetched and
packed by `tools/textures/fetch_scans.py` (1K, mips built in linear light,
normals renormalised). `engine/render/scan_library.{h,cpp}` uploads them as
two Texture2DArrays (heap [67..68]) with a per-biome table at [69].
archetypes.json chooses them: each material's `ground_scan` and `rock_scan`,
each species' `shape.bark_scan`. Presentation only; the golden gate
confirmed it twice.

- **The authored colour stays.** A scan's albedo is RETINTED by authored /
  scan mean (both linear), and its roughness scaled to the authored value,
  so a biome looks the colour it was tuned to and the GI and the
  heightfield's top-down pass -- which still read the per-vertex
  blended colour -- see the same mean. The scan brings structure only.
- **Per-vertex material WEIGHTS**, not colours, drive it: the mesher writes
  a second parallel buffer, `g_vertex_wts`, one byte per biome and one for
  the seabed (12 at most), from the same four-cell `blend_weight` as the
  colours. Interpolating material ids across a triangle is meaningless;
  interpolating weights is not. +113 MB of pool.
- **The terrain shader** takes the two heaviest biomes, each its ground
  and its rock by slope (up to four candidates), samples each triplanar
  (whiteout normal blend, u mirrored on back faces, a plane under 1% skipped)
  and blends them by HEIGHT as well as weight (`kScanBlendDepth` 0.2), so
  rock breaks through sand at a slope change instead of cross-fading. The
  scan's normal shades; the geometric normal still decides and offsets the
  shadow lookup. Its occlusion multiplies ambient only.
- **Bark** uses the trunk's uv, which is already in metres (around, along),
  with a tangent frame from screen-space derivatives -- the tree vertex has
  none. The birch keeps its procedural bark: no scan in the set reads as
  birch, and a generic cracked bark lost the dark dashes that make one.
  Impostors keep the procedural bark; the retint keeps their mean equal.
- **A material with no scan falls back** to the old noise detail, per
  material; a checkout with no packs renders exactly as before. startup.txt
  says how many sets loaded and what to run if any are missing.
- **Cost**, walking (`--walk 15 --uncapped`, packs present vs moved aside):
  2.75 / 2.89 ms against 2.69 / 2.72; startup +132 ms; 247 MB of textures
  at 1K. The flight's median rose to 2.50 ms with far more terrain on screen.
- **What the scans cost at 4K, and the filtering option.** Measured on the
  lit pit snapshot (`build/snapshots/20261122-175849`, `--res 3840 2160
  --gpu-profile 60`): the terrain pass 8.05 ms before, 5.61 after the
  array fix (the gotcha above) at 4x anisotropy; flat (`--texture 0`) ~3.4.
  What is left of the scans is texture work, not arithmetic: one cached
  texel per fetch costs what flat does. Of it, anisotropic filtering is the
  biggest part: per level, terrain 5.10 (trilinear), 5.61 (4x, the
  default), 5.96 (8x, the old fixed sampler), 6.16 (16x); 4x and 8x differ
  on 0.04% of pixels by more than 8 levels. Graphics, "Texture filtering",
  saved; `--aniso <n>`. It covers terrain, rocks, the character and bark.
- **The terrain draws a depth prepass** (`record_scene`; `--no-terrain-prepass`
  and the Terrain tunable for A/B). The chunks draw in slot order, not near
  to far, so every wall behind a wall was shaded and painted over: 1.63
  terrain pixel-shader invocations per screen pixel in a dug chamber. Now
  `vs_main` with `ps_depth` (the LOD fade clip, nothing else) writes depth,
  0.11 ms at 4K, and the colour pass tests EQUAL with no write -- the
  trees' scheme. Measured at 4K: the dug chamber (`build/snapshots/
  20261122-192039`) terrain 8.44 -> 5.74 ms, frame 14.6 -> 11.9; the lit pit
  5.59 -> 3.49, frame 15.7 -> 13.7; a 1440p flight's median 5.90 -> 4.92;
  invocations exactly 1.000 a pixel; stills the same to 2 levels; flight
  holes 0. Rocks and the character keep their LESS, writing pipelines and
  test against it. **The fade clip must be identical in both passes**
  (`lod_fade_clip`, shared). It was checked by forcing every chunk
  mid-fade: the pit's still matched with and without; the chamber's
  differed on 1.4% of far pixels, scattered singly, where the forced test
  (not complementary, unlike a real fade) drew two LOD levels over the same
  ground and their depths rounded equal -- LESS keeps the first, EQUAL the
  last. A real fade's dithers are complementary, so no pixel is drawn twice.
- **s5 is a TABLE, not a static sampler**, on the terrain's and the trees'
  root signatures: `Renderer`'s sampler heap holds one sampler per level,
  made once and never rewritten, and the option only changes which one the
  table points at (`scan_sampler()`), so it is live with no pipeline
  rebuilt. Limiting it in the shader instead (lengthening the short
  gradient) was built and measured: ~480 instruction slots for 14 inlined
  calls, and 8x -> 4x saved 0.1 ms where the real sampler saves 0.35.
  **Every `SetDescriptorHeaps` goes through `bind_shader_heaps`**
  (`shader_heaps.h`): the call unbinds every heap it is not given, and a
  pass that bound only the CBV heap would leave the terrain, rock and tree
  pipelines with no sampler -- with no debug layer here to say so. The
  UI's own heap, drawn last, is the one exception. Every
  `SetGraphicsRootSignature` of the terrain's or the trees' root sets the
  table next to it.
- **Not done**: anti-tiling on the placed rocks (terrain rock has it: see
  "Cliffs" below), BC compression -- 247 MB of scans at 1K would be ~62 MB, and the BC7
  proxy (no fetch reading the top mip, a quarter of the bytes) took
  0.35-0.4 ms off the 4K terrain pass: worth it for memory, not the frame --
  scans for the grass, leaves and placed rocks, silhouettes.

**Bare ground: hex-tiling and far detail** (archetypes.json `ground_look`,
the tuning panel's Terrain group, `--no-ground-look` for A/B). A 2.5 m
scan repeated on one square lattice read as rows from ~10 m out and a
wallpaper grid over a clearing. Three parts (`scan.hlsli`, `mesh.hlsl`):

- **Hex-tiling** (Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022,
  `hex_sample`): a hex lattice over the uv, every hex its own random
  offset and rotation (the rotation from the hash as a direction, no
  trigonometry), three hexes per pixel blended by barycentric weight^7
  biased by luminance; the normal map's xy turned back by each rotation.
  Only for the heaviest GROUND candidate, only in its dominant triplanar
  plane -- everywhere was +1.1 ms at 1440p -- and a corner under 1% of the
  heaviest weight is not fetched, so most pixels take one sample. No
  ground parallax under it: the march reads the height map straight and
  would not match a turned scan (ground parallax was 3-5 cm to ~10 m).
- **Far detail** (`scan_far_ratio`): the same scan at 5.3 x its tile,
  hex-tiled, as a colour ratio to its own mean, multiplied over the near
  scan from 10 m out (`far_detail` 1.2): clumps, bare patches and colour
  shifts over metres, from the photograph rather than from noise. Two mips
  coarser than the footprint asks: it is structure, the near scan is grain.
- **Macro variation** (`ground_macro`): value noise at 97 and 41 m, a 63 m
  dry/lush noise (dry warmer and paler, towards straw or dust -- going to
  the luminance read as grey ash), and the terrain's curvature: hollows
  darker and lusher, crests paler. The curvature is a channel of the
  heightfield GI's map (R32G32: height, curvature over 6 m), written once
  a texel by `cs_hf_light`; five height reads a pixel cost 0.15 ms.
- **What did not work, and why it is gone.** The first attempt FADED the
  scan to its mean from 30-110 m and laid noise and a far-grass colour on
  top. It removed the grid by removing the detail: the ground past 50 m was
  a flat colour, which read worse than the repetition, and the 10-50 m rows
  were untouched. The fade remains only as a far LOD (350-700 m, under the
  haze). Far grass changed nothing visible in meadows and cost 0.1 ms;
  removed. Value-noise octaves at 17 and 7 m went when the far detail took
  that band: every noise in this shader is paid by every pixel.
- **Measured**: `tools/profile/tiling.py` on straight-down stills (the
  autocorrelation at the tile's lattice lags after a high-pass, against
  half-period lags): from 20 m up 0.416 -> 0.001, 60 m 0.240 -> -0.016,
  140 m 0.105 -> -0.024 (baselines 0.00-0.05). Cost at 1440p: the terrain
  pass +0.18 ms in a walk-like view; walking 6.69 / 6.53 against 6.36 /
  6.30 ms. Profile THE WALK'S view, not an eye-level still: the third
  person camera sees far more ground at 5-40 m, and an eye-level profile
  understated the first version's cost by half.
- **Flags**: `--no-hex`, `--far-detail <k>`, `--macro-var <k>`,
  `--detail-fade a b`. Not `--ground-...`: `--ground` is a flag and
  `find()` matches it inside one -- an A/B still was taken at eye height
  for that.

**Cliffs: rock hex-tiling and a far rock layer** (archetypes.json
`rock_look`, the Terrain group's "Rock:" knobs, `--no-rock-look` for A/B).
A 5 m rock scan along a cliff read as evenly spaced vertical streaks from
~20 m out, and as stripes at 140 m. Both parts are `scan_rock`
(`scan.hlsli`), for the HEAVIEST rock candidate only (`mesh.hlsl`):

- **Hex-tiling in EVERY triplanar plane**, not just the dominant one: a
  slope near 45 degrees weighs two planes alike and the dominant one swaps
  across it, so hex in one plane would draw a seam there. Each hex turns
  at most `hex_turn_deg` (15) either way, not at random: rock's streaks and
  beds follow gravity. Parallax follows the hexes (`scan_pom_shift`'s
  `hexed`): it marches in the frame of the hex the point is most in.
- **The far layer**: the same scan at `far_scale` (5) x its tile, always
  hex-tiled, offset in the world, one mip coarser than its footprint --
  cross-faded in over `far_fade` (15-60 m) to a `far` share of 0.7, then to
  all of it by 4 x 60 m, where the near scale is not sampled at all.
  Colour, height and NORMAL: past ~40 m the near normal map has gone to its
  mips, and the far layer's ledges and cracks are what a cliff reads by. It
  goes in before the height blend, so rock breaks through ground at the far
  scale too.
- **Code size was the cost, not the fetches.** Written as hexed
  `scan_triplanar` calls at three sites (two rock candidates and the far
  layer) it cost +0.55 ms on a cliff filling half a 1440p frame; the same
  samples with the far site's hex code removed came in UNDER the baseline.
  One `[loop]` over (scale, plane), so `hex_sample` is written once: +0.38.
  Rock fetches a hex corner only from 10% of the heaviest weight
  (`kRockHexCut`; ground 1%): -0.06.
- **Measured** on the F8 snapshot `build/snapshots/20261126-083030` (a
  ~47 degree coastal bank, Rock060), from 20, 60 and 140 m along its normal:
  the high-passed autocorrelation at one tile's lag along the wall went
  0.347 -> 0.043 (baseline 0.04), 0.390 -> 0.230 (baseline ~0.26), 0.632 ->
  0.320 (baseline ~0.39) -- the excess at the tile is gone at every
  distance. **Cost** at 2560 x 1440, terrain pass: the snapshot's own view
  0.96 -> 1.02 ms; the bank filling half the frame at 20-40 m, the worst
  case, 1.69 -> 2.07. `--no-rock-look` matches the pre-change still within
  run-to-run noise (tree shadows).
- **Flags**: `--no-rock-look`, `--no-rock-hex`, `--rock-far <k>`,
  `--rock-scale <x>`, `--rock-turn <deg>`.
- **Not done**: the placed rocks (`rocks.hlsl`, outcrops to 10 m) keep the
  plain scan; strata and streaking as procedural colour over a cliff (a
  look pass, unique per cliff by construction).

**Parallax occlusion** (`engine/render/shaders/pom.hlsli`; `P`, `--no-pom`)
on the scans' height channel, terrain and bark. No silhouettes and no depth
write: outlines stay the mesh's, so nothing downstream (TAA, AO, the crack
count, physics) sees it. `depth_m` per set in `scans.json` says how many
metres the height map's range stands for: ground 3-5 cm, rock 15-18 cm,
bark 1.5-3 cm.

- **Terrain**: the heaviest of the four candidates, in the triplanar plane
  the normal faces most; its shift moves every candidate's sample point.
  Not when that candidate is hex-tiled ground (in practice, rock only):
  the march reads the height map straight, under a scan turned per hex.
  Only where one plane clearly dominates (`dom`): where two weigh alike the
  dominant one flips pixel to pixel and the shift with it. Every triplanar
  fetch now takes EXPLICIT gradients from the unshifted position, or the
  shift's discontinuities pick wild mips.
- **Bark**: the trunk's metre uvs and the derivative frame already there.
- **Self-shadow** towards the key light, six taps from the hit, multiplying
  the direct term only (the shadow-map lookup keeps the geometric normal),
  faded out as the light grazes, where six taps stride across the map and
  find occluders that are not there.
- **Reach scales with depth**: 300 x depth_m, 10 to 45 m, fading over the
  far half -- 3 cm of meadow stops at 10 m, 15 cm of rock carries to 45.
  A flat 45 m cost +0.33 ms walking, mostly meadow under grass, where 3 cm
  cannot be seen; scaled, **+0.12 ms** (2.93 / 2.96 against 2.82 / 2.82).
- **What it looks like**: it reads on rock at a grazing angle -- the badlands
  cliff at (2453.4, 106.5, 3402) yaw 1.91, 09:00, ledges that hide what is
  behind them and shadow what is below -- and barely under grass. A thin
  high ridge in a height map hides a dark band of the map's floor behind it
  from a grazing view; that is the effect working, and Rock029's 20 cm made
  it look like a smudge, so it is 15 cm.

**Hardware tessellation was built, measured and REMOVED (2026-11-27), and
nothing in the engine tessellates.** A hull/domain pair on the terrain
displaced the near LOD 0 chunks along their normals by two octaves of value
noise, per biome, in the band below the 0.5 m voxel that the scans'
parallax (3-18 cm) does not reach. It worked -- crack-free, no shimmer,
flight holes 0 -- and **it was not worth seeing**: +0.34 ms at 1440p
(terrain 1.275 ms against 0.985, its prepass 0.120 against 0.086), walking
6.35 against 6.08 ms, for ground that reads almost the same, because the
scans' normal maps and POM already carry everything at that scale. What it
taught, so the next attempt starts further on:

- **The amplitude is capped at a boot sole by everything else.** Physics,
  the navmesh, the grass bake, rocks, trees and forage all stand on the
  UNDISPLACED ground, and only the camera's two passes were tessellated
  (the cascades and the rest keep `vs_main`, which is only honest while the
  displacement stays inside the shadow bias). 2-9 cm is what that allows,
  and 2-9 cm at a 0.5 m period is not visible against the scans.
- **Halving the subdivision kept 96% of the image change**, because the
  look came from the analytic gradient of the noise turning the shading
  normal, not from the triangles. Geometric displacement only buys the
  silhouette and real self-occlusion, which at this amplitude is nothing.
  **Any detail at or below the scan's own scale belongs in the pixel
  shader or in a better height map, not in geometry.**
- **The band that is actually empty is 1-18 m, not sub-voxel** (below), and
  that one belongs in `layer4_detail`, where collision and the navmesh
  would get it too.
- Two mechanics are worth remembering if geometry ever is displaced: a
  patch edge's tessellation factor must come from that edge's two endpoints
  alone, through their midpoint (`(a + b) * 0.5` is commutative in IEEE, so
  both patches compute the same float), and with that plus a displacement
  that is a pure function of world position, skirt curtains need no special
  case -- a curtain quad's top edge IS a surface edge.
- **Check which biome a test site actually weighs before judging ground.**
  The first site, an open-looking "meadow" at (808.9, 26.6, -873.8), is
  about 90% SEABED by vertex weight -- an ocean cell's share of the blend
  is what makes coasts sandy -- so with the seabed amplitude at 0 the
  effect was invisible there and the feature looked broken for an hour.

### Trees

**Placement is layer 8, in `worldgen/`, under the golden hash (v12, fixed
in v13 -- see the out-parameter gotcha).** One
candidate per 4 m cell; the cell's hash decides the jitter, the keep test,
species, height, yaw and mesh variant, so a tree is a pure function of its
cell and `tree_id(i, j)` is its save identity -- nothing is stored. Per biome,
`archetypes.json` gives `trees.density` (chance a cell in a patch holds a
tree) and `trees.cover` (share of the biome that is patch, the rest clearing,
from a 3-octave fBm thresholded at its MEASURED quantile -- `validate
--tree-quantiles` printed the table `tree_cover_threshold` bakes; the guessed
table was 4 points of cover out). Species weights are relative; each species
has an altitude band, and its upper end is what moves broadleaf to conifer
uphill and makes the treeline. A tree stands on the REAL surface, bisected
inside layer 6's overhang displacement: testing for air and rock 0.75 m
either side of the 2.5D height rejected 80% of forest candidates. For trees
the biome blend weights are raised to the 4th power (`blend_sharpen`) so a
forest's trees do not reach into the next cell over.

**Placement is authoritative on the CPU.** The engine streams 64 m tiles
round the camera on three worker threads (`TreeField`); the GPU only culls and
draws. A GPU evaluation of the keep test could land an ULP the other side of
its threshold (invariant 8), which is a tree you can see and not chop.

**Drawing** (`TreeRenderer`): CPU-generated meshes, two levels per species x 4
variants, in metres at the species' nominal height; a compute pass culls
against the frustum and the draw distance, picks LOD 0 (< 45 m), LOD 1 (< 170
m) or an octahedral impostor, with a dithered cross-fade at each boundary and
a dithered fade over the last 12% of the distance, and appends to per-draw
lists for two ExecuteIndirect calls. Impostors are baked at startup from 8x8
hemi-octahedral views, premultiplied, mip-chained on the CPU (~180 ms).
Trees use the terrain's sun, sky term and fog. The draw-distance slider is
`-`/`=` (100-2000 m, default 500); terrain is unaffected. Cost, uncapped:
~0.4 ms a frame at spawn, ~0.6 ms over the densest forest.

**Trunks stand on roots** (`root_flare` in `tree_mesh.cpp`): a lobed tube
round the base, from 0.6 m under the ground to 3.5 x the trunk's radius +
0.25 m up, widening concave to (1 + 0.85 x `shape.roots`) at the ground in
4-6 buttress ridges of random strength, normals from the lobes' own slope;
the strongest send a root out along and into the ground. Its randomness is
a COPY of the tree's generator, so every tree keeps its branches.
**The flare was BLACK, and its winding was why.** The bark shader flips
the normal of a back face (`ps_mesh`, for double-sided cards), and the
flare's rings run counter-clockwise where `tube()`'s run clockwise:
wound like `tube()`, every face of it was a back face, its normal pointed
into the trunk, and it shaded black -- a dark skirt with teeth where it
poked through. The first "fix" chased the teeth (an inset too shallow for
the trunk's facets: the middle of a flat face is cos(pi / sides) of the
radius out, 5% in at ten sides, 13% at six), was judged on a still where
the black base passed for shade, and missed it. Both are fixed; the inset
is now the facet depth + 3%, and each ring follows the trunk's axis.
Hand-built geometry on this shader: wind it as `tube()` does, and check a
SUNLIT close-up.
archetypes.json `shape.roots`: beech 1.5, oak 1.3, willow 1.1, maple and
spruce 1.0, fir, alder and snag 0.9, pine 0.8, birch 0.5, scrub 0.
Presentation only (validate's golden hashes unchanged); walking cost within
noise. The flare is bark, in the casters like the trunk.

**Distant leaf cards are solid clusters, not a halftone.** Once a card is
too small for its leaves (`blur * 6`, ~30 m on), its coverage used to be
the pattern's average tested against the screen-fixed dither: the dots
stayed still while the leaves slid under them, so TAA dropped its history in
motion and mid-distance crowns went see-through and blurred; and every card
tested against the SAME screen pattern, so overlapping cards kept the same
pixels and a crown never got denser than one card. `leaf_cluster`
(`leaf.hlsli`) takes over on that switch: per-card value-noise blobs at a
fixed 0.5 cut, set so a card keeps leaf_coverage's own 55%.
**Willows are the exception, and were a regression twice.** Blobs on a
tall strand card read as fat painted tongues; streaks within it as
hatching; one solid strip per card as blocks. A willow's `narrow` cards keep
their own sharp leaves at every distance, cut at 0.5 -- fixed to the card,
so TAA averages them into a soft curtain. And the willow's LOD 1 was a
DIFFERENT TREE: 4-5 branches a limb and one strand 1.7x as wide where LOD 0
had 7-8 and three, so it drew a different number of randoms and every
branch after the first moved. Now both levels have the same branches and
the same three strands, each from a generator of its own (`srng`, one
draw from the tree's), so the far level's fewer, longer cards hang where
the near level's do. Cost: +0.25 ms walking in a willow wood. Sky through the
crowns' upper third of a woodland still: 10.7% before, 10.6% after. Lit pass
only -- the shadow casters keep `leaf_coverage`. Walking cost within noise. The level cross-fades and far
fade still dither on the screen (walking per frame), and the grass's clump
cards fade against the blades the same way.

**What a wood costs at 4K, and the depth prepass.** `--shot ...
--gpu-profile [frames]` is the instrument (`gpu_profile.h`): `<shot>.gpu.txt`
gives the frame's GPU time as it runs (first to last timestamp), then every
pass timed alone behind a NULL UAV barrier (so the passes serialise), the
pipeline statistics of each draw (PS invocations are the overdraw actually
PAID, after early depth), and one frame through counting variants of the tree
pixel shaders (`ps_mesh_od`, `ps_impostor_od`: per pixel, fragments
rasterised and fragments surviving the dither and leaf tests, leaf and
impostor shares) with a heat map, `<shot>.overdraw.bmp`. The test view is the
densest stand of seed 0 (`validate --tree-sites 0`: (-704, -448)),
`--cam -704 33.1 -448 0 0 --res 3840 2160` -- the eye height from `--pick`,
because `--ground` put it inside an overhang and the first profile measured
trees hidden behind rock (0 PS invocations, which is how it showed).
`tools/profile/prof_ab.ps1 <label> "<flags>" [res] [cam|none]` runs one variant.
- **Where it went, before**: 24.1 ms a frame, 6.8 without trees. The camera's
  LOD 0 draw alone 8.6 ms at **13.1 PS invocations per screen pixel**: 42
  fragments rasterised per pixel, max 261, and 14.9 surviving the leaf test
  per covered pixel, every one fully shaded (PCSS, GI, local lights) and
  then painted over. Tree shadow casters 3.9 ms (cascade 0's LOD 0 2.0, its
  LOD 1 1.2). Terrain 3.5 ms, mostly ground later hidden by trunks. Not the
  cause: distance (`--tree-distance 200` 24.0), impostors (alone 7.9),
  foliage (`--no-foliage` 24.0), the level (LOD 0 only 18.4, LOD 1 only
  17.3, `--tree-lod0 0` 20.3 -- LOD 1's cards are fewer but larger, the
  same layers per pixel). `--no-shadows` 16.4.
- **The depth prepass** (`record_prepass`, before the terrain;
  `--no-tree-prepass` and the Trees tunable for A/B): the mesh levels drawn
  depth-only with the dither, cut and leaf tests (`ps_mesh_depth`), then
  colour over that depth with EQUAL, no depth write and
  `[earlydepthstencil]` (`ps_mesh_eq`, no tests: depth already said). Both
  run the SAME vertex shader so the depths match bit for bit; a uniform
  `depth_only` flag skips the vertex GI in the prepass. Impostors stay one
  ordinary pass. Colour LOD 0 went to 0.76 invocations a pixel, and terrain
  drawn after the prepass 3.5 -> 1.7 ms. 22.4 -> 16.3 ms.
  **Underground the terrain goes first.** Looking level along a tunnel the
  whole wood overhead is in the frustum, and the prepass rasterised and
  leaf-tested its cards over half the screen before the rock that hides
  them all was drawn: the LOD 1 prepass 2.89 ms at 4K, the frame 14.9 ms.
  With the eye's open sky (`HeightGi::camera_open`, geometric) under 0.3
  the terrain draws before the prepass and its depth rejects them: 0.25 ms,
  the frame 11.5. The colour pass tests EQUAL, so the picture is the same
  (two runs of one order differ as much as the two orders).
  `BIOMES_TREES_FIRST=1` keeps the old order. `--gpu-profile` also prints
  the wall clock a frame (a still's frame waits for the GPU, so wall minus
  GPU is the CPU's share).
- **Level-major draw slots**: every model's LOD 0, then every model's LOD 1
  (`tree_cull.hlsl`'s `mesh0/mesh1`). Interleaved per model, a far LOD 1
  drew before the near LOD 0 in front of it; 24.1 -> 22.4 without the
  prepass.
- **The leaf test was the cost, not the raster**: with a trivial test the
  prepass's LOD 0 is 1.4 ms against 4.65. Three fixes, in `card_coverage`
  and `leaf.hlsli`: the clusters blend in from a tenth of the way (k was
  never 0 -- ~0.02 at 10 m at 4K -- so every near leaf paid for both
  patterns): 4.65 -> 4.12. Fragments where the card's round rim is under
  0.5 return before the leaves -- 36% of a card, EXACT, since every pattern
  is scaled by the rim: 4.12 -> 3.10 (it barely helps the shadow casters,
  whose 40-texel cards put corner and middle in one wave). One hash per
  leaf cell, three 10-bit draws, where it was three hashes of three integer
  multiplies each: 3.10 -> 2.51, and cascade 0's casters 1.95 -> 1.33. The
  leaves land elsewhere; shape, size and density are the same (mean luma of
  the 4K still 101.55 both).
- **Cascade 0's LOD 1 casters take the card path** (`ps_mesh_shadow`, packed
  bit 29): a tree past 45 m is in cascade 0's box but its shadow mostly
  lands past the 28 m slice. 1.16 -> 0.33 ms.
- **After**: 12.3 ms at 4K (prepass off, the rest kept: 18.9); 1440p 7.9
  (11.6 without the prepass); spawn at 1440p 6.9 against 9.1 -- the prepass
  wins in thin woods too. Remaining at 4K: post chain 3.3, prepass 3.0,
  tree shadow casters 2.3 (cascade 0's LOD 0 1.3), terrain 1.7, tree colour
  1.3. Cracks 0, as before.

**Felling** (`TreeFelling`): trunks within 40 m of the player are static
capsules; a click swings from the eye; 2 + one per 6 cm of diameter fells it.
The tree's id is marked felled, a stump (static) and a log (Jolt rigid body:
a 650 kg/m^3 trunk capsule and a 6 kg/m^3 crown sphere) replace it, both drawn
from the same mesh split at `kCutHeight`. F6/F9 save/load `build/edits.bin`
and `build/trees.bin` (same field id). `--fell` walks into a trunk, fells
it and watches the log; `--fell-save` then `--fell-verify` in a second
process checks the log is where it rested and the tree does not stand.

### Far forest: the canopy shell

Past the trees' draw distance the TERRAIN is the forest: lifted to the
canopy and shaded as crowns wherever trees stand. Placing trees is the cost
that grows with area (a CPU decision per 4 m cell), and nothing a kilometre
away needs to be collidable, so the far forest is the placement's
EXPECTATION rather than its draw.

- **The forest map** (`engine/render/forest_map.{h,cpp}`): 1024^2 over the
  disc (~20 m texels), baked once per seed on every core (~380 ms) from
  layer 8's own terms short of the per-cell rolls -- the blend sharpened as
  trees sharpen it, density x patch noise, the species' altitude fit (so the
  treeline is the trees'), shore, slope and rivers. Per texel: cover
  (crowns dropped at random, 1 - exp(-n x crown area)), mean canopy height,
  conifer share, leaf albedo. Heap [70..71], t47..t48 in the terrain's
  table.
- **`BIOMES_FOREST_CHECK=1` checks the map against layer 8 itself**
  (`build/forest_check.txt`): every 16th texel each way, every tree cell in
  it through `tree_at_cell` -- trees grown against trees expected, texels
  the map calls wood where layer 8 grew none (and the reverse), and what
  rejected the cells. It found the visible wrong patches: rivers. Left out
  at first as "narrower than a texel", channels up to ~40 m run through
  woods, and the shell laid canopy straight over them -- 23 of 1558 sampled
  texels wood-but-bare, their cells rejected mostly by `river`. The map now
  removes the share of each texel inside a channel's width + 1 m (layer 8's
  own rule): 23 -> 3, the rest the rolls' own variance. The slope test
  became a share falling off over 0.7-1.3 x the limit, not a cut on a
  twenty-metre average: bare-but-wooded 11 -> 9. Trees grown / expected
  0.92 -> 0.98. Bake ~380 ms (the river lookups).
- **The shell** (`forest.hlsli`, `mesh.hlsl`): `vs_main` lifts up-facing
  terrain by cover x height x 0.85, faded in from 75% to 100% of the tree
  draw distance -- measured from the camera in the shadow constants, so
  every pass that draws the terrain with `vs_main` lifts it the same way:
  the cascades, the far terrain shadow and the heightfield GI's top-down
  map all see the shell, and far woods cast long evening shadows. That
  needed the terrain root's shadow CBV and table visible to the vertex
  shader. Coming in from 88% to 130% left a pale band past the trees where
  neither was whole.
- **Shading as leaves.** `ps_main` blends the canopy over the ground by the
  shell's weight, lit as `trees.hlsl` lights leaves -- `leaf_sun_light`
  (wrap, transmission), diffuse GI, no specular. On the ground's BRDF it
  read pale and blue beside the real trees: the specular reflected grey sky
  that crowns do not. The sun and sky shares (`kShellSun` 0.7, `kShellSky`
  0.85, leaves shading leaves) were matched over the shell's own footprint
  against the trees drawn to 2 km, no mist, fixed exposure: real (88.9,
  114.8, 71.1), shell (85.3, 114.9, 63.5) -- no tint needed. Measuring
  with mist, or over boxes chosen by eye, moved nothing: the mist adds the
  same grey to both, and the boxes held as much meadow as forest. The mask
  came from a temporary view of the map's own cover and weight.
- **Crowns** are jittered domes, one per cell, 7 m (4 m conifers), the
  tallest winning where they overlap, each with its own tone, and leaf
  clusters a fifth that size on them. Each scale fades into its average as
  its pixel footprint grows -- a procedural mip chain. The first version's
  value-noise bumps read as soft square blobs, low resolution beside the
  real trees, and unfaded they aliased into ripples a kilometre out. Cover
  goes through `smoothstep(0.15, 0.55)`: a wood's edge is a few metres and
  the texels twenty, so bilinear cover blurred every edge and clump.
- **The shell's sun comes from the FAR terrain shadow**, not the cascades:
  it holds terrain and the lifted shell (drawn with `vs_main`) but no trees.
  The shell already darkens itself for leaves shading leaves, and the
  cascades also hold the real trees, whose last row cast onto the shell
  behind it: a dark band between the trees and the shell. Weighting the
  cascades by the shell's fade did not remove it -- the shadows fell
  beyond the band.
- **The trees' distance-fade dither walks per frame** (`keep_pixel`'s
  `frame`, lit passes only), so TAA resolves a fading tree into a fade; the
  fixed 4x4 screen door read as a checkerboard at the border.
- **Cost**: walking within noise; a flight from 260 m, median 2.12 / 2.13
  against 1.98 / 1.98 ms; startup +380 ms (the map bake).
- **One metric for the hand-over: HORIZONTAL distance.** The trees' cull
  (`tree_cull.hlsl`), their far fade, the rocks' reach and fade, the tile
  streamers and the shell's band all measure distance flat; the trees' and
  rocks' LEVEL is still chosen by 3D distance, which is what sets the size
  on screen. The trees' draw distance used to be 3D while the shell's band
  was flat, and from 650 m up the forest under the camera was bare: its
  trees were past the draw distance, and the shell, measured flat, had not
  begun. Cost nothing measurable: a flight at 260 m 2.12 ms as before, at
  650 m 1.30 ms; walking unchanged.
- **Clearing edges are slopes of crowns, not walls.** Cover falls from 1 to
  0 across one twenty-metre texel, so with the lift following the
  sharpened cover every wood stood on a near-vertical wall -- a mesa with a
  flat top -- and the cover sampled ON the wall was partial, so it shaded
  as bare ground, grey-brown. Now the lift follows the map's own bilinear
  cover (`Forest.raw`) while the colour keeps the sharpened one, so a
  wood's margin is a slope; the vertex shader passes its lift as a share of
  the full canopy (`VSOut.shell`), which says how far up a slope a pixel
  is, and above its foot it is canopy, lit with the face's own normal (the
  mesh's normals are the unlifted ground's) and taking less sky on a side.
  Colour match unchanged (87.8, 117.0, 65.3); flight cost unchanged.
- **Not done**: GPU-generated far cards (500-1500 m, for single trees on a
  skyline from the air) -- the shell was expected to carry most of it, and
  does; no altitude gate was needed.

### Micro-relief and flat sites (v17, the fine band v21)

Calm land read as one smooth sheet: the profiles' finest octave is ~110 m,
so meadows and wetland were long even slopes. `validate --flatness` is the
instrument -- the height range over pads of 10, 20 and 40 m radius at
random land points, per dominant biome: the share under 0.5 / 1 / 2 m and
the median.

- **Micro-relief is part of layer 4.** ONE field (`layer4_detail`,
  archetypes.json `detail`: 72 m, 3 octaves, gain 0.5, its own feature
  seed) shared by every biome, each scaling it by its own `detail` metres
  before the heights are blended -- which is blending heights, never noise
  parameters. Meadows 4.0, wetland 2.8, forest 2.4; meadows' flatness 1.6
  -> 1.3, wetland's 2.4 -> 1.8. The fbm's typical swing is about a quarter
  of its amplitude: 1.8 m moved the median 0.2 m and did nothing visible.
  Faded by voxel like every octave (LOD agreement still 0.39); the GPU's
  amplitude is `g_arch` row 2's `w` (it was spare) and the field's shape is
  in the world constants. Meadows' 20 m pads level to 0.5 m went 1.2% ->
  0.3% of the land, 10 m pads 9.0% -> 1.9%; the median over a 10 m pad
  1.4 -> 2.2 m. Rolling, not hills.
- **The fine band is a SECOND field (v21)**, 16 / 8 / 4 m
  (`detail.fine_period`, `fine_octaves`), its own feature seed
  (`FEATURE_DETAIL_FINE`), each biome scaling it by its own `detail_fine`
  metres: meadows 0.75, forest 0.70, wetland 0.85, highland 1.1, badlands
  1.4, taiga 0.9, ashlands 1.2, rift 1.2, **glacier 0.35** (snow should
  read smooth, and at albedo 0.9 any relief in it shouts). Note the six
  biomes that carry no `detail` at all carry this one: their profiles are
  loud at 37-110 m and were still glass underfoot.
  **Two fields, not more octaves on one**: `gain` would otherwise tie the
  two bands' amplitudes together, and v17 tuned the coarse band by eye --
  six octaves at gain 0.5 put only 11% of the amplitude below 18 m, and
  buying more by raising `gain` changes the look from a hilltop as well.
  The last octave is 4 m, eight voxels at LOD 0, which is resolved where it
  matters and fades early enough that **LOD agreement did not move** (0.73
  of a voxel, limit 1.00). `g_arch` grew to six rows for the amplitude
  (`kArchRows` 6, `kArchFloats` 24 -- keep them equal; `--probe-field` is
  the check, 31 um at the spawn, unchanged).
  Measured: meadows' median range over a 10 m pad 2.2 -> 4.0 m; the
  capsule's feet against the field 2.4 -> 4.3 cm worst (0.7 -> 3.0 mm mean
  beyond what the capsule radius explains, so it is riding bumps, not
  sinking); the navmesh still covers 98.0% of walkable dry land; validate's
  throughput and the walking frame time both within run-to-run noise (the
  extra fbm is one more call in a function that already holds six).
- **The flat sites' shoulder is what caps it.** `validate`'s flat-sites
  check measures the steepest eased ring at 0.54 rise/run against a limit
  of 0.6, where v20 was 0.51: bumpier land round a pad makes the ring that
  eases down to it steeper. meadows 0.9 / forest 0.8 / wetland 1.0 took it
  to 0.58, which passes and has no headroom, so those three are the values
  above instead. Raising them further means bounding the shoulder in
  `build_flats` first -- its placement test is an end-to-end `dh` against
  `max_grade * outer` and does not bound the local gradient at all. Sites
  went 2.8 -> 2.6 a seed, slope budget 67.3% -> 67.0% of land under 30
  degrees.
- **Flat sites are layer 5b** (`build_flats`, after the rivers): one
  candidate per 320 m cell, jittered inside its middle 56% so no two touch
  (the load checks that), kept by the blended biome's `flats` chance where
  the land is already gentle (max_grade 0.08 across the pad and out to the
  shoulder), dry, and 15 m clear of every channel. The pad is pulled to the
  mean of its own heights (`flat_apply`, field.h) and eases back over a
  shoulder of 0.6 x radius, at least 10 m. `terrain_height` applies it
  after the river carve; nothing upstream of `build_flats` sees one.
  Trees and rocks keep 3 m past the shoulder (`TREE_FLAT`, `ROCK_FLAT`),
  so every site is a glade. ~19 a seed: meadows 9.8, wetland 6.7, forest
  2.5; radius 14-36 m, mean 26.
- **On the GPU the sites ride in the RIVER buffers**, after the segments,
  as entries with a negative width -- (x, z, radius, shoulder), (-1,
  level) -- binned on the river grid's geometry. The mesher's root
  signature is at 60 of 64 DWORDs: three more root UAVs would not fit.
  `real_nearest_river` skips them, `real_apply_flats` reads the point's own
  bin. `--probe-field --probe-at x z` compares a chunk anywhere: on the
  default seed's nearest site the pad is exact and the shoulder 4 um.
- **Hashed by buffer** (id, x, z, radius, level, in the per-layer slot 2):
  a site is tens of metres across and the hash grid samples every 156 m.
- `validate`'s **flat sites** check: every pad level to 2 cm through
  `terrain_height` (measured 0.000), every shoulder under 0.6 rise over run
  (0.49), every configured biome grows some and no unconfigured one more
  than a trace. `startup.txt` names the count and the nearest to spawn.
- **Cost**: `--walk 10 --uncapped` at 720p, v16 built beside it and run
  alternately: 4.39 / 4.40 ms against 4.28 / 4.33 -- about +0.08 ms (the
  shared fbm in every field call). Validate's throughput unchanged.

### Rocks

**Placement is layer 9, in `worldgen/`, under the golden hash (v14, set
into the ground in v15)**, in
layer 8's shape: two classes on their own candidate grids -- boulders on
8 m cells (0.4-4 m across) and outcrops on 48 m cells (4-10 m) -- each a
pure function of its cell, `rock_id(kind, i, j)` its save identity. A
biome's `rocks` block in archetypes.json gives boulders and outcrops
(chance a cell holds one inside a rock patch) and cover (share of the
biome that is patch); the top-level `rocks` block the classes. Sizes are
skewed to the small end by a cube (invariant 9: no pow). Outcrops are kept
with a chance rising with slope, so they stand in hillsides. A rock stands
on the real surface -- layer 8's bisection, moved into `real_ground` and
proved bit-identical by the golden gate before any rock existed -- and is
set `sink` x its size below the LOWEST of four heights at its footprint's
edge, not below its centre (v15): set from the centre, an outcrop on a
slope or a crest showed its whole face where the ground fell away and read
as a block standing on the hill. It keeps clear of rivers, the shore, every tree trunk
(trees are pinned, so rocks give way) and, for a boulder, every outcrop. Its
`biome` is the dominant one where it stands, which picks its rock scan.

**Engine** (`engine/rocks/`): `RockField` streams 64 m tiles of boulder
cells on two workers, each tile also owning the outcrops whose CELL ORIGIN
falls in it, so none is in two tiles. `rock_mesh` builds 8 boulder and 4
outcrop models at startup, in unit size with the origin at the base, two
levels each, absolute indices. Boulders are icospheres cut by random planes
into facets, flattened. **Outcrops are bedrock**: a wide, low rounded box
(superellipse), cut by vertical joints, its top pressed flat, and banded
into beds each inset by its own amount and undercut at its base, so the
face steps in ledges that slightly overhang -- the first version, a
stretched sphere with a faint sawtooth, read as a giant boulder. Outcrops
are one subdivision finer than boulders (20k / 1.3k triangles a level) so
the ledges resolve.
`RockRenderer` culls on the CPU into two lists per frame in an upload buffer
-- the camera's (frustum, 700 m, near level inside 30 + 12 x size m) and the
sun's (boulders within 200 m, outcrops within 600 m) -- grouped by model and
level, one DrawIndexedInstanced each. **They draw on the TERRAIN's root
signature**, straight after the terrain in the main pass and in each
cascade, so the constants, shadow table and scans are already bound; t0 is
rebound per group to its instance range. `rocks.hlsl` shades exactly as the
terrain does -- the shared `scan.hlsli` (scan declarations, triplanar,
`scan_pom_shift`) was cut out of `mesh.hlsl` for it, and the cliff still
came out within one level of the pre-refactor still on every pixel -- with
the biome's rock scan, parallax and its self-shadow, and a dithered fade
over the last 12% of the draw distance.

**Collision**: one Jolt mesh shape per model from its far level
(`PhysicsWorld::add_prop_mesh`), and a scaled static body
(`add_static_prop`) for every rock within 40 m of the player, diffed by id
like the trunks -- synced from `TreeFelling::update`, which every mode with
a player already calls. **The physics copy is wound the other way**: the
draw's triangles are clockwise from outside for D3D, which Jolt takes as
back faces, and its rays ignore those -- the first probe went straight
through a boulder to the terrain under it. The `--walk` report probes it: a
ray down the nearest big boulder's axis hits a prop 6 cm under the near
level's top.

- Every `--shot` and `--walk` report has a `rocks:` line -- resident,
  drawn, triangles, colliders -- and names the largest outcrop and the
  nearest outcrop and boulder over 2 m, for a `--cam`. The badlands
  hillside draws 368 rocks, 119k triangles. **Cost: within noise**, walking
  and on a badlands flight (median 2.36 / 2.37 ms with and without).
- `validate`'s `rocks` check: each configured biome grows each class it is
  configured for, unconfigured ones no more than a trace, and every pinned
  rock's ground is air over rock. Measured per hectare (boulders/outcrops):
  highland 14.7 / 0.48, rift 9.0 / 0.50, ashlands 9.9 / 0.28, badlands 7.5 /
  0.17, meadows 1.4 / 0.01.
- **In every lighting system but one.** The sun's list also draws into
  every local light's shadow faces (the GPU culls it per face; +0.03-0.07
  ms in `--lights-report`) and the heightfield GI's top-down pass, with
  `ps_rock_top`'s albedo -- the scan's smallest mip times the retint, the
  authored rock colour exactly. **The far shadow does not**: it applies
  past 1.36 km at 8 m texels, and rocks stream to ~760 m.
- **Not done**: grass grows through a rock's footprint -- the bake would
  need the rocks bound and its cached tiles re-baked when rocks stream in
  after them, so it waits; the LOD switch pops (no cross-fade, by choice
  for now). Shots do not run physics: the collision probe is in `--walk`.
- **"Cascade acne" beside a badlands boulder was the CONTACT shadow**:
  blocky squares in bands on gently sloped sand at 16:00 (`--cam 2452 123
  3366 0.99 -0.2 --time 16`), long filed as acne in a middle cascade. A 5x
  depth bias changed nothing; `--no-shadows`, `--no-pom`, `--texture 0`,
  `--no-hfgi`, `--no-foliage` and `--no-ao` left it; `--no-contact` removed
  it. Fixed in the post chain's `contact_shadow` (see the frame section).
  Turn things off one at a time before believing which pass a pattern is.

### Glowing mushrooms: the first thing to pick up

Clusters of glowing blue mushrooms on the floors of the placed caves
(`engine/props/glow_mushrooms.{h,cpp}`, `shaders/mushrooms.hlsl`), a
collectable for testing emitters. **E** picks the one the crosshair is on,
within 2.5 m of the head (`kPickReach`), in front of whatever the camera's
ray meets; it is gone and counted (the message line). No inventory yet,
and picks are not saved.

- **Placement is the engine's, not the golden hash's**: per cave, from its
  capsules (`SeedWorld::caves`, `entrances[]`), a pure function of the
  cave's id -- `per_chamber` clusters per chamber capsule, a
  `tunnel_chance` per tunnel capsule, none in the mouth's bowl -- each on
  the floor found by marching the edited field down from the capsule's
  axis (`Renderer::ground_below`). A cave is populated when the camera
  comes within 160 m + its length (one a frame in play, a few ms; all for
  a still). archetypes.json `glow_mushrooms`; presentation only. The
  forest cave 960 m from spawn holds 16 clusters, 87 mushrooms.
- **Each cluster is one local light**: steady blue, `lumens_each` (4) per
  mushroom left, `range` 5 m, 45 cm over the caps (lower, the floor under it
  was a hot spot), ids stable per cluster so it keeps its shadow slot;
  clusters within 70 m of the camera. Shadowed like any light while among
  the 24 most important; past them it lights through rock within its 5 m
  and the probes leave it out (gaps.md).
- **Drawn** on the terrain's root signature after the rocks: three
  procedural models (stem, gills, domed cap), double sided, absolute
  indices. Lit by the sun, sky and bounce, plus a glow (`nits` 1.2 at the
  gills, about half on the cap) into the AMBIENT target with no AO. **Not
  by the local lights**: their own cluster's light over the caps burnt them
  white; at 3 nits they were white anyway against an eye keyed to the lit
  floor. So a torch held to one does not light it.
- `--shot ... --mushroom-test` stands over the cluster nearest the camera
  (`build/mushroom_before.bmp`), picks every cap within 1.5 m by aiming at
  it as E would, after a ray at the roof that must pick nothing, and shoots
  again (`mushroom_after.bmp`); `build/mushroom_test.txt` says what went and
  the lumens lost. Every shot report has a `mushrooms:` line and the
  nearest cluster. `--no-mushrooms` for A/B.
- **Cost**, the forest cave at 23:00, 1440p: frame 5.22 -> 5.40 ms with 13
  cluster lights in reach -- the terrain walking their clusters, and the
  probes they wake in sealed air (1,674 -> 2,253 live). Nothing where no
  cave is near.

### Survival: health, stamina, food and the inventory

Valheim's rules, deliberately (`engine/player/`, data in
`game/data/items.json`, never in the golden hash):

- **Health 25 and stamina 50 at base; food raises the MAXIMUM.** Up to three
  foods at once, each a different one; a food's value is (time left /
  duration)^0.3 of its full amounts, so near full for most of its time and
  falling away at the end. The same food can be eaten again only below half
  its time (it refreshes); with all three slots full, a new food replaces
  one under half, otherwise "too full". Health regenerates in 10 s ticks by
  the foods' regen; stamina continuously, 1 s after its last use, not while
  swimming.
- **Stamina**: sprint 10 a second, swimming under way 6, a jump 10 (paid on
  the press, from the ground), a swing of the axe 8, a dig 5 -- on foot
  only; flying spends nothing. With none: no sprint, jump, swing or dig
  (an action needs at least half its cost), and swimming drowns, 5 health
  a second. **A fall** over 4 m hurts, linearly to 100 at 20 m; into water,
  nothing. At 0 health the player wakes at the spawn with no food (the
  inventory is kept for now: no tombstone).
- **The inventory** is 8 x 4; the top row is the hotbar. A pick-up stacks
  onto the same item, then fills the first empty slot from the top left, so
  new kinds land in the hotbar. **1-8** use a slot: eat a food, equip or put
  away a tool. **Tools**: the axe (left click chops, and fights, only with
  it in hand: a creature in the swing's way takes it instead of the tree,
  at the swing's impact -- see Creatures and stealth)
  and the torch (in hand = the carried light; `F` still toggles the light
  directly, for debugging). A new game starts with both (`start` in
  items.json). **Left Ctrl** crouches (see Creatures and stealth).
  **Tab** opens the inventory (the old fly/walk toggle is
  **F10**, a system key: `WM_SYSKEYDOWN`): click takes a stack, click again
  places it (merge or swap), Shift + click splits into an empty slot, right
  click uses. The game runs on behind it.
- **The clock** runs in the window: a day is 30 real minutes (`clock`
  `day_length_min`), day 1 at `--time`'s hour. `Survival` owns it (game
  seconds since day 1, 00:00); the renderer's hour is set from it in steps
  of 6 game seconds -- the sky's hemisphere integral (~0.3 ms) re-runs on
  any change of the hour, and at 48x that was every frame. PgUp / PgDn move
  it, H runs it 360x. Headless modes never run it: their clock is `--time`.
- **C++ routes, Lua decides, as before**: `ui.cpp` hands Tab and 1-9 to
  `game.on_key`; `inventory.lua` and `hud.lua` decide what a click does and
  what is shown, through `engine.player()`, `engine.inventory()`,
  `engine.item(id)`, `engine.use_slot`, `move_slot`, `split_slot`,
  `engine.prompt()` (what E would do), `engine.messages()` and
  `engine.captured()` (script_api.h). Without a UI, 1-8 still use the
  hotbar.
- **Icons are data**: each item's `icon` is a list of ellipses, rectangles
  and polygons in a unit square, painted at 64 x 64 with 4 x 4 samples
  (`icons.cpp`) when RmlUi asks for the image `icon:<id>` (the UI renderer's
  `LoadTexture`, which still decodes no files).
- **The HUD** (`hud.rml`, `hud.lua`): hotbar top left, messages under it,
  the day and hour top right, the foods (icon, time left, a bar; the border
  lights when it can be eaten again) over the health bar bottom left,
  stamina bottom centre (fading out once full), and what E would pick under
  the crosshair. It touches an element only when its text changed.
- **F6 / F9** save and load the player (`build/player.txt`, text, items by
  id) and the picked plants (`build/forage.bin`) beside the edits and trees.
  A snapshot (F8) does not carry either yet.
- **`engine.exe --survival-test`** (`build/survival_test.txt`, exit code =
  failures): the rules on a test table it writes itself
  (`build/survival_test_items.json`), so retuning items.json moves nothing
  there -- eating, refreshing, replacing, wearing off, the decay curve,
  regen, the stamina delay, sprint, drowning, falls into ground and water,
  death and respawn, equipping through a move, stacks and splits, a save's
  round trip; then the shipping seed's forage at the spawn (the nearest
  plant picked, ripe again exactly regrow_days later, picks saved, another
  world's refused); then stills: `build/forage_ripe.bmp`,
  `forage_picked.bmp`, `survival_hud.bmp`, `survival_inventory.bmp`.
- **Cost**: walking at 1440p with the plants 6.22 / 6.09 ms against 6.06 /
  6.10 without (`--no-forage`) -- within noise.
- **Not done**: see gaps.md -- the window loop itself (keys, the clock
  running, sprint drain, a jump's cost, fall damage, the respawn) has only
  been run through its parts headlessly, not played.

### Creatures and stealth (ADR-007)

`docs/adr-007-creatures-perception-navigation.md` is the plan: 64 m zones
that spawn, simulate and FREEZE their creatures (saved, so leaving does not
shake a chase), per-zone spawn rules with an area cap, perception by sound
and sight, Recast/Detour navmeshes built from the field, simulation by
distance, behaviour in Lua. Data in `game/data/creatures.json` (to come)
and items.json's `stealth` block; never in the golden hash. **Phases 1
(the player's side) and 2 (zones, spawning, freezing, saving, drawing) are
built, and phases 3 (the navmesh), 4 (movement) and 5 (perception and
behaviour scripts): every phase of the ADR.**

- **Crouch**: Left Ctrl toggles it on land (in the water Ctrl still dives
  while held; Right Ctrl does nothing); 1.4 m/s (`stealth.crouch_speed`),
  no sprint -- Shift, a jump or swimming stands the player up. The
  animator blends `Crouch_Idle_Loop` to `Crouch_Fwd_Loop` by speed on the
  crouch's own phase (`LocomotionInput::crouch`). The capsule keeps its
  height.
- **Sound** (`engine/player/stealth.h`): a source level in dB at 1 m --
  silent standing, crouch walk 35, walk 50, jog 60, sprint 70 (by speed,
  walk 1.4 m/s, jog 5, sprint 8), swimming 55, treading water 40, +4
  wading, nothing in the air -- and decaying spikes (30 dB/s): jump 62,
  landing 64 + 2 a metre fallen, the axe 82, a dig 78. A creature hears
  it within its `hear_range` where L - 20 log10(d) reaches its threshold
  (`engine/mobs/perception.h`): the mimic's 20 m and 34 dB hear a jog to
  20 m, a walk to 6.3, a crouch walk to 1.1.
- **Sight**: visibility by posture and speed (crouched still 0.35, moving
  0.5; standing 0.7, walk 0.85, jog 0.95, sprint 1; swimming 0.5) x the
  LIGHT on the player through a log curve (0.05 lx the floor 0.1, 400 lx
  full; moonlight 0.15, dusk 0.59, a carried torch at night 0.79), scaled
  by a creature's `dark_gain` for night vision, x its view range, inside
  its cone (the TOTAL width), with a clear ray.
- **The light on the player** is `Renderer::illuminance_at` (chest height,
  every frame): the key light's beam if a physics ray to it is clear,
  times the cloud cover's MEAN sunlit share (not the shadow map at the
  player); pi x the sky's mean radiance x the camera's open sky x
  `storm_dim`; every placed light in clear view and the carried torch.
  Crowns do not shade it (they are not colliders).
- **The HUD** shows it bottom right while walking (brighter crouched): the
  level in dB and the share seen in full light, as two bars
  (`engine.player()` has `crouching`, `noise_db`, `visibility`, `light`,
  `lux`).
- `--survival-test` section A2 checks the gaits, spikes and decay, the
  hearing distances, the visibility table, the light curve, the cone and
  the sight ranges against the mimic's numbers, on the default tuning (so
  retuning items.json moves nothing there).
- **Zones** (`engine/mobs/creatures.h`): 64 m, the forage and rock tiles'
  grid. A zone whose centre is within `zones.active_m` (128 m) of the
  player wakes -- 11 at the spawn -- and freezes past `release_m` (160 m,
  hysteresis). Freezing takes every creature standing in it out of the
  live list as it is, with its rules' remaining waits; its clock stands
  still while frozen, and thawing puts back exactly what was there. A
  creature that walks into a sleeping zone freezes with that zone.
- **Spawning**: per awake zone and rule, an attempt every `interval_h` game
  hours (each zone's first somewhere in its first interval), at most one
  per call (a clock jump fires no burst), with `chance`, under
  `max_per_zone` and `max_active`, within `hours`. A point: up to 12
  random spots in the zone, on dry ground under a 0.6 slope (the 2.5D
  generated height), out of channels and cave mouths, not on a prop where
  physics has them, where the rule's biome weighs `min_weight`, 20 m from
  the player and `min_same_m` from its own kind, and NOT visible -- outside
  the view widened by 10 degrees, or both rays from the eye (to feet and
  head) blocked (`player_could_see`). Rolls hash the field id, the zone,
  the rule and the zone's attempt count. Measured at the spawn
  (`--mob-test`): the area cap (4) is reached in ~1.5 game hours (~2 real
  minutes), every mimic 61-94 m off, out of view.
- **Saving**: F6 / F9 write and read `build/mobs.bin` beside the edits
  (live creatures written into their zones as frozen; kinds by id, so the
  table may change under a save; another world's save is refused).
- **Drawing**: the character renderer draws up to 31 creatures (nearest
  first, `zones.max_drawn`) on the player's mesh, each its own block of the
  bone buffer -- root SRV t0's address moved per draw -- with its kind's
  TINT in the block's last record (`kTintBone` 254), in the main pass,
  every cascade, and every light face being redrawn within 2 m of one. An
  animator each (idle for now), made when first drawn.
- **The navmesh** (`engine/nav/`): one Recast/Detour tile per 64 m zone
  within 192 m of the player (`kNavRadius`), built on three workers,
  dropped past 288 m. **Built from the field, not from triangles**: per
  0.5 m column (cells CENTRED on the edit lattice) the density is sampled
  at the lattice points down a band round the 2.5D height -- padded by
  layer 6's overhang reach, down to any placed cave, over any edited brick
  -- and every run of rock is handed to `rcAddSpan`, its ends where the
  density crosses zero. Then Recast's pipeline (agent 0.4 m, 1.8 m, climb
  0.5 m: 45 degrees over a cell). The main thread GATHERS the trunks
  (radius widened 40% of the root flare), rocks, stumps, resting logs and
  a copy of the edited bricks; the worker never touches engine state.
  Obstacles are stamped as solid spans inflated by a whole cell. Areas:
  ground; WADE where the floor is within 1 m under sea level; deeper the
  floor is not walkable and a thin span at sea level - 0.5 m is SWIM (half
  a metre down so the deepest wading floor connects). Flags walk / wade /
  swim; a query includes what the creature can use, swimming costs 3x.
- **Rebuilds**: a dig (`nav_invalidate_around`), a log coming to rest or
  starting to roll (`nav_frame` watches `TreeFelling::shapes`), a load
  (`invalidate_all`). The old tile stays until its replacement lands; a
  stale result is thrown away. Tiles wait until the trees and rocks have
  streamed in (`set_ready`).
- **Spawning uses it**: where a tile is in, a spawn point must be on the
  mesh, walkable and dry, and stands at the mesh's height.
- **Measured** (`--nav-report`, seed 0, 224 m round the spawn): 49 tiles,
  20,780 polys, ~335 ms a tile on a worker (max 608), ~203 thousand field
  samples a tile, 5.6 s for all of them from a cold start; 97.8% of dry
  land under 35 degrees on the mesh; every trunk within 100 m at least
  0.46 m from the mesh past its bark; 7 of 8 paths 80 m out complete, 1.02-
  1.22 x the straight line; a swim 182 m to 4 m of water that the
  walkers' filter cannot complete; a 10 m log across a path rebuilt in one
  tile (~350 ms) and passed 1.4 m off its axis; a 3 m dig leaves no mesh
  in the hole. `build/nav_report.bmp` is the map: 256 m at 0.5 m a pixel.
- **Two things that bit**: cells sampled at their corner put the whole
  mesh a quarter cell off the world; and eroding by one 0.5 m cell clears
  a blocked cell's sides, not its corners, so the mesh touched thin trunks
  diagonally 0.1-0.2 m from the axis. A quarter cell of inflation left
  0.09 m past the bark; a whole cell, 0.46. Contours are simplified to
  0.35 m (Recast's demo allows 1.3 m, which cut corners through trunks).
- **Movement** (`engine/mobs/locomotion.h`): a creature with a gait (1
  walk, 2 run; saved on the Mob with its goal, so a frozen chase resumes)
  follows a navmesh path to its goal -- re-planned when the goal moves 0.5
  m or every 1.5 s (3 s far off), corners passed within 0.5 m, the goal
  within 0.6 m, easing in over the last 1.5 m -- at its kind's speed,
  filtered by how it moves (walkers walk and wade, the mimic swims too).
- **Creatures and each other**: pushed apart within 1.2 m; one ahead within
  3 m and 1 m of the line of travel is steered round -- away from the side
  it is on, or, met head on, to the same hand, which both take, so they
  pass rather than mirror. Bodies (near) collide with each other and the
  player in Jolt; off physics a final pass keeps any two centres 0.75 m
  apart, back onto the mesh. Creatures do NOT steer round the player (a
  chasing one has to come up to them); a body slides round theirs.
- **Stuck** (under 30% of the asked speed for 0.8 s -- the player, a log
  the mesh has not stamped yet): a 2 m sidestep away from the nearest
  creature (else alternately left and right), then a new plan from there;
  after 4 without passing a corner, the goal is given up.
- **Near the player** (within 34 m, released past 38) a creature has a
  PHYSICS BODY: `PhysicsWorld::add_walker`, the player's capsule and its
  rules, through the same `step_body` -- ground, slopes, steps, buoyancy,
  swimming, wading's drag. Jolt's `CharacterVsCharacterCollisionSimple`
  makes the player and every creature collide. Further out it slides along
  the mesh (`NavWorld::move_along`, Detour's moveAlongSurface), feet at the
  mesh's height, or 1.35 m under the sea in a swim area at half speed, as a
  body floats. The hand-over puts the body where the creature stands.
- **Animated** from what it did -- measured speed, supported, swimming,
  vertical speed -- through the player's animator, so walk, jog, sprint,
  swim and the air come from the same clips.
- **Perception** (`engine/mobs/minds.h`), on a think tick -- 5 a second
  for a creature with a body, 1 further out, staggered: it HEARS the
  player (their level through `perception::hears`) and SEES them (its
  range x their visibility x the light on them, the cone HORIZONTAL and
  `view_vertical_deg` up or down -- the mimic's 120, no limit -- and a
  clear physics ray to their chest or head from its eye or a head's width
  either side). As a 3D cone, a player on a slope 2 m off, or lower down
  30 m away, fell out of 45 degrees; from one point, a trunk beside the
  creature blinded it to a whole sector. No player while flying.
- **What hides a player on a slope** (`--mimic-test` 7: 12 slopes of grade
  0.25-0.75 near the spawn, the player walking 8, 20 and 35 m up or down
  the slope in the cone, 67 placements): the steepest look was 33
  degrees, so a vertical limit of 60 never hid anyone; what did was a rise
  between them (7), trees and rocks (7), and a trunk right beside the
  mimic's eye (3), which the wider eye fixed: 49 seen, now 54, the other
  13 behind real crests and trunks.
- **Behaviour is Lua**: a kind's `script` returns hooks -- `on_wake`,
  `on_see(c, ev)` every tick it sees them, `on_lose` the tick it stops,
  `on_hear` every tick it hears them unseen, then `on_think(c, dt)` --
  called with a handle whose methods are listed in `minds.h` (state and
  mark are saved; `move_to`, `stop`, `face`, `random_point`, ...). One Lua
  state for every creature, apart from the UI's (a UI that fails to start
  does not stop them). Errors go to `build/creature_log.txt` and leave
  the creature standing. A kind with no script wanders
  (`Locomotion::wander`).
- **The mimic** (`game/scripts/creatures/mimic.lua`): IDLE wanders near
  home; HEARD, it stops, turns to the sound and listens (ALERT, 4 s after
  the last sound) -- turned, it often then sees; SEEN within its sight it
  walks over (APPROACH), within 40 m runs (CHASE), and closes to just
  inside its own reach (2.1 m), where it stops, faces them and swings
  (ATTACK) until they are 1.3 m further off than that -- a kind with no
  attack of its own stares instead (STARE), which is what this did before
  blows existed. Struck, it turns on whoever did it (`on_hurt`), wherever
  it was looking. Out of view it keeps on to
  where it saw them for 3 s (a pursuer weaving round trees has them out of
  its 45 degrees every few steps; without the grace it flipped to
  searching and back), then SEARCHes there, LOOKs about for 6 s and
  RETURNs home.
- **A standing creature keeps its facing**: only a creature asked to move
  turns to its travel. A body settling on a slope slid fast enough to swing
  an idle mimic 8 degrees, and a player 15 degrees off its facing fell out
  of the cone.
- **Blows, both ways** (`engine/mobs/combat.h`). A kind's `attack` block in
  creatures.json is the arithmetic -- `damage` and `stamina` off the player,
  `reach_m` chest to chest, `windup_s`, `recover_s`, `arc_deg` -- and the
  script decides WHEN (`c:attack()`, with `c:attacking()` and
  `c:attack_reach()`; a kind with no block cannot strike and only stares, as
  every kind did before). **A blow is in two parts because a blow that lands
  the instant it is decided cannot be avoided**: `begin` starts the windup,
  and when it runs out the blow lands only if the player is STILL within
  reach and within the arc of its facing -- so a step back or round during
  the telegraph is a miss, and `--combat-test` A asserts both. Then
  `recover_s` before the next. The windup is session state, not saved: a
  creature frozen mid-swing thaws and swings again. Health IS on the Mob, so
  one frozen half dead thaws half dead.
- **The player's swing is in two parts as well, and lands at the ANIMATION'S
  IMPACT** (`engine/player/swing.h`). The press starts the clip; `impact_s`
  later the blow lands, aimed from where the player stands and looks THEN,
  so a turn away mid-swing is a miss -- their side of the rule a creature's
  windup gives them -- and `recover_s` after that the next swing may start.
  Before this the tree lost a chop and the mimic its health on the button,
  a third of a second before the axe reached either. The timing is per TOOL, in items.json's `tool`
  block: the axe `impact_s` 0.47 and `recover_s` 0.33, measured from UAL's
  `Sword_Attack` -- its hand accelerates from 0.33 s and stops at 0.50,
  which is the bite. A heavier weapon lands later; a knife at once
  (`impact_s` 0, which lands on its first frame).
- **A click inside a swing is KEPT, not dropped**, if the swing is within
  `swing_buffer_s` (items.json's `player` block, 0.3 s) of ending: it is
  swung the moment the swing frees up, through the loop's own gates (the
  axe in hand, the stamina) run afresh then, since half a second has
  passed since the click. One press only -- a second inside the window
  replaces it -- so holding the button cannot stack swings up. Chopping a
  tree is click after click, and without this a click that lands during
  the swing reads as a dead button.
- **The blow itself is one ray from the eye**, as a chop is: a creature
  whose body capsule (the navmesh's agent sizes) it enters within the tool's
  `reach_m` takes the tool's `damage` (items.json's `tool` block: the axe 15
  against the mimic's 40, so three swings), and the tree behind it does not
  -- one swing, one target. A creature at zero health leaves the live list
  (`Creatures::kill`); nothing drops. The blow calls `on_hurt(c, ev)` at
  once, not on its tick, so a creature cannot be killed from behind without
  ever turning round: the mimic turns on whoever struck it and chases. The
  axe is HEARD where it bites (the 82 dB spike, stealth.h), not on the
  press. The swing is session state, like a creature's windup: a death
  drops one in flight, and nothing saves it.
- **Being hit**: health and stamina go (`Survival::drain` takes what is
  there and never refuses, where `spend` would), the HUD's `#hurt` reddens
  and fades over 0.8 s (`engine.player().hurt`), and death is the survival
  layer's own -- the player wakes at the spawn, foods gone. The mimic's
  ATTACK state is state 8; `--mimic-test` knows it as "reached them"
  alongside STARE.
- `--combat-test` (`build/combat_test.txt`, exit code = failures): section A
  is the arithmetic on a table of its own, so retuning creatures.json moves
  nothing there -- the windup, a step out of reach, a step round behind, the
  recovery, and the ray against a body, in front and behind; then the
  player's own swing clock on a tool of its own (nothing at 0.38 s of a
  0.4 s impact, one impact and only one at 0.4, no second swing inside the
  recovery, a tool with `impact_s` 0 landing on its first frame, and the
  click buffer: thrown away early in a swing, kept within
  `swing_buffer_s` of its end, replaced rather than queued, handed back
  once as the swing ends, and cleared with it).
  Section B is
  the shipping world at the spawn: a mimic 10 m off sees the player, walks
  up and lands a blow in 2.3 s (health 25 -> 17, stamina 50 -> 44), does not
  swing again inside its recovery and does once it is over; the player's
  first blow lands 0.48 s after the press (the axe's impact_s 0.47) through
  the window's own path -- the camera aimed as a player aims it and
  `land_swing` at the impact -- and three swings of the axe kill it; a
  wounded one comes back wounded through a save.
  `build/combat_hit.bmp` is the pair, from off the player's shoulder.
- `--mimic-test` (`build/mimic_test.txt`): a player whose speed, crouch and
  light the test sets, at places along a line of clear sight: a jog 15 m
  behind (ALERT 0.7 s, standing, CHASE at 1.2 s as it turns and sees); a
  walk 15 m behind and a crouch-walk 3 m behind unheard, a crouch-walk 0.8
  m heard; standing still 45 m ahead by day unseen, sprinting seen (walks,
  runs from 39 m, stares, facing them); 30 degrees off unseen, 15 seen; a
  chase lost, searched, looked about 0.6 m from where it lost them, home;
  a sprinter 25 m off unseen by moonlight, seen by torchlight; a chase
  through a save. `build/mimic_stare.bmp` is the stare.
- `--move-test` (`build/move_test.txt`): a walk near the player (1.40 m/s
  measured against 1.4; feet within 0.10 m of the mesh; a body all the
  way), a run back (5.19 against 5.0), a walk 70 m out (on the mesh, no
  body), a run in from 50 m (mesh, then a body; the biggest step in a
  frame 0.083 m, a run's own), a walk frozen half way, saved, read back and
  finished, two passing head on near (never nearer than 0.90 m) and far
  (0.92), four walking to one point (they stand round it, never nearer
  than 0.82), one standing on the path walked round (0.97), the player on
  the path slid round (0.74), a 2 m log the mesh has not seen stepped
  round (1 sidestep), and a swim from the nearest beach to 3.7 m of water
  (feet ~1.3-1.5 m under, swimming); stills `build/move_walk.bmp`, `move_swim.bmp`. Every
  body is given back. The player's `--swim` and `--walk` are unchanged by
  the refactor (eye never under; worst step 2 cm).
- `--mob-test` (`build/mob_test.txt`, exit code = failures): 24 game hours
  at the spawn (spawns, distances, biome, unseen, both caps), the player
  2 km away (all freeze, the zone's clock stands for 10 game hours), a save
  read back and another world's refused, the return (every creature back
  bit for bit, the clock where it stood), and `build/mob_test.bmp`, the
  nearest from 5 m.

### Forage (layer 10): things to pick, and the zones they live in

**Placement is layer 10, in `worldgen/`, under the golden hash (v20)**, in
layer 8's shape: per species (archetypes.json `forage.species`) a grid of
candidate cells (strawberries 40 m), each holding at most one PATCH of 1-3
plants within 2.5 m of its middle, 0.8 m apart. A biome's `forage` block is
each species' chance a cell holds a patch (meadows: strawberries 0.5),
blended across borders with the weights sharpened as trees sharpen them; a
patch on a slope is kept with that chance times (1 - slope / max_slope).
Plants stand on the real ground (`real_ground`), clear of trunks (1.4 m:
the root flare), rocks, rivers, flat sites and cave mouths, with no roof.
`forage_id(species, plant, i, j)` is a plant's save identity. Rolls are
keyed by plant and try, never by how many came before. `validate`'s
`forage` check: configured biomes grow patches (meadows 103 /km2, ~2 plants
each), unconfigured ones no more than a trace, every pinned plant seated;
the hash pins them by instances at the trees' 24 sites, 5 x 5 cells each.

**Zones, not a world list** (`engine/props/forage.h`). Valheim generates a
64 m zone's contents when a player first comes near and keeps what changed
as per-object records. Here generation is free -- a plant is a pure function
of its cell -- so 64 m tiles round the player are built on a worker as they
come within 150 m and dropped when they leave (RockField's shape), and the
only record is what the player CHANGED: `picked_`, id -> the game time it
was picked. A BVH would buy nothing: every query is "near the player",
which the tile grid answers by construction. **Regrowth** is a comparison,
not a timer: ripe when the clock is `regrow_days` game days past the pick
(strawberries 3 -- 90 real minutes).

- **What a plant gives** is the engine's half of its archetypes.json entry:
  `item` (an items.json id), `yield` [min, max] per plant from its id's
  hash, `regrow_days`, and `look` (size across, leaf / berry / flower sRGB).
- **Drawn** like the mushrooms, on the terrain's root after them: procedural
  meshes built at startup (`build_models`), three variants a species, each
  in three parts -- a rosette of trifoliate leaves on arching stalks (every
  plant), berries hanging at the rim with a calyx (ripe), white flowers on
  stalks (growing back). Leaves are lit as leaves (wrap, through), berries
  glossy and seeded by the BRDF; local lights, the wet world and the flash
  reach them; no shadow cast. A dithered fade over the last 15% of 90 m.
- **The grass clears from under them**: `FoliageRenderer::set_bare_fn`
  lets the bake mark columns bare where a plant stands (the bake's existing
  edit mask), and a newly streamed tile re-bakes the grass under its
  plants. Without it 25-60 cm of meadow hid a 20 cm plant.
- **E** picks the ripe plant under the crosshair within 2.5 m of the head
  (`Renderer::aim_forage`: the plants first, and a terrain ray only when one
  is in reach, so the per-frame prompt is free when none is), into the
  inventory; a full inventory refuses before the plant is picked.
  Mushrooms (E in caves) go into the inventory too: archetypes.json
  `glow_mushrooms.item`.
- `--no-forage` for A/B.

### The player character

Walk mode (F10; the window opens walking at the spawn) is third person: Quaternius's **Universal Base Character**
(CC0, `assets/models/Universal Base Characters[Standard]`), male by default,
`--character female`. Chosen over the low-poly girl beside it, which has 36
clips but no licence file, for its UE5-mannequin skeleton (65 joints): the
skeleton most animation sets target. The kit ships no animation; its clips
are Quaternius's **Universal Animation Library** (CC0, `assets/models/
Universal Animation Library[Standard]`, 43 clips on the same joint names).

- **The pack** (`tools/models/pack_character.py` -> `CHR1`): body and a
  hairstyle merged on one skeleton, 1K albedo and (normal, roughness)
  arrays. The kit's hairstyles were bound on the FEMALE skeleton -- 35 cm
  out at the male's head -- so hair vertices are rebound per joint into the
  body's rest pose. Hair textures are a grey mask meant for the kit's own
  shader: tinted in linear light to an authored colour. Normals from the
  kit's OpenGL folder, stored with +y DOWN the texture.
- **The clips** (`ual.anim`, the same packer): the IN-PLACE glb, since
  physics moves the body, resampled to 30 fps; each clip's speed measured
  from the root-motion glb (walk 0.97 m/s, jog 5.36, sprint 8.25, swim
  2.18), and for gaits the frame where the left foot leads (`phase0`), so
  walk, jog and sprint blend IN STEP on one phase.
- **Retargeting at load** (`AnimLibrary::load`): the library's rest pose is
  not the characters' -- up to 17 degrees at the neck (20 on the female),
  limbs up to 8 cm different -- so copying local rotations would bend every
  joint by its rest difference. Each joint's GLOBAL rotation relative to the
  library's rest is put on the character's rest (C = G Lrest^-1 Crest), then
  turned back into locals; bone lengths stay the character's, and the
  pelvis's translation is scaled by hip height (1.035 male, 1.016 female).
- **The animator** (`character_model.{h,cpp}`) picks and blends from what
  physics did -- speed measured from the feet physics moved, on the ground
  or not, in the water or not. Locomotion is idle, then the pair of walk /
  jog / sprint either side of the speed, the phase advanced by DISTANCE over
  the blended stride so feet do not skate; `Jump_Loop` in the air;
  `Jump_Land` after a fall of more than 0.35 s, faded out by speed;
  `Swim_Idle_Loop` to `Swim_Fwd_Loop` by speed. Masked layers over that:
  `Idle_Torch_Loop` on the right arm while a torch is carried, and the
  TOOL'S OWN clip on the upper body (spine up) for a swing, the legs
  carrying on: `trigger_action(clip, len)` takes it from items.json
  (`tool.anim`, `tool.anim_s`), so a weapon can swing differently, and
  `len` plays only part of a clip -- UAL's `Sword_Attack` is 1.533 s and
  holds a SECOND strike after the first, which an axe should not show, so
  the axe plays 0.78 s of it. A library that lacks the named clip falls
  back to `Sword_Attack`; with no library at all the procedural gait plays
  nothing and the blow still lands on time, which is the swing's business,
  not the animator's. The lowest heel or ball is planted on the capsule's
  ground (`foot_drop`), no IK -- capped at 2 cm down with clips: uncapped it
  pulled a run's flight phase 18 cm down and glued a foot to the ground. In
  the water the head joint rides 1.3 m over the capsule's feet (buoyancy
  holds them ~1.3 m under): the swim clips lay the body level at pelvis
  height, which stood on the feet put the swimmer a metre under.
  On foot: a jog (5 m/s) by default, Shift sprints (8), Caps Lock walks
  (1.4) -- near each clip's own speed. A diagonal is no faster.
- **Without the clips** the animator falls back to a procedural gait: every
  delta a rotation about a MODEL-SPACE axis at the rest pose (`apply`), legs
  on gait-lab curves (Winter's walking, Novacheck's running).
- **The draw** (`character_renderer.{h,cpp}`, `character.hlsl`): GPU
  skinning, four joints a vertex, bone matrices model -> world in a root
  SRV (t0), textures at heap [110..111] (t49..t50, a tenth range of the
  terrain's table). On the TERRAIN's root signature after the rocks, in
  every cascade, and in every shadowed local light's faces. Not the
  heightfield GI's top-down map. Double sided, cull none, a back face's
  normal flipped.
- **The body in the lights' shadows** (`record_light_shadow`,
  `ps_character_light_shadow`). The faces are CACHED (a light redraws its
  six only when it moves or every 240 frames), so the body keeps its own
  bookkeeping: a bounding sphere from the skinned joints (+0.3 m), a
  `pose_serial()` that moves when any joint has moved 3 mm, and per slot
  the faces last drawn with the body in them. When the serial changes,
  the faces the sphere is in now AND the ones it was in (to take the old
  shadow out) are redrawn whole at the light's cached position, at most
  `kMaxBodyFacesPerFrame` (12) a frame on top of the lights' own twelve --
  the trees' face constants are 24 now. **The carried torch clears the
  body within `kTorchBodyClear` (0.4 m) of the flame**: it sits 0.28 m
  from the fist, and at 0.3 m the fist's far side still cast a hard black
  blot down the shin. The body darkens what is behind it from a campfire
  and throws a long shadow off a torch, its own far leg included.
  `BIOMES_NO_BODY_LIGHT_SHADOW=1` is the A/B; `--pose-sheet --time 23
  --torch` or `--fire-ground 0.4 1.7` the stills. Cost walking at 1440p
  with a torch and two campfires: 7.92 / 7.98 ms against 7.90 / 7.92,
  99th percentile +0.3 ms. Cost: within noise (`--walk 15 --uncapped` at
  1440p 5.94 ms against 6.08 in first person).
- **The camera** (`main.cpp`, `place_walk_camera`): orbits a pivot 1.55 m
  over the feet at the mouse's yaw and pitch; the wheel sets the distance
  (1.2-18 m, 15% a notch, 4.5 by default) and zooms through to the old
  first-person eye with the body hidden. A physics ray (terrain, trunks,
  rocks) and the heightfield pull it in at once and ease it back out.
  Movement is relative to the camera; the character turns to where it is
  going at 12 rad/s, and to the camera's heading when it swings the axe.
  The axe swings from the head, and the carried torch sits over the right
  fist (`Renderer::set_torch_anchor`).
- `--swim` also writes `build/swim_char.bmp`, the same moment in third
  person (its main still stays first person: it judges the waterline).
- **Not done**: see gaps.md -- foot IK, motion vectors for TAA, starts,
  stops and turns.

### Ground cover

Grass blades, clump cards, bushes and flowers, per biome from the `ground`
block in `archetypes.json` -- presentation only, never in the golden hash or
a save, nothing collides. 16 m tiles of 1 m cells are baked on the GPU as
they come into range (`cs_bake`: the real surface, bisected inside layer 6's
overhang as trees are; slope; river and shore; the blended ground cover; a
canopy factor from layer 8's patch noise, so a forest floor has less grass
and more bushes) and cached in a 768-tile pool. Every frame `cs_emit` turns
cached cells in the frustum into: blades (5 segments to 22 m, 2 beyond),
full density inside 10 m and thinning as (10/d)^2 to 60 m with each blade
widening by the square root so cover holds, the last blade of a cell
shrinking rather than popping; crossed clump cards 48-150 m, dither-faded
against the blades; bushes (below); flowers on a few blade tips. A 6-8 m
tuft noise varies height and density inside a patch. Same sun, shadows and
fog as everything else. `G` toggles it. Cost: +0.3 ms a frame walking
through a meadow (`--walk 15 --uncapped`, with and without `--no-foliage`);
about 24k near blades, 13k far, 13k cards in a woodland-edge still.

The bake kernel is the third time the inlining lesson has bitten: written
longhand -- five heights and three density probes -- it exceeded FXC's 4096
temp registers (X4505). One call site per field function, each in a
`[loop]`, the probes as one loop with a little state machine.

**Tufts.** A placed blade draws THREE (`tuft`, `kTuftMax` in `foliage.hlsl`;
the index ranges hold three strips each): the placed one and two more 2-13
cm off its root (skewed small by u^2 -- every clump the same size across is
as regular as a grid of them), each with its own angle, 65-105% of its
height, leaning out -- one placement reads as a clump. Blades are also 1.4x
as wide. The spawn meadow went from stems over pale ground to a sward.
`--grass-tuft 1|2|3`, `--grass-width <k>`, `--grass-density <blades/m^2>`
(96) and `--grass-distance <m>` (150) for A/B, **and all four are player
options** (Graphics, saved: "Grass density", "Grass thickness", "Blades
per clump", "Grass draw distance").

**Density is the cost; distance is not.** `--walk 15 --uncapped` at
2560 x 1440 at the spawn: density 32 / 96 / 192 is 5.87 / 6.04 / 6.28 ms
against 5.68 with `--no-foliage`, so the slider spans more than the whole
of the default's 0.36 ms -- the near list holds one entry per blade
INSTANCE and each draws `tuft` of them. The distance over its whole range,
50 / 150 / 300 m, is 6.04 / 6.05 / 6.08: blades thin as 1/d^2 and the far
ones are 2-segment and sub-pixel, so reach buys resident tiles (88 / 424 /
1388 of the 1536 pool) and almost no frame time. It is a look knob here;
on a smaller card it is not.
Cost walking at 2560 x 1440: all ground cover ~1.0 ms (5.19 with
`--no-foliage`, 6.23 before); tufts of 3 +0.26, with the width +0.32,
tufts of 2 with the width +0.18. Tinting the terrain under grass toward
the sward (a 256 m grass map from the cells, read by the terrain shader)
was built and judged not to look right; left for a later quality pass.

**The draw distance is ONE number and the near distances are CLAMPED, not
scaled** (`FoliageSettings::set_range`): card_far is the slider, blade_range
is min(60, m - 10) and card_near min(48, blade_range - 12). Where blades
hand over to cards is tuned, not a ratio of the reach -- `cs_emit` thins
them as (near_radius / d)^2 and widens each by the square root, and the
widen caps at 4, so past ~40 m they stop covering the ground, which is what
card_near 48 is for. Scaled proportionally (the first version, built and
shot) 300 m stretched blades to 120 m where they are far too sparse, and
50 m put the crossed cards at 16 m, where they read as dark Xs on the
ground. Clamped, the near field is the shipped one from 70 m up -- 150 and
300 m draw the same 7726 far blades and differ only in cards (4556 against
10256) -- and under that the bands compress, the cross-fade keeping 12 m
and the cards' own band 10. Setting `blade_range` alone is what the one
number exists to prevent: at 30 it leaves the 30-48 m ring bare. Bushes
reach with the rest (130 m of the shipped 150), and the slot pool is sized
for the slider's top rather than the default (kPool 768 -> 1536, ~14 MB).

**Things standing in the ground cover push it aside, and grass stays laid
over** -- the trample map, `engine/foliage/trample.{h,cpp}`,
`shaders/trample.hlsl`. `--trample x0 z0 x1 z1 r s` presses a path for a
still, `--no-trample` is the A/B.

- **The state lives in the WORLD, not in the blade.** A blade is a pure
  function of its cell and nothing about one is stored, so the memory of
  being trodden on is a texture: 2048^2 of RGBA16F at 10 cm texels, 204.8 m
  across the camera, 33.5 MB. Each texel holds a push vector in the wind's
  own measure -- a tip displacement as a fraction of a blade's height, ~1
  being lying over -- so `vs_blade` bends, shortens and leans by it without
  knowing which pushed it.
- **TOROIDAL, which costs nothing on the reading side.** World texel w lives
  at w mod N; N is a power of two, so the wrap is a mask in the kernels and
  the read is `frac(world.xz / kMapMetres)` through a WRAP static sampler --
  no origin is passed to a shader at all. Following the camera clears only
  the strips that scrolled in. The near field's occupancy volume does the
  same thing for the same reason.
- **The push IS the flatten, read at age zero.** `cs_splat` writes the same
  vector into both pairs of channels and `cs_decay` fades them at different
  speeds: the fast pair (xy, tau 0.12 s) is what a bush reads, so it is
  shoved aside and back upright a moment later; the slow pair (zw, tau 18 s)
  is what grass and the clump cards read, so a trail stays. Something
  standing still re-stamps its texel every frame and both stay full, which
  is the "pushed while on them" case with no special handling. **Two decay
  rates, not two systems.**
- **The recovery time is bounded by the map, not by taste.** Anything that
  scrolls past the edge is forgotten, so a trail that outlived the window
  would pop. 102 m from the camera to the edge is ~20 s at a jog, which is
  what tau 18 s sits under. Longer trails need saved world-space state,
  which is a different and much larger feature.
- **It rides in `Blade`'s spare word.** `cs_emit` samples the map at each
  blade's root and packs the push into `pad` as two snorm16 -- `b.a`'s 32
  bits were all spoken for and `pad` had none, which is exactly the room
  needed -- so no list grew, the vertex shaders only unpack, and clump cards
  and bushes get it free off the same struct. The push is a function of the
  root, not of where you are up the blade.
- **A stamp is a swept CAPSULE, not a disc.** The player's is last frame's
  feet to this frame's. Stamped as a disc, a log at 10 m/s leaves 16 cm gaps
  at 60 Hz -- a dotted line at 10 cm texels. The rain draws a streak as the
  path over 45 ms for the same reason. A capsule longer than the splat's
  fixed 6.4 m footprint (a felled log is ten metres) is cut into pieces on
  the CPU, so the kernel keeps one dispatch per stamp.
- **Two stamps can race for a texel** -- two feet 20 cm apart -- and the
  answer differs by less than a pixel either way, so it is left alone
  rather than paid for with atomics on a packed format.
- **Cost: 0.025 ms** at 2560 x 1440. The map's own passes are 0.012 ms a
  frame (max 0.082, the frame in eight the fade runs: a sweep of 4.2 M
  texels is 34 MB read and written, which is more than the whole of the
  ground cover if it ran every frame), and the read in `cs_emit` takes the
  foliage pass 0.280 -> 0.293. Walking at 1440p 6.10 ms with and without; a
  900 m flight at 60 m/s holds 0 holes, which is the toroidal scroll under
  the worst movement there is.
- **A still can show no trail**, because a trail is history and nothing
  walks during one: `--trample` HOLDS a path, re-pressed every frame, the
  way `--dig-row` digs before it shoots.
- **Not done**: creatures and resting logs do not stamp yet (the player
  does); the forage plants read nothing (they are a different renderer);
  the fade is a plain exponential, so grass starts recovering at once
  rather than holding and then springing back; nothing is saved, by choice.

**Half of every meadow was drawn BLACK, and it read as "the grass is laid
out in a grid".** `vs_blade` tilts each blade's normal up by 0.9 so a sward
is lit as a surface rather than as a thicket of vertical strips each in its
own shade; `ps_blade` then turned the normal to face the eye with
`if (!front) n = -n`, which negated that tilt as well and pointed it DOWN on
every blade the camera sees from behind. No sun, no sky: at 11:00 in the
spawn meadow about half the blades came out near black, in the 3-blade fans
they are placed in, which is exactly what rows of dark clumps look like.
`n.y = abs(n.y)` after the flip -- the length is kept, so nothing is
renormalised. Walking cost unchanged (6.02 / 6.05 ms against 6.05 / 6.05 at
2560 x 1440). It was found by looking at a still, not by a metric; it had
been there since the blades were written.

**The placement is NOT a grid, and the obvious fix makes one.**
`tools/profile/scatter.py` is the instrument -- `tiling.py` answers a
different question (does the same CONTENT repeat, which a scatter never
does, so it reads ~0 either way and cannot see this at all). From a
straight-down still it reports the power on the lattice's axes against a
ring of the same radius: 1 is nothing lined up. The shipped scatter --
`ceil(want)` blades per 1 m cell, each uniform within its OWN cell -- reads
3.91 on the axes against 3.70 on the diagonals, which is nothing. **A
jitter of exactly one cell cancels at the lattice frequency** (the cell's
box transform is sinc(1) = 0), so an equal count per cell is a blue-noise
scatter, not a grid. Two "fixes" were built and measured and are gone:
spilling the blades over 1.5 cells so neighbours overlap breaks that
cancellation and reads **21.75**; a per-cell random density multiplier is a
1 m square wave in its own right and reads **17.26**. What is left of the
quilt under a sward is AO and the contact shadow pooling under each clump
(`--no-ao --no-contact` removes it), which is the light being right.

**Bushes have stems now** (`vs_bush`, 24 quads: six stems, eighteen leaf
cards). They were fourteen cards round the inside of a ball -- a few
textures with nothing solid in them -- and evenly 0.4-1.4 m in radius, so
waist-high balls every few metres. Now up to six tapered stem ribbons from
the base, facing the eye, with the leaf cards on their upper halves, in four
kinds from the bush's hash (round, upright vase, low spreading, sparse
twiggy), a tint per bush (most green, some yellower, bluer, a few red), one
in seven needle-leaved, and radii 0.25-1.4 m skewed small (u^2). Leaves cut
as the trees' are: sharp, then solid clusters (`leaf_cluster`), no dither.
The index layout is 264 entries (`foliage_renderer.cpp` checks it).

Dug ground grows nothing: see Digging, below.

### Digging (right mouse)

Right click digs where the screen centre (the HUD's crosshair) meets the
ground, held it digs every 0.2 s; `,` `.` step the radius 0.5 m (ONE edit
voxel) to 8 m (Shift + `,` `.` is exposure now). `Renderer::dig_aimed`
marches `edited_density` -- the edit store at each lattice corner, else
the field at the 0.5 m voxel, trilinear between: what LOD 0 meshes -- so a
second click digs deeper into the first hole. It centres the sphere on the
nearest SOLID lattice point half a voxel past the hit, so even the smallest
dig removes rock. `--shot ... --dig-row` digs
one of each size across the view before the still and writes
`<shot>.dig.txt` (hit, centre, voxels written, ms, how far the ray now
goes). Two edit-store fixes came with it: `edit_sphere` measured each voxel
from its middle while the mesher reads it at the lattice POINT (every edit
sat a quarter voxel towards +xyz), and it wrote exactly 0 on the rim and
overwrote earlier edits.

**A dig writes a signed distance, not a fill value.** Every lattice point
inside the sphere and within `kEditBand` (1 m) outside it becomes the union
of what was there and the sphere: min(old, d - r) for a dig, the reverse
for a fill; a point it does not change is not written. It used to write
the inside only, as -strength x (1 - t^2) held under -5 cm, so the rock
point beside it kept the FIELD's value -- metres, under ground -- and every
surface vertex landed beside the air point: dug walls in half-metre steps,
blocky rims, and a toothed waterline wherever the sea crossed a wall. Now
the surface lies where the sphere's does, between lattice points. Signs are
unchanged everywhere, so the flood, sky access, physics and the near field
label exactly as before. `old` is the edit store's value, else the field
(`base`, a batch callback, spread over every core past 256 points); a
point more than kEditBand inside the sphere asks nothing, since no surface
can cross an edge it ends. The column map (grass, settling trees) marks
only points written as air: the band's rock stays rock. Cost
(`--dig-row`): 0.3 ms at 0.5 m, under 1 ms to 3 m, 1.2 ms at 4 m, 3.7 ms
at 8 m (asking the field everywhere, single-threaded: 30 ms). Saves load
as before; old edits keep their steps.

**A dig keeps a roof thinner than the lattice.** A dig that stops 1-40 cm
under the surface leaves rock no lattice point holds -- carved on one side,
air on the other -- and drew as a hole in the ground it never reached
(a dug pit's rim on the beach). Before a rock point is carved, each neighbour that is
air and outside the sphere is asked where, along the edge between them, the
sphere's boundary and the ground's surface cross; if the sphere's comes
first, rock remains there and the point stays rock, so the roof is drawn a
voxel thick. A dig that does break through carves as before; so do the
player's aimed digs, whose sphere holds the point they hit.

- **Grass goes where an edit reaches the ground.** The edit store keeps a
  derived map (`BrickStore::column_top`, rebuilt on load, not saved): per
  0.5 m column, the highest edited lattice point that reaches it. Each
  foliage bake gets that for its tile (t3, 32 x 32 floats) and marks each
  1 m cell's four quarters bare where the top is within 1 m of the
  generated ground (`Cell.bare`, now 28 bytes). `cs_emit` drops the blades
  in a bare quarter and any card or bush in a cell with one. A dig
  re-bakes the resident tiles under it, in their own slots, before the
  frame's new tiles (`FoliageRenderer::invalidate_box`; a load re-bakes
  all). A one-voxel pit bares a metre square. A tunnel more than a metre
  under the grass leaves it.
- **Trees drop straight down to the new ground** (`Renderer::settle_trees`).
  A standing tree whose trunk column an edit reaches within 1.5 m of its
  base is checked once per edit generation (`BrickStore::generation`):
  `ground_below` marches the edited field down the trunk's axis from half
  a metre over the generated base. If that ground is more than 10 cm lower,
  the tree falls from rest under gravity and lands there, upright: not
  physics. `TreeField::set_base` overrides by id, so `standing()` carries
  the new base to drawing, shadows, the canopy map and felling. It also
  persists across tile re-streams. Ground that comes back (a load) lifts it
  at once. Felling rebuilds a trunk capsule whose tree moved
  (`collider_y_`), so a sunk tree blocks and chops like any other.
- **Rocks drop too** (`Renderer::settle_rocks`, `RockField::set_drop`),
  by the LEAST ground lost under nine points of their footprint: the
  middle and a ring at a third of their size. Each point compares the
  generated field with the edited one (`ground_below(..., with_edits)`), so
  an outcrop undercut on one side stays put, held up by the rest. Both `y`
  and `ground` move. The rock renderer rebuilds a moved rock's body
  (`collider_y_`); the draw reads the list every frame anyway.
- `engine.exe --dig-settle` is the test, `build/dig_settle.txt`. Tree: a
  3 m sphere under the nearest fellable tree to spawn. It checks the drop
  (3.87 m in 0.88 s; free fall predicts 0.89), the landing height, a ray
  at the new base hitting a prop, and felling from the bottom of the hole
  (`build/dig_tree.bmp`). Boulder: the nearest over 1.5 m, a hole under
  its whole footprint (3.48 m boulder: 3.35 m down in 0.83 s, free fall
  0.83). A ray down its axis must hit a prop at its NEW top: it hits 6 cm
  under, as `--walk`'s undisturbed probe does (`build/dig_rock.bmp`).
  It also counts frames from the dig until every chunk it threw away is
  redrawn: 1.
- **A dig swaps its chunks, it does not cross-fade them.** `invalidate_box`
  used the LOD change's hold: the old mesh stayed until the remesh, then
  the two dithered over 16 frames, so every hole arrived ~18 frames
  (0.3 s) late whatever its size, and faded in. Now an edited key goes on
  `edit_swap_`, and `plan_work` frees its held mesh and draws the new one
  the frame it is meshed (`edit_swaps_pending()`).
- **Skirts over dug ground.** Every chunk-border cell hangs a curtain two
  voxels into the rock to hide gaps between LOD levels. Under natural
  ground that is always buried. After repeated small digs the rock can be
  a lip a voxel thick. Undercut, its normal points down and the curtain
  climbs out into the air. Thin, it pokes through into the next hole.
  Those were the spiky fins with stretched streaks (a vertical face shaded
  with its vertices' "up" normals). `cs_vertices` now drops the skirt of an
  EDITED border cell unless it goes down and neither its middle nor its
  end is in dug-out air. `--shot ... --dig-burst <n>` hammers the view's
  middle with n small digs (0.5-1.5 m, fixed sequence). With
  `--mesh-debug 1` at spawn (`--cam -96 32.5 -440 0 -0.55`, over two chunk
  borders) the visible curtain went from 1427 pixels to 0, with 0 cracks.
  A LOD seam over dug ground can show a sliver now; coarse levels
  point-sample the edits and disagree with LOD 0 there anyway.

### The frame: HDR targets and the post chain

Scene shaders write TWO R16G16B16A16F targets through `scene_out.hlsli`: the
sun's direct light, and everything else (sky, bounce, fog). Then
`post.hlsl`: horizon-based AO at half resolution from depth; a composite of
direct x screen-space contact shadow (a 0.5 m march towards the sun through
depth, for small occluders the cascades are too coarse for) + ambient x AO; TAA (Halton 2,3 jitter over 8 frames, reprojection by
depth with the unjittered previous matrix, YCoCg variance clipping, 1:9
blend); the exposure meter; then exposure, a light contrast-adaptive
sharpen and AgX into the back buffer. Y / O / J toggle TAA, AO and contact
shadows. A still renders 16 frames so TAA converges. Cost: ~0.3 ms a frame
at ground level, 0.09 of it the effects.

**The contact march must not find its own surface.** It started 2 cm
along the light, so on any slope a low sun grazes it hugged the surface
for its whole length, and the depth buffer -- one depth a pixel, ~15 cm
wide at 60 m -- came out "in front" of it in pixel-sized steps: a field of
dark squares in bands. Now it starts 1.5 pixel footprints above the surface
along the depth buffer's own normal (`normal_at`), a hit must clear the
surface by more than a footprint (`wet.z` carries the pixel angle), and the
effect fades where N.L is under ~0.14, which the cascades already darken.
It was most of the cascades' flicker too: the close-floor `--shake 60` view
went from 1.77-3.50 mean luma change a frame to 0.51-1.04 (RT's denoised
shadow, before its removal, 0.76-0.95). Walking cost within noise.

### Light in physical units (ADR-004 L1)

**Both targets hold radiance in nits.** Since L2 the light itself comes
from the atmosphere (below); `engine/render/lighting_units.h`, which compiles
as C++ and HLSL like `water_wave.h`, keeps what does not move -- the clear
MARKER the crack test and the composite read, the ground's bounce albedo,
the clamp. Every authored colour in the engine is still sRGB-encoded as typed;
`srgb_to_linear` comes first, in the shader (or in `world_gpu.cpp` for the
terrain materials). `lighting.hlsli` has the BRDF (Lambert + GGX, Smith
height-correlated, Schlick, F0 0.04) and the ADR's shading-facing GI
interface -- `gi_diffuse`, `gi_specular`, `gi_sky_visibility` -- with its
fallback: sky irradiance from above, the ground's bounce from below, which
the heightfield GI replaces where its map reaches. Terrain, trees, grass
and the sea all light through it. Trees and grass keep a little extra fill
as a STAND-IN for inter-leaf bounce (`gi_fill`).

**Auto-exposure** meters a 256-bin log2 histogram of what TAA produced and
puts the mean of its 10-90% band at middle grey 0.18, adapting like an eye
(dark vision, below). A still adapts instantly. Every `--shot`
writes the EV100, the band and the histogram to `<shot>.txt`: spawn EV100
13.7, the badlands hillside 14.9, a sea view 15.4. Shift + `,` `.` step the
compensation; `U` cycles AgX / ACES / raw.

**Dark vision: darkness is the eye's and the display's, never added
light.** No flat or minimum ambient term anywhere in the lighting -- it
would relight sealed spaces, flatten a torch's contrast and make emitters
worth less. `Renderer::DarkVision` (renderer.h; defaults there, as there
is no exposure block in archetypes.json), the tuning panel's Exposure
group (floors open / underground / lit, the lit key, both time constants,
the display floor), `--ev-floor <open> <under>`, `--no-dark-display`,
`--brightness`.

- **The floor, by context**: how far down the eye adapts, EV100 -0.5 under
  the open sky (night) and +1.5 with none over it (underground), blended on
  the CPU by the eye's open sky BY GEOMETRY -- `HeightGi::camera_open`, the
  camera pass's sky visibility with no bounce. The light share
  (`camera_sky`) counted a torch's bounce as sky and read a torch-lit tunnel
  at night as open air. The old single floor was -1, tuned for moonlit
  meadows; it let a tunnel 20 m past its mouth read clearly at noon.
- **A light in view sets the exposure; the floors only make the UNLIT
  dark.** The first dark vision held a torch-lit tunnel (walls 0.05-0.2
  nits) 1-3 stops under middle grey on the +1.5 floor, and a slow rod
  phase took 75 s to settle it after walking in from daylight: "everything
  artificially dark, and the eye opens up gradually". Now the CPU sums the
  illuminance the lights put on the GROUND under the eye (a physics ray
  down; at the eye itself a torch carried by a camera 22 m up counted in
  full and lit nothing in view) -- the carried torch always, a placed light
  only in clear view of that point (a physics ray, `set_ray_query`) -- and
  between 0.03 and 0.3 lux that becomes a light presence 0..1 (the shot
  report's `dark vision:` line). It lowers the floor towards `floor_lit`
  (EV100 -2) and, in a dark scene only (EV100 under 3), keys the meter on
  the lit part of the view: the mean of its 70th-97th percentiles
  `lit_key` (1.5) stops over middle grey, INSTEAD of the frame's mean. The
  90th percentile alone jumped between a torch-lit tunnel's two humps; the
  max of the lit key and the mean let the unlit depths' slow settle drag
  the exposure along.
- **Adaptation** (`cs_exposure`): one time constant each way, 0.8 s into
  the dark and 0.3 s into the light, twice as slow for changes under a
  stop (a flame's quarter-stop flicker does not pump the picture; four
  times left a ten-second tail). The lightning hold is unchanged; stills
  snap. `--shot ... --dig-tunnel ... --adapt-test [--torch]` walks from
  daylight into the test tunnel, holds 20 m in for 75 s and walks out, a
  simulated 1/30 s a frame (`Renderer::set_post_dt_override`), logging
  every frame's target, adapted EV100, light presence and the meter's 10th
  and 90th percentiles to `build/adapt_test.txt`. With the torch: within a
  stop 1 s after arriving, settled in ~4 s (EV100 -0.7, steady to +-0.13);
  unlit: the floor (1.5) in ~3.5 s; back to the day in ~2 s.
  `live_exposure()` reads u_expo[0..1] back every frame.
- **The display's dark floor** (`ps_final`'s `dark_floor`, after the
  tonemap): colour fades below 0.015 scene nits (gone by a tenth of it)
  towards a slightly blue grey -- rod vision, the Purkinje shift: a
  moonlit meadow (~0.005) goes grey, a torch-lit tunnel (0.05-0.2) keeps
  its colour; then true black lifts 1.5% of the range towards a dark
  blue-grey; then the player's Brightness (Graphics, saved: a gamma).
- **Measured** (the dead-end test tunnel, `--cam -96 32.5 -440 3.14159
  -0.55 --dig-tunnel --tunnel-ramp --tunnel-len 55 --tunnel-r 2
  --tunnel-depth 8 --tunnel-in <m> --tunnel-inward`), adapted EV100: 11:00
  20 m and 35 m in, unlit, 1.5 (black); 20 m in with `--torch`, 11:00 or
  23:00, -0.9 (was clamped at 1.5); a moonlit meadow at spawn, 23:00,
  -0.48, and the same with a torch carried by that airborne camera.
  `--leak-test` sealed 0.001 nits, with a torch 0.053.

**AgX, with the reference "punchy" look** (power 1.35, saturation 1.4). Base
AgX drew every view pastel; punchy matched a fitted ACES on the A/B stills
and keeps AgX's hue handling, which a torch's orange will need.

- **A debug tint is marked, not exposed.** `scene_flat` writes a NEGATIVE
  ambient alpha; the composite turns it into alpha 0, TAA carries it, and
  `ps_final` passes the colour straight through, so `--mesh-debug 1`'s
  magenta is exactly (255, 0, 255) -- it was (236, 0, 236) through the old
  tonemap. Without the mark auto-exposure meters a 1-nit magenta against a
  5,000-nit sky and draws it black. The sea's `--water-debug` views are not
  marked: pair them with `--tonemap 0`.
- **A nearly neutral sky goes grey under AgX**, and a sea view -- all sky
  and reflected sky -- meters it to a slate. The sky's hues are the pre-L1
  sky's, linearised (blue/red ~3 at the horizon); its luminance is physical.
- **Terrain has a material per biome**: archetypes.json's `materials`
  block, indexed by `material_id`, ground and rock with a roughness each,
  plus `seabed` -- presentation only, and the golden gate confirmed it. The
  MESHER blends them per vertex with layer 4's own warp, four cells and
  `blend_weight`, so a material boundary sits where the shape changes; an
  ocean cell's share goes to the seabed, which is what makes coasts sandy.
  **That share is faded out with HEIGHT above sea level** (`seabed.above_sea`,
  [full up to, gone by] metres, 1 and 10). The cell weights say nothing about
  altitude, and since v18 made the world an archipelago a landmass is rarely
  a kilometre across, so ocean cells sit among the four nearest almost
  everywhere: at the spawn, 41.6 m up under a closed birch wood, three of the
  four nearest cells were ocean and the ground came out **70% seabed** --
  trunks standing on a tidal flat, and the sward reading as stems on pale
  sand. A beach is low ground by definition, so height is the honest test and
  it costs one smoothstep. It moves the vertex WEIGHTS too, so the scans
  follow the colour and the shore stops reaching inland in both; beaches keep
  their sand (checked at the waterline), and the golden gate confirmed it is
  presentation.
  They ride in `g_arch` rows 3-4 (`kArchRows` = 5 in `real_field.hlsl`,
  `kArchFloats` = 20 in `world_gpu.cpp` -- keep them equal; `--probe-field`
  is the check, 6 um after the change) and come out in `g_vertex_mat`, a
  buffer PARALLEL to the vertex pool so nothing that reads vertices changed
  stride. Slope picks ground or rock per pixel. Cost: meshing 0.089 -> 0.102
  ms a frame; the flight's mean frame 1.83 -> 2.05 ms, walking unchanged.
- **The sea reads duller than before L1**, grey-green where it was teal: the
  old teal came from blending in display space, and a physically blended
  grazing reflection is mostly sky. L2 did not fix it -- the sky it reflects
  is real now and the sea is no better at noon (at sunset its glitter path
  is the best thing in the engine). L7 owns water reflections.

### Volumetric clouds, rain, the wet world and lightning (ADR-005, C1-C5)

`docs/adr-005-clouds-and-rain.md` is the plan: Schneider's Nubis (Horizon
Zero Dawn, SIGGRAPH 2015/2017) with Hillaire's integration and multiple
scattering, then rain. `engine/render/clouds.{h,cpp}`, `shaders/clouds.hlsl`;
archetypes.json `clouds` block. F3 / `--no-clouds`.

- **Passes** (compute, before the composite, after the atmosphere's tables):
  noise volumes once at startup (128^3 tileable Perlin-Worley + Worley
  octaves, 32^3 Worley detail); a 512^2 weather map every frame over +-64 km
  (coverage, type, precipitation from the storm dial and a 2D noise carried
  at `drift_factor` x the ground wind's drift); the cloud-shadow map; the
  march at quarter resolution each axis, jittered per frame; a temporal
  resolve reprojected by each pixel's cloud distance and clamped to the new
  3x3, written into a fixed `out` the composite reads (heap [83..108]).
- **Field**: a spherical shell on the atmosphere's planet (y = 0 on its
  surface), base 1.5 km, the top by type (stratus 0.4 km, cumulus 1.8,
  cumulonimbus 8); base shape remapped by its Worley octaves x a height
  profile by type, coverage as a threshold, edges eroded by the detail
  volume, sheared downwind with height. Beyond the map, the storm's mean.
- **Coverage is the field's own QUANTILE, measured.** The shaped field
  (Perlin-Worley remapped, times the height profile) is FLAT: its peak along
  a sun ray lies between 0.80 and 0.90 over 80% of the sky in any weather
  (`BIOMES_CLOUDS_DBG=4` writes that peak into the shadow map and the shot
  report prints its percentiles). So the threshold is cut at the field's
  (1 - coverage) quantile, 0.895 - 0.125 x cov - 0.08 x cov^8 (the last term
  closes the deck), and past it a cloud ramps to full density over 0.06, so
  the detail erosion shapes edges rather than deleting thin clouds. The
  history: Schneider's plain 1 - coverage let nothing stand under half
  cover; then 0.7 x (1 - coverage) sat BELOW the whole field, so coverage was
  really set by a density scale and the erosion, the sky went from scattered
  to closed between coverage 0.2 and 0.4, and "50% reads as overcast" was
  this. Sunlit share of the shadow map at noon now: coverage 0.17 85%, 0.35
  70%, 0.45 60%, 0.56 45%, 0.68 20%, 0.80 2%, closed from 0.93. The storm's
  coverage reaches 1 at storm 0.85 (`clouds.cpp`), where the rain is a
  downpour. `BIOMES_CLOUDS_DBG=1` paints the weather map over the screen.
- **How bad the weather is belongs to the TIME, not the place**: the storm
  dial sets coverage and type for the whole sky, and the map only varies them
  +-0.15. At +-0.55 its low spots were clear holes kilometres wide, and with
  the clouds barely drifted early in a session one sat over spawn: the sky
  looked closed at the horizon and open above you wherever you stood (a
  player's report). `BIOMES_CLOUDS_DBG=2` charts the real cover top-down --
  a ray straight up from the ground under every pixel over +-10 km, bright
  clear, dark cloud -- which the view from the ground cannot show (straight
  up it looks between the clouds, at the horizon through all of them); `=3`
  paints the camera's own rays' transmittance.
- **Check where a looking-up camera stands**: `--ground` over water puts it on
  the seabed, and the sky through the water's underside shows no clouds --
  which read as a second cloud bug until the shot report said "over 3.8 m of
  water".
- **Lighting**: a 6-tap growing cone march to the key light (sun or moon);
  the light reaching each sample from the atmosphere's transmittance table;
  dual-lobe Henyey-Greenstein; four Hillaire octaves of multiple
  scattering; Schneider's powder; ambient from the sky above and the ground
  below by height in the cloud; energy-conserving steps, out at T 1%.
  Far clouds fade into the sky-view table over `haze_km`.
- **C2, the light under cloud**: a 512^2 cloud-shadow map, +-6 km, of the
  key light's DIRECT beam through the layer (16 taps through the layer's
  real height, with the detail erosion, no floor), indexed in the cloud BASE's plane so
  a ground point finds its texel by sliding along the light -- one map for
  every terrain height. The composite multiplies the DIRECT target by it, so
  terrain, trees and grass darken in one place and no lit pipeline changed.
  And `fill_light` greys the sky terms (not the ground's bounce) towards
  their luminance by coverage^2 x 0.9: an overcast sky is as bright as a
  clear one but colourless.
- The sea's reflections read the composited sky, so clouds reflect.
- **Cost** at 2560 x 1440: walking +0.1 ms, a flight at 300 m +0.1-0.25 ms
  (median). One gale flight showed a 99th-percentile frame of 22 ms that a
  rerun did not (8.0 ms); the 40 ms worst frames appear with clouds off too.
- **The cloud shadow reaches every reader through one function**,
  `cloud_shadow.hlsli`'s `cloud_shadow_at`: the composite's direct target,
  the sea's body and glint (`water.hlsl`; under a storm deck the sea had
  gone on sparkling), and the mist's in-scatter (`fog.hlsl`, t60 in its
  shadow table -- LAST frame's map, because the clouds record after the
  mist; none until the map has been made once, `Clouds::shadow_ready`).
- **Rain (C3)**: `engine/render/rain.{h,cpp}`, `shaders/rain.hlsl`; drawn
  onto the scene target after the sea and before TAA, alpha-blended, RGB
  only. Intensity is `Clouds::precipitation()` (the storm dial past 0.55),
  or `--rain <0..1>`; `--no-rain`, Shift+F3. Nothing stored: a drop is a
  hash of its index with a FIXED world position wrapped into a box round
  the camera, so walking does not drag the rain along. Two layers --
  40k drops in 44 x 24 x 44 m, 60k in 132 x 36 x 132 m (the sheets a
  downpour shows at distance) -- falling at 8.5 m/s and carried at 0.7 x
  the 10 m wind. A streak is the path over 45 ms, at least 0.8 px wide with
  its opacity cut to match; drops nearer than ~4 m fade out (they smeared
  across the screen as wires). Occlusion is a manual depth test against
  the scene's depth (the sea's included). **Under crowns** the canopy
  map's vertical transmittance thins the drops and dims them (they see
  leaves above, not sky). The mist's density rises x(1 + 1.5 x rain).
  **Under the ground** no drop draws (`under_top_surface`: below the
  lowest of the heightfield GI map's four texels round it, less 25 cm; a
  dug shaft is in the map, so rain still falls down one). The depth test
  alone hid a drop only behind a wall: a tunnel under a wood drew the
  crowns' drips in its own air, white streaks that a dark-adapted eye
  showed at full strength -- the "sparkles" 20 m into a sealed tunnel.
  `--no-rain` (Shift+F3) now takes the drips too; with `--wet` it did not.
- **Splashes are WORLD points.** The first version picked random screen
  pixels and read the depth there, so every splash rode along with the
  view and looked fake. Now each of 2500 has a world (x, z) for its 0.4 s
  life, wrapped into a 72 m square round the camera like the drops, kept
  within 36 m. `splash_ground` takes a first height from the heightfield
  GI's top-surface map (heap [52]; splashes need `hfgi` on, the default),
  projects it, reads the depth there for the surface plane, and puts the
  splash where that plane meets its own vertical; it gives up if the
  depth disagrees (a trunk in front, ground the 2 m map does not describe)
  or the surface faces away from the sky. The map sees the seabed, the
  depth the sea, so under sea level the guess is 0 with a wider tolerance.
  Grass hides them (a depth test with 8 cm of tolerance), as it does real
  ones. On the sea a ring spreads to ~12 cm; on land only a ~4 cm crown
  -- rings on leaves read as bubbles. Never under ~1.5 px across, the
  opacity cut LINEARLY to match, so they still read at 30 m.
  `BIOMES_RAIN_DBG=1` draws them 0.3 m and opaque, `=2` also skips every
  check (on the map's height): which check drops them, and whether they
  hold still while the camera moves (two `--no-taa` shots a metre apart).
- **A drop must be brighter than the sky's mean.** At the mean luminance
  the rain vanished against an overcast sky and the mist, which are that
  grey: the sea view showed a dozen streaks. It is 2 x the mean less 0.4 x
  cover (a drop gathers the bright zenith and a highlight). Cloud cover
  barely dims it because this engine's overcast is as bright as a clear sky,
  only colourless (`fill_light`).
- Cost: walking at 2560 x 1440 in a downpour 8.09 / 8.13 ms against 8.07 / 8.07
  without -- within noise. Every `--shot` report has a `rain:` line.
- **The wet world (C4)**: `shaders/wet.hlsli`, read by the terrain, rocks,
  trees and grass through `l_wet` in the shadow constants (wetness,
  puddles, the rain now, a ripple clock wrapped at 1000 s; 800 bytes).
  - **State is the weather's, from its clock** (`weather.h`): `rain` from
    the storm dial (`rain_storm`, 0.55-0.9, shared now with the rain pass),
    and the rain of the last 45 min summed a minute at a time, each sample
    weighted by exp(-age / `dry_s` 900 s) -- the sea's lag trick, so a
    still and a long run agree with nothing stored. Wetness saturates over
    `soak_s` (120 rain-seconds); puddles from 240 rain-seconds to full 600
    later, and go first as it dries; the crowns' `drip` has its own 300 s
    memory. A held downpour is soaked with puddles; ~17 min after it stops
    the ground starts to dry and the trees have stopped dripping. `--rain`
    gives its steady state; `--wet <k>` sets wetness, puddles and drips.
    Every shot report's `rain:` line prints them.
  - **Porosity is the lever** (Lagarde's wet surfaces): soil (ground,
    0.9) darkens to x0.5 and reads MORE saturated but stays nearly matte;
    rock (0.3, 0.35 on terrain rock) barely darkens and takes a glossy
    film (roughness to 0.12-0.2), its fine normal partly filled. Wet soil
    at roughness 0.35 mirrored the grey sky and the meadow read as frost.
    Steep faces get 70% (they drain).
  - **Shelter**: the canopy map's sky visibility (a wood's floor gets
    45-100%) and the heightfield GI map -- under a top surface a metre and
    more above, dry. Under an overhang stays dry; in a cave it would too.
  - **Puddles** stand on flat GROUND (not rock, n.y > 0.93) where a fixed
    two-scale noise, the scan's own height map and the terrain's REAL
    hollows (the heightfield map's 4 m Laplacian, 2 m texels) are lowest,
    the threshold rising with the puddle value: dark (x0.3), roughness
    0.02, the normal up, and while it rains RINGS -- one drop per 0.4 m
    cell at a time, 3x3 cells a pixel, faded as the pixel outgrows a 5 cm
    ring. Most pixels leave before the hollow's four lookups.
  - **Bark** x0.55 and roughness 0.85 -> 0.4; **leaves and grass** x0.82
    and a SHEEN: a normalised Blinn-Phong sun lobe and the sky's mean
    radiance, Fresnel-weighted. The first sheen was the full GGX lobe and
    sky lookup and cost 0.55 ms walking -- leaves and blades overdraw --
    and at 0.6 strength greyed whole crowns (edge-on cards, Fresnel).
  - **Drips**: under crowns, drops the rain's roll rejected come back as
    drips while the crowns are wet -- up to a third of a downpour's count,
    slower (0.6 x) and barely blown -- and their splashes, so a wood keeps
    dripping after the shower.
  - **Puddles reflect the scene** (screen space). The scene has a THIRD
    target, the puddle mask (RGBA16F, heap [109], t15 in post): every scene
    shader writes it through `SceneOut` -- zero unless it is the terrain's
    standing water -- so grass, a rock or a tree drawn over a puddle clears
    it, and every scene PSO declares three targets (terrain, rocks, trees'
    mesh and impostors, foliage). The terrain takes the puddle's share of
    its sky reflection OUT of its ambient, exactly as `ambient_light` put
    it in, and leaves it in the mask (rgb) with its weight (a, env_brdf x
    puddle). `cs_composite` adds it back BEFORE the haze, after marching
    the reflected ray -- bent by the same rain rings (`ripple.hlsli`,
    shared) -- through the depth buffer in world space: 26 steps from 10 cm
    growing 1.3x, a 5-step bisection where it passes behind a surface thin
    enough to be the one it reached. The colour comes from LAST frame's
    TAA history, reprojected through TAA's own matrix: lit, hazed, clouds
    and all. No hit: what lies far along the reflected direction, if the
    screen shows it there (sky, or anything beyond the march). Only a ray
    that leaves the screen keeps the terrain's sky. Without the far case a
    grazing ray from a distant puddle -- which runs above the ground and
    hits nothing near -- took the bright horizon sky, and an evening's
    puddles glowed white like snow. `--post-debug 4`: green a hit, blue far
    along the ray, red the sky.
  - **FXC crashes ("internal error: compilation aborted unexpectedly") on
    a ternary between two TEXTURE objects** (`(c ? g_hist1 :
    g_hist0).Sample`) in this compute shader. Branch instead.
  - Cost walking at 2560 x 1440, wet against dry under the same weather:
    8.66 / 8.73 ms against 8.28 -- about +0.4 ms, of which the reflections
    ~0.2; the third target costs ~0.1 ms everywhere (dry was 8.19 before
    it). `validate` passed with the `weather` block's new keys
    (presentation only).
  - Not done: reflections are screen space -- what is off screen or behind
    a nearer object is not there; a far hit is fogged twice (its own haze in
    the history, then the puddle's); grass still grows out of puddles (the foliage bake is cached per tile);
    nothing runs down a slope or drips off a rock; wetness is the same
    everywhere the sky reaches -- a beach does not dry faster than a bog.
- **Lightning (C5)**: `engine/render/lightning.{h,cpp}`,
  `shaders/lightning.hlsl` (the bolt), `shaders/flash.hlsli` (its light);
  archetypes.json `weather.lightning`.
  - **A strike is a pure function of seed, clock and the camera's 2 km
    cell**: 2.5 s slots, each hashed for a strike (rate from storm 0.78,
    14 a minute at storm 1), its start, place (within 6 km of the cell),
    kind (40% to the ground) and 1-4 return strokes 40-150 ms apart, each a
    30 ms decay over a quarter-second continuing current. Peak 5e10 cd,
    about a real stroke's. The walk report counts strikes and lit frames;
    `--walk` always starts the clock at 0, so every run sees the SAME
    minute -- for the default seed, 1 ground strike in its first 15. That
    looked like a bug in the ground share until a replay of the schedule
    in Python matched the engine strike for strike (40.2% long run).
  - **Its light goes into the AMBIENT target** (`flash_light` /
    `flash_leaf`: inverse square from the channel's middle, or a point in
    the cloud, times the surface's sky visibility -- a wood's floor
    flickers, it does not light up like a field), never the direct target,
    which the composite multiplies by the SUN's and the clouds' shadows.
  - **The mist carries the flash**: in rain it is most of what is seen,
    and the clouds behind are lost in it. `cs_fog_inject` adds the flash
    and takes the frame whole while it is lit (1:12 history smeared a
    stroke to nothing); the decay after is an afterglow. The clouds glow
    round it too (diffused over ~1.5 km in the march).
  - **The bolt**: midpoint displacement from the cloud base to the ground
    (8 levels, sideways more than up), 3-7 dimmer branches, made on the CPU
    once per strike; core >= 1.2 px and a glow 10x, additive, depth tested
    by hand, drawn after the rain.
  - **Exposure holds through a flash** (`expo_flags` 2), or the frame after
    a stroke came out dark. At night the held exposure is a moonlit one and
    the flash is fifteen stops over it -- a white screen -- so the flash's
    own frames may stop down until it is 2.5 stops over middle grey; the
    stored state stays the held one.
  - **Storms are dark now** (`storm_dim`): a closed deck of tall cloud lets
    down a fifth of the sky's light -- every sky term in `fill_light`, the
    ground's bounce, the haze's in-scatter in the composite, the clouds'
    own ambient, the rain's drops. Before it a storm was a grey day at
    EV 14.4 and a flash added 2 luma; now EV 11.9, and a strike 2.5 km off
    takes the sky from luma 93 to 133. A grey stratus overcast keeps its
    light (storm 0.7: 0.98; 0.8: 0.4; 0.9: 0.2).
  - **The cloud-shadow map's edge**: past it, `cloud_shadow_at` returned
    full sun, and distant mist sliding out of the 12 km map lit a band down
    the frame under a closed deck. It takes the edge texel now.
  - Cost walking at 1440p through a storm with a flash lit a quarter of the
    time: 9.78 / 9.77 ms against 9.74 / 9.72 without -- 0.05 ms.
  - Not done: thunder (the report gives its delay; there is no audio); the
    flash casts no shadows of its own; the sea's glint does not see it; no
    sheet lightning across the cloud tops seen from above.
- **Not done (later phases)**: rain on the camera lens;
  1/16-pixel-a-frame cloud rendering (Decima's cost path); the base can
  look like a flat ceiling from the ground -- tuning.
- **"The sun through a gap in the storm" was the shadow's 10% FLOOR, not a
  gap.** The badlands lay in hard sun at storm 0.9 under a closed deck.
  `--post-debug 5` (the cloud shadow on the key light) read exactly 0.1 on
  every ground pixel: the floor was meant as "light scattered through", but
  the composite multiplies the DIRECT target, so 10% of a 100 klx sun was
  10 klx of sharp-shadowed beam beside a 14 klx sky, and auto-exposure
  brightened it back. Scattered light comes out diffuse; the overcast sky
  term stands for it. With the floor gone the map turned out opaque at
  EVERY coverage, which the floor had hidden: the shadow march skipped the
  detail erosion, so the faint fringe the eye never sees made every 600 m
  step opaque. Hence the erosion in the march, the taps fitted to the
  layer, and the coverage recalibration above. The shot report's `cloud
  shadow:` line (sunlit share of the whole map, mean T) is the instrument --
  one view's pixels sample too little sky to tell drift from calibration.
  Cost: walking within noise, clouds on or off.

### The atmosphere and time of day (ADR-004 L2)

Hillaire 2020 on Bruneton's Earth, with a real day's aerosol. The model is
`engine/render/atmosphere.h` (the medium, phase functions, table
parameterisations) and `atmosphere_march.h` (integration along a ray), both
compiled as C++ AND HLSL. The CPU (`sky.cpp`) builds the transmittance and
multiple-scattering tables once (~200 ms) and uploads them; the GPU
(`atmosphere.hlsl`) builds a sky-view table and a 32^3 aerial-perspective
volume every frame from them; the post composite draws the sky, the sun's
and moon's disks behind everything and the haze in front of every surface
by its depth. Scene shaders no longer fog.

- **The CPU computes each frame's light** -- the key light (sun, or the
  moon once it puts more light on the ground), its illuminance at the
  camera's altitude, the sky's irradiance integrated over the hemisphere,
  the ground's bounce, three sky samples for reflections -- in `evaluate()`
  (0.18 ms; re-run only when the hour or the camera's height changes) and
  hands it to every lit shader in the SHADOW constant buffer, which they
  already bound. The sea gets the same seven float4 in its own constants;
  `lighting.hlsli` reads them as `l_sun_dir` and friends. Shadows, contact
  shadows and impostor shadow cards follow the key light.
- **`--sky-report` is the instrument**, against the IES clear sky: the direct
  beam within 2-7% of the reference from 20 degrees up and the diffuse sky
  within 4-11%; the worst is x1.14 at 10 degrees, where the IES single-
  coefficient formula over-attenuates. **The shipped Mie is an aerosol
  optical depth of 0.005**, cleaner than any real day, and the report showed
  it: the sky on level ground at HALF the reference everywhere and the low
  sun's beam up to 8x too strong. 0.073 (single-scattering albedo 0.9) fixed
  both; 0.09 overshot the sky by 1.27x.
- **Every `--shot` checks the GPU against the CPU**: the sky-view table read
  back and 192 texels compared with the CPU's march at the GPU's step count.
  Mean 0.04%, worst 0.12% at noon; 0.52% worst at sunset, where the horizon
  is steepest; half-float storage is ~0.1% on its own. The shot report also
  prints the hour, the key light and its lux.
- **The sky is drawn in post, so the crack test survived.** The ambient
  target still clears to a constant -- now a marker, `kSkyClear` -- and the
  composite reads what fraction of it a blended sea left as the share of sky
  to show. Crack counts identical before and after on every view.
- **Time:** `--time <h>`; Page Up / Page Down step half an hour, with Shift
  five minutes; `H` runs an hour every ten seconds; the title shows the
  clock. Latitude 40 N,
  declination +15: sunrise 05:08, sunset 18:52. World +x is west and +z
  south, which puts 14:00's sun on the fixed pre-L2 sun to under a degree.
- **Night is a full moon opposite the sun**, 0.19 lux on the ground. The
  meter's floor decides what night looks like: anything metered darker
  renders darker than middle grey. At EV100 -2 a moonlit meadow metered up
  to a dim blue day; -1 rendered moonlight a stop under middle grey. Dark
  vision (above) made the floor depend on the open sky: under it at night
  **EV100 -0.5** by default (the tuning panel's Exposure group; +1.0 gives
  the ~2 stops darker a survival night wants), underground +1.5. The
  display's dark floor greys and blues what is under 0.05 nits (the
  Purkinje shift). No stars, no moon phases yet.
- Cost: walking 2.08 -> 2.10 ms; flying 2.05 -> 1.99 ms (the scene shaders
  lost their fog).

### Local lights (ADR-004 L3)

Torches, fires, later magic: `engine/render/local_lights.h`. Point or spot,
in lumens (a torch 200 lm, a campfire 1500), inverse square windowed to zero
at the range, flickering. Each frame `Renderer::record_lights` sorts them by
importance (lumens over distance squared; the carried torch always first),
uploads up to 256, and `lights.hlsl` bins them into 16 x 9 x 24 froxel
clusters (log slices 0.5-400 m, up to 128 a cluster). Terrain, trees and
grass walk their pixel's cluster in `local_lights.hlsli` through the same
BRDF as the sun (`direct_light`, `leaf_light` in `lighting.hlsli`).

- **The buffers reach every lit pipeline without a new root parameter.**
  They sit at heap slots [24..28] (t20..t24), and the table each pipeline
  already binds for the sun's shadow map at [0] carries a SECOND RANGE with
  `OffsetInDescriptorsFromTableStart = 24`. The cluster parameters ride in
  the shadow constants. The sea is not lit by them (its pipeline has no
  such table) -- yet.
- **Shadows: a 6144^2 atlas of 512^2 tiles** (144 MB), a cube of six for
  a point light. The 24 most important shadowed lights (8 until a ninth
  fire lit through rock) hold slots and keep
  them while they stay in the eight, so their faces stay cached; at most
  twelve faces are redrawn a frame -- never drawn or moved first, then a
  static light's faces every 240 frames so a dug hole or a felled tree
  reaches its shadow. A face draws only the chunks within the light's range
  (`Streamer::slots_near`, one ExecuteIndirect a slot), the trees through
  their own cull (`TreeRenderer::record_light_shadow`, the first cascade's
  floor: real leaves), the rocks, and the player's body -- which also
  redraws the cached faces it moves through (see The player character).
- **`--lights-report` is the instrument** -- at night, at ground level,
  frame mean over 400 frames: none 2.33 ms; torch held still 2.40; **torch
  carried at walking pace 2.51** (all six faces redrawn every frame, 72 chunk
  draws); 64 campfires 2.98 unshadowed, 3.55 with eight shadowed; 255
  campfires 4.42, 46.5 lights per lit cluster, none overflowed. Two things
  it caught in itself: a frozen clock cached the torch's shadow and called
  it free, and 255 campfires in the 64-campfire disc overflowed every
  cluster; the report now runs a clock and keeps density constant.
- **Each light's bounce is an instant fill** (`local_fill_irradiance` in
  `local_lights.hlsli`): 20% of what it puts at a distance of at least
  1.5 m, wrapped round the normal, diffuse only, its visibility one tap of
  the light's own shadow from a point moved up to 1.5 m towards it -- it
  wraps round a pillar, never through thicker rock. **Under cover it steps
  aside** (`kFillUnderCover` 0, by `nf_cover`, the near field's own blend
  rule): the near field's probes carry the bounce there. A/B, a torch-lit
  tunnel at 23:00: the fill alone (before) EV100 -0.94, the probes alone
  -0.90, both -0.65 (counted twice). In the open the probes only copy the
  heightfield, which no local light reaches, so the fill stays -- and on
  leaves and grass. A light past the 24th shadowed one lights through
  everything, fill included; the probes leave it out. `--lights-report` at 1280 x
  720, 23:00: torch held 4.27 -> 4.30 ms with the fill; 64 campfires, 24
  shadowed, 6.78 -> 7.74 (the fill's four-tap shadow cost 1.2 ms, one tap
  0.96); no lights 4.07 either way.
- **Local light goes into the AMBIENT target**, so GTAO darkens it in
  creases -- not quite right for a point light, cheap and honest until the
  composite has a third target. The torch has no visible model yet.
- Daytime with no lights: a uniform early-out skips every cluster read;
  walking 2.10 (L2) -> 2.18 ms. Spot lights are implemented, with a
  one-face shadow, but nothing places one yet.
- Keys: `F` carries a torch, `V` drops a campfire 4 m ahead, `C` clears them.

### Mist and volumetric light (ADR-004 L4)

A froxel volume, 160 x 90 x 128, view-aligned and log-spaced from 0.5 m to
6 km (`fog.hlsl`). `cs_fog_inject` gives each froxel the mist's density and
the light it scatters to the eye -- the key light through its cascade shadow
(one tap; this is what makes shafts behind trunks), the sky and the ground,
and every local light in its cluster through its own shadow (a torch's glow,
a campfire's halo) -- blended 1:12 with last frame's, reprojected, from a
Halton-jittered depth in the slice. `cs_fog_integrate` accumulates it front
to back. The composite applies it at each pixel's VIEW depth over the
atmosphere's aerial perspective, and its far end over the sky.

- **Where the mist is: `engine/render/fog.{h,cpp}`**, a 256^2 map over the
  disc baked once per seed on the CPU from `worldgen::biome_mix` and the
  heights -- density at the surface, thinning height, the surface it lies
  on (ground, or the sea). Per-biome `fog` blocks and a top-level `fog`
  block (sea fog, haze, morning, wind, patch size, phase, albedo) in
  archetypes.json; presentation only, the golden gate confirmed it. The
  shader adds wind-drifted patches and the hour (thickest just after dawn).
  **The central, safe biomes and the sea are the misty ones on purpose**,
  to keep the wider map to be found: visibility 1.5-5 km at noon, much less
  at 07:00, when the lowlands fill and the peaks stand out.
- **Densities are MIDDAY values.** The first pass authored them as morning
  mist, the morning factor multiplied UP from them, and noon was a grey
  veil; a haze floor of 8e-5/m also double-counted L2's aerosol (an optical
  depth of 0.48 over 6 km). Both fixed; haze is now 1e-5.
- **The shafts are measured, not judged**: at fixed exposure (`--ev100 13`),
  the sunward sky between the crowns of a wood at 07:00 is luma 152 with no
  mist, 195 with mist and no sun shadows, 176 with both -- the canopy takes
  back almost half of what the mist adds.
- **Every `--shot` prints the post chain's GPU times** (timestamps, smoothed
  over the still's frames): mist inject 0.095 ms by day, 0.142 at night with
  a torch and campfires, integrate ~0.02; `--no-fog` skips both passes. A
  timestamp with no barrier before it is written while the dispatches before
  it still run -- the atmosphere first read 0 ms, its time inside the
  mist's -- so a stamp goes after the transitions that follow a pass.
  Frame-time A/B could not resolve this on a noisy day (mist off measured
  SLOWER than on).
- `N` toggles the mist in the window. The sea's own sky reflection does not
  see the mist yet; distant sea under sea fog reads right because the
  composite fogs it.
- **The exploration layer is not built**: mist that lifts where you have
  been, or weather that rolls in. The hook is `FogMap` -- one density
  function, read in one place -- and ADR-004's L4 section lists the options.

### The exploration fog: layered height fog

How far the land can be seen is a game decision, not only an atmospheric
one: climbing a hill must not show the shape of the island or what lies on
the ground far off, while peaks and high things stay visible enough to want
to go to. `haze.hlsli`'s `height_fog`, applied in `cs_composite` to every
surface and to the sky (to infinity), and by the sea over itself and its sky
reflection. archetypes.json `fog.layers`; `--no-height-fog`,
`--height-fog <k>`.

**On by default.** It was off for a day (2026-11-26) while it blew out at
dawn and dusk -- a glaring orange column over the scene at `--time 18.5`
at spawn; see its colour below. With it off the froxel mist stood in at
1.3x and stopped thinning above 60 m (`mist_top` is tied to it); both are
back as they were.

- **Extinction falls off with ALTITUDE above sea level, not with height
  above the ground.** Three layers, sigma at sea level x exp(-h / H):
  ground 0.0023 / 70 m (the lowland's ~1.5 km), valley 0.015 / 40 m
  (dawn's sea of fog), deep 0.0001 / 1000 m (peaks fade by ~20 km). From
  the lowland every near-level sight line runs through the dense air; a
  summit rises out of it; from a hill above the layer the lowland a few km
  off is hidden, because the line down to it crosses km of the dense air,
  while the ground below, seen steeply, is not. The terrain-following mist
  could do none of this: it thinned over a hilltop as much as over a
  valley, so climbing cleared the view -- that was the complaint.
- **Analytic**: an exponential layer's optical depth along a straight ray
  has a closed form (`hfog_layer`), exact at any range, sky included, with
  the stretch below the base at the base's density. The froxel mist keeps
  the near field's patches, shafts and torch glow, and now thins above 60 m
  by e-folding 120 m (`mist_top`): left alone it fogged summits inside its
  6 km as thickly as the plain, and at dawn hid the far peak entirely.
- **Its colour is the sky's own horizon**, so the land fades into the
  horizon -- warm towards the sun (the table's Mie glow), cool away, dark
  at night -- greyed by cover^2 x 0.9 and dimmed by `storm_dim` as the sky
  light is. Near the eye it takes the heightfield GI's share of open sky
  (a sealed chamber does not glow).
- **The horizon is read by SCATTERING ANGLE, not azimuth** (`hfog_colour`):
  at the azimuth whose angle to the sun is the ray's. Read at the ray's own
  azimuth, every ray towards a low sun's azimuth -- 30 degrees down at the
  grass included -- took the glow two degrees from the sun: the orange
  column, green-tinged where AgX met it, at dawn and dusk, and over the sea
  even looking AWAY from the sun at dusk. (The first fix blended rays
  looking down towards a four-azimuth mean; a quarter of the aureole was
  still a column.) A level ray keeps its own azimuth, so distant land still
  fades into the sky behind it; the glow is now a disc round the sun.
- **The first 150 m take the horizon's mean round the compass**
  (`hfog_mean`, 30/90/150 degrees from the sun; `fog.layers.glow_from`,
  carried in `hfog_b.w`), the sun's angle only past it. The fog has no
  shadows, and with the angle fixed the air round the eye -- the shade of
  a wood, the lee of a hill -- still glowed with the aureole: the spawn
  wood at dusk looking west metered 216 nits against 39 with the fog off,
  128 with this. The froxel mist draws that near stretch's sunlit air
  through the cascades. Old against new at noon and 07:00 (from 120 m up)
  differ by under 1% in the meter; the composite costs the same (1440p
  1.013-1.017 ms against 1.001-1.011).
- **Thinner while the sun is low** (an aesthetic choice, not physics):
  every layer's density x0.5 while the sun is within 5 degrees of the
  horizon, either side, easing back to x1 by 15 degrees
  (`fog.layers.low_sun` [scale, full within, gone by]; `FogMap::
  low_sun_factor`, by the sun's ELEVATION, so it follows sunrise and sunset
  through the year). 18:30 x0.50, 06:00 and 18:00 x0.72, 07:00 and 17:00
  on x1; the shot report's `height fog:` line prints it. The spawn wood
  looking west at 18:30 meters 73 nits against 106 at x1; from 120 m up
  2057 against 2423. The froxel mist is untouched: its own dawn and dusk
  bumps still veil a wood's floor.
- **The valley layer** follows the mist's dawn and dusk bumps over a midday
  floor of 8% (`valley_noon`) and is blown away by wind, (1 - U / 6 m/s)^2
  (`valley_wind`). Rain thickens every layer as it does the mist.
- **Measured** (the shot report's `height fog:` line: the layers' own 2%
  contrast distance, 3.912 / sigma, along a level line; the froxel patches
  add near the ground): at 20 m above sea 1.8 km at noon, 0.7 km at 07:00,
  0.86 km in a storm; at 300 m 37 km, 600 m 71 km. The views: spawn's wood
  closes in within a few hundred metres; from 300 m over the badlands at
  noon the far summit stands as a silhouette while the coast and lowland
  sink into the layer; at dawn a sea of fog; the sea-level view of the
  island's far headland, clear before, is gone into the haze.
- **Cost**: walking at 1440p 7.05 / 7.04 ms against 6.98 / 6.99 without.
- **Not done**: the base is sea level everywhere, so a high plateau is
  clear air (highland, glacier sit at 90-110 m base, relief to 300+) --
  a regional base from the fog map, blurred over a few km, is the next
  lever if plateaus see too far; the lightning flash and local lights do
  not light this fog (the froxel mist carries them); the far terrain rings,
  far shadow and canopy shell still draw to their old reach, which the fog
  now hides -- a draw-distance cut would pay for it.

### Hardware ray tracing: REMOVED (git tag `rt-last`)

ADR-004 L5/L6 built a DXR 1.1 ray-tracing tier: denoised RT key-light
shadows (the default for a while), a traced sky visibility through crown
volumes, DDGI probes, opaque leaf cutouts for the shadow ray, and the
`--rt-report` / `--gi-report` / `--check-rt` instruments. **It was removed**:
it cost ~1.2 ms of a 1440p walking frame (8.2 -> 7.0 ms without it) plus
~300-600 MB of BLAS, TLAS and scratch, and every future dynamic thing --
built structures, animated creatures, particles -- would have needed its own
BLAS upkeep, where the cascades shadow anything that is drawn into them.
Shadows are the cascades with PCSS and the far terrain shadow; diffuse GI is
the heightfield GI with the canopy map.

Everything is in the tag: `git show rt-last:engine/render/rt_scene.cpp`,
`git checkout rt-last -- <path>` for a file, or `git log rt-last` for its
history -- `rt_scene`, `rt_denoise`, `rt_check`, `ddgi`, `leaf_cutout`,
`gi_trace.hlsli`, `rt_shadow.hlsl`, `ddgi_update.hlsl`, and this file's old
sections on them. What it taught that still applies without it:

- **This GPU (RX 7900 XT, RDNA3) traverses in shader code**: a non-opaque
  candidate in an inline RayQuery is a branch in the same loop, so "opaque
  is 2-4x faster" does not hold, and dense geometry costs real traversal.
- **One sample a pixel a frame is grain TAA cannot remove** (it clips to
  the 3x3 neighbourhood, which holds 0 and 1); it needs its own temporal
  filter. Half-resolution traces boiled on swaying trees.
- **A leak test needs the air too**: the haze and the mist assume sunlit
  air, so a sealed chamber glowed until their in-scatter near the eye was
  scaled by the GI's share of open sky at the camera (`cs_hf_camera` now).
- **Hanika's lifted shadow-ray origin** ("Hacking the Shadow Terminator")
  cured self-shadow streaks on smooth-normalled coarse facets -- worth
  remembering for any future screen- or ray-space shadow.
- **What a probe GI's "off" probe means decides its leak**: falling through
  to a coarser cascade handed daylight into a sealed room.

### Canopy shade: the canopy map (ADR-004 L6, step 2)

On by default (`--no-canopy`, `B` for A/B). `engine/render/canopy.{h,cpp}`
splats every standing tree's crown ellipsoid (`crown_volume` in
`tree_mesh.h`) into a 512 m camera-centred map of 1 m texels on the
GPU -- vertical optical depth, tau-weighted crown bottom and top, vertical
transmittance -- then blurs it a-trous into five wider slices
(`canopy.hlsl`). `canopy.hlsli`, included by `shadow.hlsli`, reads it in
`gi_sky_visibility`, so it darkens the SKY term of `gi_diffuse` and
`gi_specular` only: bounce and local lights are untouched. The map is at
heap [38] (t25), a THIRD range of every lit pipeline's shadow table; its
corner and reference height ride in `l_canopy` (ShadowConstants, 432 bytes).

- **It was judged against traced crowns** (while RT existed: `rt-last`). 16
  crowns-only rays a pixel and compares terrain pixels inside 200 m: wood
  0.32 traced / 0.37 map (mean per-pixel error 0.066), spawn 0.51 / 0.48
  (0.077), bare hillside 1.00 / 1.00. One ray a pixel made the "error"
  mostly the reference's own noise (0.31), and pixels past the map's reach
  made it look 0.10 too bright.
- **Neither obvious model is right.** Averaging transmittance over the
  footprint left a dense wood 0.10 too bright; averaging optical depth first
  matched the wood and smeared its dark into clearings. The shipped one
  mixes them 1:3 in log space (`kCanopyMix`), with the slab's 2E3 fit
  (`kCanopyPath` 1.5) for the diffuse path.
- **Cost is the trees' leaf cards, not the map.** Building every frame was
  ~0.15 ms, so it rebuilds only when the tree list changes or the camera
  moves 16 m (39 builds in a 6,000-frame walk). Read per PIXEL, the leaf
  cards' overdraw cost 0.1 ms against 0.02 ms for all the terrain, so trees
  read it per VERTEX (`vs_mesh` / `vs_impostor`; the shadow passes have
  their own entry points, `vs_*_shadow`, because they bind the frame's
  constants where the map's corner would be). Net: +0.06 ms walking.
- At EV100 13 the shaded half of the woodland floor goes from luma 82.6 to
  62.2 (the traced sky: 58.5); crowns seen from above keep their tops.

### Heightfield GI (ADR-004 L6, step 5) -- the diffuse GI

On by default (`--no-hfgi`, `Z`). The world is
mostly 2.5D, and the light that matters -- the sky the terrain round a
point leaves, the light that terrain bounces at it -- comes as often from
off screen as on. `engine/render/height_gi.{h,cpp}`, `shaders/height_gi.hlsl`
(the update), `hfgi.hlsli` (the reader), `hfgi_common.hlsli` (the layout):

- **The map**: 256 x 256 texels of 2 m round the camera. The resident
  terrain is drawn into it from above (`mesh.hlsl`'s `ps_top`, an
  orthographic `mesh_top_pso_`: albedo into RGBA16F, depth for height),
  rebuilt when the camera has moved 8 m, the clock a minute, or 60 frames
  have passed.
- **`cs_hf_light`**: per texel the height, a normal from the neighbours,
  the sun through a march over the heightfield and through the canopy
  map, and the radiance sent back -- albedo / pi times sun, sky and the
  last rebuild's bounce (every further bounce, a rebuild at a time).
- **`cs_hf_gather`**: twelve azimuths out to ~90 m, each rise of the
  horizon a band of terrain whose radiance goes into an L1 spherical
  harmonic, open sky above the last into a visibility harmonic -- analytic
  in elevation. `gi_diffuse` evaluates both for any normal: kSkyMean x
  visibility x canopy, plus the bounce, in place of the two hemispheres.
- **Above and below the top surface**: a point 40 m above it fades to an
  open hemisphere. A point more than 1.5 m BELOW it is under rock or an
  overhang; it looks for the nearest column whose surface is below it (4,
  8, 16 m out) and takes that column's light, faded over 12 m. None: walled
  in, no sky and no bounce. "Below means covered" alone drew every
  overhang's underside black.
- **The air**: `cs_hf_camera` (DDGI's `cs_ddgi_camera` before it) -- the
  map's share of an open sky at the eye, read back, scaling the haze and
  the mist near the camera. Without it the sealed chamber's walls were
  black and its air 13.8 nits.
- **Measured**: the badlands' shade (63, 37, 25) -> (70, 31, 19), DDGI
  (81, 30, 16); the wood's shaded floor 58.1 -> 55.1 (DDGI 44.8), the birch
  trunk (83, 93, 69) -> (57, 75, 54) (DDGI 35, 53, 29). `--leak-test`'s
  no-RT config: sealed and unlit **0.001 nits** (the histogram's floor),
  where the bare fallback leaks 566. **Cost +0.05 ms walking** (2.36 ->
  2.41), against DDGI's +2.8. The DDGI figures above are from `rt-last`.
  After RT's removal, `--leak-test`: sealed 0.001 nits with and without
  mist, the bare fallback 424, outside 709.
- **Not done**: crowns as bouncers (the canopy map dims the sky, nothing
  sends the leaves' green), local lights in the bounce, anything past
  256 m, and true caves seen from inside a big cavern (it sees only the
  nearest opening's light).
- Heap [48..52] the scene's SRVs (t38..t42, a sixth range of the lit
  tables), [53..55] the update's inputs, [56..61] its UAVs; the heap is 64.

### The near field: light under the top surface

The heightfield GI is 2.5D: anything more than 1.5 m under its 2 m map was
"covered", and either borrowed light from a way out found by a search or
went black -- a hard step in every dug pit and tunnel. `engine/render/
near_field.{h,cpp}`, `shaders/near_field.hlsl`, the reader in `hfgi.hlsli`
(`nf_sample`, `nf_depth`, `nf_openness`). On by default; `--no-near-field`
for A/B, `BIOMES_NF_NOREAD=1` updates it but has no shader read it (cost
A/B). The goal set for it: deep is DARK unless something down there emits.

- **The volume**: 128 x 64 x 128 voxels of 1 m round the camera, the field
  (`real_terrain_density` at 1 m) plus the player's edits (their absolute
  value at the 1 m points, scattered from the CPU). TOROIDAL: world voxel w
  is stored at w mod N, so following the camera refills the 8 m blocks that
  scrolled in (48 a frame, dug ones first) and moves nothing. A dig refills
  its blocks and dirties every probe within 12 m. Occupancy (a 1 m ramp of
  the density, R32 so typed UAV loads are guaranteed) with mips to 16 m,
  and a per-column map of the REAL top surface at 1 m (`cs_nf_top`).
- **Probes**: 64 x 32 x 64 at 2 m, 64 rays each -- one per bin of an 8 x 8
  EQUAL-AREA octahedral map (Clarberg 2008), jittered inside it by a hash
  of the probe and the update -- marched through the occupancy at 1 m to
  6 m, where the coarse level takes over (below); the coarse mips only
  SKIP empty 4 m cells. A ray that rises above the
  heightfield's top surface, or leaves the volume, carries back the
  heightfield's sky visibility and bounce in its direction. L1 SH in the
  heightfield's own convention, blended over updates (DDGI's way) by
  exp(-interval / memory): 0.35 s (`NearField::Schedule`, the tuning
  panel's Near field group, `--nf-schedule <fine s> <coarse s> <light s>`);
  an edit's probes start over. **Every probe under cover is traced every
  other frame (the LIVE LIST, below)**; the CPU's round robin, 2048 a
  frame, only places probes and copies the heightfield into the open ones.
  (Until the live list: 4096 a frame round robin with 1.5 s of memory, and
  the 8 x 8 x 8 round the camera on a 60 Hz timer. The history notes below
  are from then.)
  - **32 rays at 0.85 everywhere made the walls DANCE** where the sky came
    in through a small opening -- the bottom of a stairwell 10 m down: a
    few of the 32 rays found it, a different few each frame. Settled at a
    fixed EV100 3.5 (300 frames, then `--shake`), 39k pixels a frame
    jumped more than 20 levels; 64 rays and the box's longer memory, 10.
    The level is unchanged (luma 102 -> 107): that stairwell sees the sky.
  - **A thread a probe was latency, not work.** The pass cost 0.8 ms at
    1440p whether it traced 4,608 probes (play) or all 131k, and doubled
    with the rays: each thread walked its 64 rays in series (up to 96 steps,
    and a lit hit gathering 8 probes), and the pass lasted as long as the
    slowest. A GROUP a probe and a THREAD a ray (the SH summed in group
    memory): **0.818 -> 0.052 ms**, same rays, same image. The timer was
    meant to win that back and could not: the box at 10 Hz cost the same
    as every frame. `BIOMES_NF_PLAY=<fps>` runs a still on play's schedule
    at that frame rate after settling, and `--gpu-profile` prints probes
    traced a frame -- the two that found it. A probe in rock moves 0.9 m along an axis into
  air or switches off. Only probes under REAL cover trace -- under the 1 m
  top map by half a metre, or 1.5 m under the heightfield's (a narrow pit)
  -- the rest copy the heightfield: open air over a slope sits under the
  2 m map's texel maximum, and tracing it doubled the walking cost.
- **A ray that meets rock is lit** (`nf_hit`, step 2): the surface found by
  three bisections, its normal from the occupancy's gradient, then the sun
  (one cascade tap half a metre out along the normal -- the volume's wall
  is a metre-resolution guess at the mesh -- times the clouds' shadow) and
  the probes round the hit (read from the buffer being written, with the
  reader's rock-in-between test): every further bounce, a frame at a time.
  Times a fixed wall albedo (0.26, 0.23, 0.20) over pi. **A fine hit (under
  6 m) is lit by the local lights too** -- the frame's first 32 with a
  shadow, each through one tap of its own map, the distance held to 0.5 m
  (`kNfLightNear`). They came out once: their bounce faded in through the
  probes' history, and a ray that struck rock beside a flame (the inverse
  square at a few tens of cm) lit its probe tenfold for a frame. Back in,
  with the clamp, the short rays, and the FAST LIGHTS: a shadowed light
  that moved or took its shadow slot within 60 frames (the carried torch
  always) gives its probes within 10 m a 0.1 s memory
  (`Schedule::light_memory`, `light_reach`; the live pass tests each probe
  against up to 8 fast lights), so the bounce follows the torch with no
  fade-in. The torch walk
  with the bounce in the probes and no coarse level: the meter's 90th
  percentile 0.234-0.399 nits, sd 0.023 -- what the fill gave. A light
  past the 24 shadowed ones is left out (it would bounce through rock).
- **Far hits are lit by the local lights too** (past 6 m: the coarse
  level's, and a fine ray's that found no coarse probe -- in sealed air
  every fine ray past 6 m, since it takes no coarse bins). Until then a fire
  10 m down a tunnel reached walls more than 6 m from a probe only
  second-hand. A far hit holds the light a FOOTPRINT away -- the patch of
  wall the ray's share of the sphere covers there, 0.25 t for the fine
  level's 64 rays, 0.125 t for the coarse level's 256 (`kNfFootFine`,
  `kNfFootCoarse`) -- or one ray striking rock beside a flame 30 m off lit
  its probe tenfold. It skips a light that MOVED within 60 frames
  (`LocalLightGpu::moving`, the carried torch always): the probes seeing it
  from far keep the coarse level's 1 s memory and would trail its old
  light. A light just placed is not moving; its far bounce fades in over
  that second, and a still shows it whole. Measured at 23:00, fixed
  exposure: a campfire 24 m down the dug tunnel at spawn, walls round the
  camera mean luma 5.84 -> 6.85 (fine rays alone, `BIOMES_NF_NOCOARSE`,
  6.19); the forest cave's chamber fire lights the tunnel walls round its
  opening from 30 m up. Near field 0.097 / 0.096 ms, unchanged; `--shake`
  frame-to-frame change within the flame's own; `--leak-test` sealed 0.001
  nits, the held torch 0.0954 -> 0.0988 (it holds still in the test).
  `BIOMES_NF_NOFARLIGHTS` A/Bs it.
- **Three things made a lit tunnel pulse and creep** (the torch walk of
  `--adapt-test --torch`, its meter's 90th percentile per frame):
  - **One rotation of the ray set for every probe.** Down a straight
    tunnel the sunlit ramp at its mouth is a sliver most rays miss; on the
    frame the shared rotation aimed one up the tunnel, every probe caught
    100 klx at once and the whole tunnel flared 10x -- 16 pulses in 75 s.
    Each probe now turns the set by its own hash (`nf_probe_turn`).
  - **The sun at a FAR hit.** Turned per probe, the same rare rays still
    lit single probes tenfold (the meter wandered 0.25-1.25 nits; 0.22-0.29
    with the near field off). The sun at a hit faded out over 4-12 m of
    ray (`kNfSunNear/Far`); that fade now applies only to a fine ray that
    finds no coarse level to take. A radiance clamp at the sky's mean did
    nothing more and cut real bounce; it went.
  - **Stale probes in the toroidal store**: a block that scrolls in
    clears its probes in the fill pass, so no neighbour reads a probe from
    the far side through `nf_hit_irradiance`. The round robin's memory
    is 1.5 s (3.3 s took half a minute to settle a tunnel walked into).
- **Measured**: 8 m into `--dig-tunnel` at 11:00 and EV100 11, the walls
  round the camera 2.1 -> 5.8 luma (the sunlit shaft bouncing in), the
  floor near the mouth 73 -> 80; sealed and unlit still 0.001 nits, the
  sealed chamber with a torch 0.046 -> 0.053 (its walls bouncing). A torch
  down a STRAIGHT tunnel barely changes: it lights every wall seen
  directly, and 0.26-albedo walls send back ~2% of a lux. Frame-to-frame
  change in that tunnel view (`--shake 60`, raw frames): 0.23 levels on
  average against 0.14 with the probes unread.
- **Cones leaked; rays do not.** The first version traced six 45-degree
  cones through the occupancy mips. At 8-16 m a cone samples 16 m cells,
  where a 6 m roof averages to half open: `--leak-test`'s sealed chamber
  went from 0.001 nits to 18.5. Rays at 1 m put it back to 0.001.
- **The reader**: `hfgi_sample` blends to the probes where a point is under
  the REAL top surface (`nf_depth`, from the per-column map) by 0.5-1.5 m,
  and wherever the heightfield's old covered branch would have run (more
  than 1.5 m under its map). Eight probes, weighted trilinearly, by facing,
  and to ZERO where either of two occupancy loads on the way to the probe
  is rock -- so a tunnel under a metre of roof does not borrow the sunlit
  probes over it. Deciding cover by the 2 m map instead put ordinary bumpy
  ground in the probe path: it keeps the HIGHEST point of each texel.
- **The air**: the mist lit every froxel with the sky's share at the EYE,
  and the composite lit the haze and the height fog the same way, so a
  tunnel seen from a sunlit shaft glowed (centre luma 58; 2 without the
  height fog). Each froxel now records how much ground over it darkens it
  (`nf_openness`: 1 in the open, the probes' mean sky visibility under the
  top surface, 1 inside rock, which nobody sees), `cs_fog_integrate`
  averages that along each view ray, and the composite multiplies its old
  rule by it. Open air keeps the old rule EXACTLY: lighting it by the
  heightfield's visibility instead moved 63% of an open meadow's pixels by
  up to 47 levels, against fog tuned to the old one. With it on, the open
  spawn view differs from `--no-near-field` on 3 pixels in 921,600.
- **`--shot ... --dig-tunnel`** digs a shaft 6 m down at the camera and a
  22 m tunnel along its heading, and stands the camera in the shaft
  looking down it; `--tunnel-in <m>` stands it that far in, looking back
  (`--tunnel-inward`: further in); `--tunnel-up` looks up the shaft;
  `--tunnel-len`, `--tunnel-r`, `--tunnel-depth` its size; `--tunnel-ramp`
  a ramp in from the surface instead of the shaft. From spawn: `--cam -96
  32.5 -440 0 -0.55 --dig-tunnel --time 11 --ev100 11`. A dead end 8 m
  down, 4 m wide, entered by a ramp (`--cam -96 32.5 -440 3.14159 -0.55
  --dig-tunnel --tunnel-ramp --tunnel-len 55 --tunnel-r 2 --tunnel-depth 8
  --tunnel-in <m> --tunnel-inward --time 11`), adapted exposure: the mouth
  EV100 4.6, 10 m in 2.6, from 20 m the meter's floor (-1).
- **Three leaks a long 4 m tunnel found** (a player's report: "my eyes
  adjusted and it was very well lit, with noise"):
  - **The sun at a ray's hit took the cascade by distance.** The near two
    are fitted to slices of the VIEW; a hit behind or beside the camera
    fell outside their maps, where the border reads lit -- full sun on the
    walls of a SEALED tunnel 12 m down, adapted to EV100 0.2-3.4 from run to
    run. Hits take the third cascade, fitted round the camera: -1.
  - **A froxel in rock counted as open air** (chosen so haze over hills
    would not thin). Down a tunnel the last slice before a wall often
    samples inside it, and lit the fog along the whole ray: bright bands a
    metre apart (`--texture 0` showed them; the rock texture hid them).
    Rock now holds no air: no mist, and no part of the ray's openness
    average. Open ground still 1 pixel in 921,600 off `--no-near-field`.
  - **The heightfield's old covered branch** (where the near field does
    not reach) took light from any column within 16 m whose surface lies
    below the point -- any slope -- and lit that sealed tunnel to EV100 5.
    More than 4 m under its map it is now enclosed.
  The reader's probe visibility marches the occupancy every half metre
  instead of two samples, which could straddle a 1 m roof -- and since
  the visibility volume (below) it does so once per voxel, not per pixel.
- **The visibility volume: the reader's march, once per voxel**
  (`cs_nf_vis`, `g_nf_vis` t30 at heap `NearField::kSrvVis`). The per-pixel
  march to eight probes (`nf_clear`, up to ~50 occupancy loads) WAS the
  reader's cost -- with it removed for an A/B the terrain pass at 1440p
  lost 0.66 ms in a cave chamber and 0.39 in a dug tunnel; the probe reads
  themselves cost little. Now each 1 m voxel holds 27 bits, one per probe
  floor(v / 2) + d (d in -1..1 each way: every cell a reader within half a
  metre of v can fall in), set by the same half-metre march from the
  voxel's point to the probe where `nf_probe_mode` places it; a voxel in
  rock may climb a metre out first (the reader 0.4 m off the surface
  rounds into rock as often as not). It is computed for the blocks filled
  each frame and the held blocks round them, skipping buried rock and air
  3 m over the top map. Terrain pass, march -> mask: sealed chamber 3.20 ->
  2.65 ms, torch-lit 3.31 -> 2.73, torch-lit 4 m tunnel 1.70 -> 1.39, cave
  mouth 2.23 -> 1.93, the shipping world's dug ramp tunnel 1.79 -> 1.47.
  Stills match the march's to 1 level (the cave mouth's leaves aside), the
  thin-roof tunnels (1-1.5 m of roof, `--dig-tunnel --tunnel-depth 3.5`)
  on 0.06% of pixels, `--leak-test` 0.001 nits. Walking 6.14 / 6.36 ms
  (shipping / caves) against the march's 6.17 / 6.38. 4 MB. Every table
  that reads the probes declares t30: terrain (rocks, character), trees,
  foliage, the heightfield GI, the mist.
- **Two levels** (Split Radiance Cascades' idea, `docs/2607.20384v2.pdf`:
  a coarser level with more directions over the longer interval, merged
  into the finer; not its hash map, screen-space split or hardware rays).
  The COARSE level (`cs_nf_coarse`) is one probe per 8 m block (2048),
  placed on the block's 1 m lattice point that is air, under cover by the
  fine probes' own rule, with least rock beside it, then nearest the
  centre (open air takes the heightfield's answer in every bin; all rock,
  none) -- kept until the block is filled again. 256 rays, four per
  octahedral bin (the cascades' branching), each in its quarter of the bin
  jittered by an R2 sequence over the probe's updates, marched from the
  probe and counted from 6 m to 48 m; a bin keeps (a R, a G, a B, a vis)
  and a, the share of its rays that got 6 m clear, blended with a 1 s
  memory (`Schedule::coarse_memory`); the covered ones are on the live
  list, every frame, the R2 sequence stepping once an update. A fine ray that
  gets 6 m clear takes ITS bin (`nf_merge`) from the eight coarse probes
  round its probe -- trilinearly, by a, and only those the fine probe sees
  (`nf_coarse_seen`, once a probe by eight threads; per ray from its 6 m
  point it was the fine pass's longest chain) and, where their own 6 m
  point is air, whose point the ray's sees -- and stops; with none, it
  marches on as before. Measured:
  - **Rays pinned to bins, not a rotated Fibonacci set.** With the set
    turned per update, WHICH coarse bin a ray landed in was a lottery: a
    sunlit mouth 25 m off caught by one ray or three, and the meter
    wandered 4x over seconds. **Equal-area bins:** the plain octahedral
    map's strata run from 0.32 to 1.65 of the mean solid angle, and a
    sunlit patch in a heavy one jumped the stairwell's mouth on 202 pixels
    where equal-area gives 19. **Fresh random jitter for the fine rays,
    not R2**: the round robin returns to a probe every ~32 frames, a stride
    that walks the sequence slowly across a bin -- updates in a row all
    miss a sunlit patch, then all catch it: 8,467 pixels.
  - **The coarse level's direct sun still fades, 6 to 24 m**
    (`kNfCoarseSunFar`). Unfaded, its 4 rays a bin found a sunlit patch
    20-40 m down the test tunnel by chance; unlit and 20 m in, the 90th
    percentile wandered 4x over seconds (sd 70% of the mean). Faded, the
    patch's light comes through the fine probes beside it. Not clouds:
    `--no-clouds` gave the identical run. A longer coarse memory (3 s)
    only stretched the settle after walking in (gaps.md).
  - **A still iterates both levels four times a frame** (`kSettleIters`)
    and the coarse level takes the plain mean of a still's updates: the
    levels bounce off each other, and a still's ~17 frames at a second's
    memory left the stairwell's side tunnel 10x too dark against play.
  - Torch walk (`--adapt-test --torch`, 20 m in): 0 frames where the 90th
    percentile doubles; it holds 0.234-0.318 nits (mean 0.265, sd 0.014;
    before: 0.217-0.431, sd 0.022). Unlit: EV100 holds the 1.5 floor, p90
    0.031 nits. Stairwell (`BIOMES_NF_PLAY=60`, `--shake 300 --ev100 3.5`,
    pixels a frame changing over 20 levels): [11, 19, 6, 1, 5, 0, 4] against
    [131, 20070, 907, 20, 95, 9, 38] -- the 20k was a round-robin probe at
    the sunlit mouth taking a 30% step. `--leak-test` sealed 0.001 nits.
  - `BIOMES_NF_NOCOARSE`, `BIOMES_NF_NOLIGHTS` A/B the two halves;
    `BIOMES_NF_DBG=1` paints a fine ray that took the coarse level red and
    one that found none green, `=2` counts (placed, bin open, visible),
    `=3` never merges (the merge's cost); view with `--texture 0 --tonemap
    0 --ev100 1`.
- **Cost** at 1440p, walking: +0.17 ms before the hits were lit, +0.22 /
  +0.25 with them (8.27 / 8.40 against 8.05 / 8.15); the probe pass is
  ~0.05 ms in a tunnel since it went to a thread a ray (above). **With two
  levels** (the torch tunnel, `BIOMES_NF_PLAY=60 --gpu-profile`): 0.115-
  0.121 ms at 1440p and 4K alike (both levels off 0.074-0.076; coarse pass
  without merging 0.107 -- the coarse pass ~0.03, latency-bound by its
  48 m rays, the merge's visibility ~0.015, the lights at hits ~0.005).
  Walking at 1440p 8.34 ms against 8.35; `--lights-report` +0.06-0.09 ms
  on every row (the coarse pass, lights or none).
- **The live list, and the clock jumping.** Fast-forwarding the clock left
  tunnels lit by daylight for ~10 s after the jump: a 3,000x fall through
  the round robin's 1.5 s memory, the box's 0.35 s, the coarse level's 1 s,
  each bouncing off the others. `--shot ... --clock-jump <hours> [s]
  [ramp s]` is the instrument (`build/time_jump.txt`: the meter's 10th
  and 90th percentiles in nits, every simulated 1/30 s, after 2 s at the
  still's hour; with a ramp the clock runs there evenly, as H does).
  - **Only probes under cover trace**: 1,128 fine and 21 coarse of 131k
    in the test tunnel. Every probe every frame was 0.45 ms. So in play the
    CPU's list is classified a THREAD a probe (`cs_nf_classify`: copy, off,
    or mark to trace -- fresh if dirty), `cs_nf_live` compacts every probe
    marked to trace into a list on the GPU (and every covered coarse
    probe), `cs_nf_live_args` writes an indirect dispatch, and
    `cs_nf_probe_live` / `cs_nf_coarse_live` trace them -- fine probes half
    the list a frame (`Schedule::live_stride` 2, ~33 ms; stride 1 is
    0.138 ms against 0.104), coarse every frame (its pass is latency, a
    handful of groups of 48 m rays, so it runs beside the fine pass with no
    barrier between: a fine ray takes a bin from this frame or the last).
    Past 8,192 fine probes a frame the list is walked in longer turns.
    The counts live in two slots by the plan's parity, so no reset
    dispatch; `--gpu-profile` prints the lists' lengths and marks each pass.
  - **A jump of the light starts over.** `Renderer` measures each frame's
    change (|ln| of the key light's and the sky's ratios, plus the key's
    turn over 0.05 rad, `NearField::set_light_change`); past 1 (a time skip
    over ~10 minutes, the key passing from sun to moon) the near field
    clears its probes and settles in the frame as a still does (both levels
    iterated four times: one frame at a still's cost, unmeasured), the heightfield GI rebuilds four
    frames running with the first dropping the old bounce
    (`HeightGi::reset_bounce`), and the mist drops its history. Shortening
    every blend by the change instead (exp(-c / 0.1)) made the meter jump
    4x frame to frame through a fast sunset; with the live list a fast
    clock is followed anyway. `BIOMES_NF_NORELIGHT` A/Bs it.
  - **Measured**: the stairwell 14:00 -> 23:00, the 90th percentile within
    10% of night in **1 frame** (9.4 s with the live list and no reset,
    over 12 s before either); the pit 04:00 -> 12:00 in 0.3 s; 17:30 -> 20:00
    over 25 s (H's rate) no frame doubling, at night before the clock
    stops. The torch walk holds 0.234-0.294 nits (sd 0.010), unlit 0.029
    (sd 0.003); the stairwell's flicker [15, 19, 9, 2, 9, 1, 4];
    `--leak-test` 0.001 nits.
  - **The R2 sequence must step once an update.** It stepped once a lap of
    the coarse round robin; traced every frame, a coarse probe re-traced
    the same 256 directions 16 frames running and blended them as new, and
    the unlit tunnel's meter came in bursts (sd 0.025 against 0.003).
  - **Cost** (the torch tunnel, `BIOMES_NF_PLAY=60 --gpu-profile`, the
    near field's marks summed): 0.104 ms at 1440p, 0.106 at 4K -- below
    the 0.115-0.121 before the live list: fill 0.001, coarse round robin
    0.007, classify 0.005, lists 0.004, live 0.087. Walking 8.37 ms; the
    lights report within noise. It was +0.8 ms twice over before two finds: the
  shallow cover test (above), and **HLSL's `&&` does not short-circuit**:
  `knf > 0 && nf_sample(...)` ran the eight probes on every terrain pixel
  and threw the result away. Call anything costly inside an `if`.
- **Sky access: a ray that leaves the volume through SEALED air is dark**
  (`engine/render/sky_access.{h,cpp}`, step 1 of the underground GI plan:
  a flood over air as the base layer, the probes as detail). With caves
  back on (a scratch biome table, `cave_density` 0.7), a 42 m chamber 750 m
  from spawn -- sealed, by a flood fill -- metered 4 nits at noon and 0 at
  23:00: a ray leaving the 128 m volume took the heightfield's sky
  (`nf_escape`), and the chamber is longer than the volume. A CPU flood
  over 1 m cells (the field at a 1 m voxel and the edits, as the GPU fill
  has them) labels underground air open or sealed: air above the column's
  height plus the overhang term's reach there is open by construction;
  a search from an unlabelled cell goes highest-first through air, and
  reaching open air labels everything it saw open, exhausting the
  component labels it sealed (over 4 M cells: sealed). Per 8 m block a
  mask goes to `cs_nf_mark`, which stores sealed air in the density
  volume less 1000 (`kNfSealBias`: still air, occupancy still 0), and
  `nf_march` returns dark when it leaves the volume from air the trilinear
  weights call sealed. **The air followed**: a line of sight from sealed
  air stays in it until it meets rock, so a camera in sealed air
  (`NearField::camera_sealed`, `l_nf.w` 2) sees nothing else. Then the
  mist takes no sun and no eye's sky share (only the probes' openness),
  `nf_openness` is 0 past the volume, and the composite's haze no longer
  ramps back to daylit air past 64 m (`storm.y`). With the probes fixed
  alone the walls went black and the haze down the 146 m tunnel glowed
  white against the dark-adapted eye; each of the three took a fixed-
  exposure A/B to find. The chamber now meters 0 nits (the 1.5 floor) at
  noon and at night, a torch in it -2 EV100; every open view (sinkholes,
  the shipping world, `--leak-test` 0.001 nits) is unchanged.
  `--leak-test` now searches for its site from the spawn: from (40, 40),
  the pre-v16 centre, it found only sea on v18 and reported nothing. With no caves and no edits a block's mask is zero without
  touching the field: the shipping world spends 0 ms, a dug tunnel 5 ms
  once. A cave world's still spends 0.2-0.8 s of CPU on it.
  The `sky access:` line of `--gpu-profile` counts bricks, searches and ms,
  and the blocks not labelled yet.
- **The flood runs on its own thread** (`SkyAccess::run`). It ran in
  `plan`: flying over the sealed chamber at 20 m/s the worst frame was
  254-267 ms, now 11.2 (median 5.2 and 99th 9.1 ms either way; the
  shipping world's flight unchanged). The main thread posts requests (a
  block's mask, the camera's cell, an edit) and takes results; the worker
  owns every label. Results carry the generation of the last edit they
  saw: the main thread drops older ones, and the worker skips a request
  made before an edit the main thread has since made (it asks again).
  The edit store is read under a shared lock only while a batch of bricks
  is evaluated; `Renderer::dig` and `load_edits` take it exclusively
  (`NearField::sky_edit_lock`). **Until a block's answer comes**, its
  marks are the "not labelled" rule: air under the top surface counts as
  sealed (`cs_nf_mark`, flag 1, read from the top map -- so the mark pass
  now runs after `cs_nf_top`): dark, never a leak. A still waits for the
  worker every frame it settles, so stills are as before to the probe
  count. **An edit forgets only what it can have changed**: every
  component of air within a cell of it, by a fill through the labels
  themselves (open-by-construction cells stop it), and the bricks it
  touched. Measured with `--dig-play 240` (a small dig every sixth frame,
  each frame timed with its dig, `<shot>.digplay.txt`): 8 m over the
  sealed tunnel, worst frame 21.0 -> 7.2 ms and the flood's work 510 ->
  48 ms; digging the 42 m chamber's floor (each dig re-labels the whole
  chamber, ~0.1 s on the worker), worst frame 83 -> 7.3 ms. The GPU keeps
  a block's old marks until the new answer, so a dig that breaks into a
  sealed cave lets the light in ~0.1-0.6 s later. A/B: `BIOMES_SKY_SYNC`
  (wait every frame) and `BIOMES_SKY_FORGET_ALL` (an edit forgets every
  label) -- both set is the old behaviour.
- **Step 2: distance to the sky, not open or sealed.** With v10's cave
  densities -- caves reached through layer 7's shafts, the likeliest kind
  of cave system -- 14 of 19 sampled open columns were 200 m or more by
  path from any opening, the farthest 1,163 m, and that air read as OPEN:
  the deepest tunnel lit at noon (walls and white haze at EV100 4) and
  dark at 23:00. (The shallow and sinkhole stress variants have none of
  this: sealed, or open within 64 m.) The flood now gives every cell the
  shortest path through air to the sky -- to air with open sky straight
  above it (`sky_floor`) -- capped at the reach, 255 past it or sealed:
  for a batch of requested blocks a forward search through air from all
  their cells out to the reach, noting every sky cell met, then a search
  back from those over what it reached, which is exact for the requested
  cells and costs the air within the reach however big the cave. The GPU
  keeps a byte a voxel (`u_sky`, R32_UINT, `cs_nf_mark` writes it; the
  density volume is no longer biased) and turns it into an access
  exp(-path / L): a ray that leaves the volume brings back the
  heightfield's sky times the access of the air it left by
  (`nf_sky_access`), probes gate on access under 0.01, and the camera's
  darkness (1 - its access, `l_nf.w` 1 + it, `storm.y` in post) dims the
  air where the near field knows nothing (`nf_beyond`: past the volume,
  and its 2 m edge band) -- and, when it is total (sealed, or no sky in
  the reach), every froxel it sees, as step 1 had it: graded, a camera
  down a tunnel still sees the sunlit air at its mouth. **L is
  archetypes.json `underground` sky_falloff_m (12 m), live in the tuning
  panel's Near field group, and `--sky-falloff <m>`; sky_reach_m (96 m)
  caps the flood**, for cave designs to come. Measured: the deepest v10
  tunnel at EV100 4 is black (luma 3, the display's floor; it was lit),
  its live probes 4,162 -> 2,021; the sealed chamber stays black (3.0 at
  EV100 6); every dug tunnel and the cave views match their stills from
  before within noise, the sinkhole's cave mouth 1% darker (122.2 ->
  120.9: cave air 20-60 m from the sky); flight and dig-play frame times
  unchanged; `--leak-test` 0.001 nits. A camera rounding into rock (a
  still's camera set in a wall) takes the nearest air within 2 m.
- **Probes in sealed air with no light near do not trace** (`nf_gated`,
  `BIOMES_NF_NOGATE` for A/B). Neither sun nor sky can reach sealed air,
  so only the hits' local lights (the frame's first 32, shadowed) can: a
  fine probe is gated past a light's range + 8 m (a hit's 6 m and the
  probes round it), a coarse one past range + 48 m. Gated, a probe is held
  VALID and black -- switched off, the reader would fall back to the
  heightfield's covered branch, which can borrow light. `cs_nf_live`
  checks the gate both ways every frame over every traced probe, so a
  torch carried in wakes its probes that frame (fresh, from black).
  Measured (1440p, play's schedule): the sealed 42 m chamber 21,440 fine
  and 502 coarse live probes, 1.17 ms -> 3,501 and 0, 0.11 ms; standing
  on the surface over caves 8,022 -> 2,712, 0.58 -> 0.10 ms; the tunnel
  under spawn 9,250 -> 2,897, 0.63 -> 0.10 ms; the list pass +0.007 ms.
  With a torch in the chamber 8,215 and 462 stay live (0.66 ms) and the
  still is the ungated one's to 1 level in 255. Sinkholes and the shipping
  world are unchanged (no sealed air). Walking the cave world 6.72 ->
  6.38 ms against the shipping world's 6.15.
- **The coarse level, A/B'd in caves** (`BIOMES_NF_NOCOARSE`; play's
  schedule, fixed exposure, stills and `--shake 300` raw frames). It
  carries daylight from past 6 m and is worth it there: a dug ramp
  tunnel 10 m in is 40% darker without it (luma 9.4 -> 5.4) with a
  1,494-pixel flicker frame, for 0.02 ms; under a sinkhole's overhang the
  ceiling loses the sunlit floor's bounce (up to 51 levels). In SEALED air
  it did nothing -- a torch in the 42 m chamber and in the 4 m tunnel
  drew the same stills with it off, for 0.36 and 0.16 ms -- so coarse
  probes in sealed air are gated with lights or without, and a sealed fine
  probe takes no coarse bins (`merge`): finding which coarse probes it
  sees cost more than its rays marching on themselves. Torch-lit tunnel
  0.32 -> 0.19 ms, chamber 0.69 -> 0.38. Rejected, measured: coarse rays
  stopping at 24 m (`BIOMES_NF_COARSE_REACH`) cost the same and darken
  the dug tunnel 20 m in by a third; coarse probes every other frame
  (`BIOMES_NF_COARSE_STRIDE`) saves 0.02 ms.
- **The merge's per-ray test is for OPEN coarse probes only.** In open
  cave air the coarse level's cost was the fine probes' merge, not the
  coarse rays: per ray, a march from its 6 m point to each coarse probe's
  6 m point along the same direction, up to 14 m to up to eight probes.
  With that test removed (an A/B) the near field went 0.44 -> 0.23 ms in a
  cave mouth, the coarse level then costing nothing, and every still
  matched within run-to-run noise -- the dug ramp tunnel included. The
  failure the test was built for is an OPEN coarse probe (meta 1: the
  heightfield's daylight in every bin) over a tunnel's mouth lending a
  ray down the tunnel the sunlit hillside, so only open ones are tested
  now. Cave mouth 0.44 -> 0.25 ms, trench floor 0.46 -> 0.26, cave 0.49 ->
  0.28; dug tunnels unchanged (their mouth's probes are open). Stills and
  `--shake` raw frames match within noise in the shaft-and-tunnel, ramp
  tunnel 10 and 20 m in, cave mouth and cave; the torch walk's 90th
  percentile 0.234 / 0.235 nits, sd 0.0100 both. Two things tried that
  did nothing, so nobody tries them again: nested ifs in place of the
  `&&` chain (the compiler already skipped), and a march that jumps empty
  4 m cells (beside a surface they are never empty).
- Heap: [40..42] (t26..t28, the range every lit table kept from DDGI): the
  probes, the occupancy, the top map; [112..121] the near field's own
  (its visibility volume's SRV and UAV the last two); [122..124] the
  mist's openness volumes (t16 in post). The mist root and
  the heightfield's camera pass declare the range too.
- **Not done**: see gaps.md -- the walls' own material, emitters as a
  kind of light, and the volume's reach. How dark a deep tunnel LOOKS is
  not the near field's: it gives the light (none, sealed), and dark vision
  (the exposure floor by open sky, slow adaptation, the display's floor)
  decides what the eye makes of it.

### Screen-space bounce (ADR-004 L6, an experiment, OFF by default)

`--ssgi` (`X`). `post.hlsl`'s `cs_ssgi`, at half resolution after AO:
three slices swept both ways, each depth sample a thick occluder setting
bits in a 32-sector visibility mask (Therrien et al. 2023), gathering the
light this frame's two scene targets say leaves it. The scene reads it NEXT
frame in `gi_diffuse` (`ssgi.hlsli`), reprojected through last frame's
matrix (`l_prev_vp`, `l_prev_cam` in ShadowConstants, now 576 bytes in a
1 KB buffer) with a depth check. Heap [45] SRV (t29, a fifth range of the
lit tables), [46] UAV; `--post-debug 3` shows it.

- **It measured as not worth it here**: shade +1-2% (wood floor 58.1 ->
  59.5, a firelit night 14.5 -> 15.1, the badlands 63 -> 66 red where DDGI
  gives 81) for +0.3 ms walking. The bounce that matters in this world
  comes from terrain and canopy that is OFF screen or behind the camera:
  a slope facing the camera is lit by what is behind the camera. That is
  structural, not tuning -- a 30 m reach and distance-scaled thickness
  moved it a point.
- **A debug lesson**: a reader that tests with a CONSTANT texture value
  cannot tell a wrong uv from a right one. Four probes (`BIOMES_SSGI_DBG`
  1 bounce-only ambient, 2 without the depth check) pointed at bindings and
  slots before a uv-coloured probe showed the reprojection was right and
  the content simply small; and the post view's own auto-exposure made it
  look bright. View such things at the scene's fixed exposure.

- **Cracks are counted in the engine, as UNDRAWN pixels** (ADR-004 L0):
  below the horizon, far depth, and both HDR scene targets still at their
  clear values -- read before TAA and the tonemap, so neither can hide a
  hole. Every `--shot` writes the count to `<shot>.txt`. Trust it over the
  old sky-colour test, which TAA had ALREADY blinded: on the rough hillside
  it read 0 with post on against 49 real pinholes with post off. Far depth
  alone is not a crack either -- the sea beyond the clipmap writes none.
  The sky's clear value is `kSkyHorizon` in nits, chosen to be exact in
  half float so the test compares it exactly.
- **A skirt straight down −normal is edge-on to a wall seen face-on.** The
  rough hillside's ~50 "pinholes" (2420, 118, 3390) were one slit, 5 x 20
  px: a steep wall crossed by a LOD 0 / LOD 1 border at z = 3456, viewed 15°
  off its normal, so the curtain covered nothing and the ~1 m strip between
  the two meshes showed sky. The shot report now lists the undrawn pixels;
  `--pick` on them gave the wall, the border and the LODs in one run. The
  skirt now also leans outward along the surface, as a flange under the
  neighbour: undrawn 49 → 0. Leaning two voxels along the raw tangent stood
  dark fins up on distant ridges (a coarse LOD's field normal describes
  relief its mesh lacks), so the lean is one voxel and never climbs.
- **Occlusion is per surface, in the targets' alpha.** Contact shadows over
  foliage turned every crown nearly black: leaf cards shadow each other
  everywhere inside the march, and the cascades already shadow them at their
  own scale. The direct target's alpha says how much contact shadow a surface
  takes (ground and bark 1, grass 0.3 at the root to 0 at the tip, leaves 0);
  the ambient target's alpha does the same for AO (leaves 0.6, grass 0.4),
  because dense grass occludes itself in depth everywhere. Measured on a
  meadow still: AO and contact shadows darken the bottom half by ~9.5%
  together; at full strength it was 13.5% and read as mud.
- The sea writes its colour to the direct target as before and only REMOVES
  ambient under it (ZERO / INV_SRC_ALPHA on target 1), so seabed AO does not
  show through it.
- **The sea zeroes the direct target's alpha**, the contact-shadow mask
  (ZERO / ZERO). It writes no depth, so the composite's 0.5 m march runs on
  the SEABED and multiplies the whole blended colour, reflection and all.
  Blending its alpha as ONE / INV_SRC_ALPHA left every water pixel at 1:
  at a low sun the seabed's contact shadows speckled the sea in a band
  that flickered with the march's jitter (`--sea-shot --time 17.5`).
  Keeping the seabed's share (1 - a) still left 5x `--no-contact`'s
  frame-to-frame change in the shallows; zero matches it exactly. RT
  shadows are also traced from the seabed's depth, but agree with the
  cascades on open water (0.7% of a sea view differs, all round the
  island's trees).

### Shadows

Sun shadows, **four cascades out to 28, 100, 380 m and 1.6 km**, cast by
the terrain and the trees and received by both through one function,
`shadow_factor` in `engine/render/shaders/shadow.hlsli`. `L` in the window,
`--no-shadows` for A/B.

- **Fitting.** The near two are fitted to a sphere round their slice of the
  view frustum (a radius that depends only on the splits and the lens, so
  it never changes size); the far two to a sphere round the CAMERA, so a
  turn between their redraws cannot leave the view outside them. Centres
  snapped to whole texels, so edges do not crawl.
- **Resolution per cascade, in one array**: each draws into a corner of its
  slice at its own size (`shadow_scale` maps uv), so the ATLAS is the
  biggest cascade and the memory is atlas^2 x 4 B x 4 slices.
  **Graphics, "Shadow resolution"** (`Renderer::set_shadow_quality`,
  `--shadow-quality <0..4>`, saved; live -- the atlas is rebuilt and every
  cascade redrawn):

  | | cascades | atlas | VRAM | texels | wood |
  | --- | --- | --- | --- | --- | --- |
  | low | 1024 1024 2048 2048 | 2048 | 64 MB | 6.6 / 23.6 / 38.7 / 160 cm | 5.75 |
  | **medium** | 2048 2048 4096 4096 | 4096 | 256 MB | 3.3 / 11.8 / 19.3 / 80 cm | 6.49 |
  | high | 4096 4096 4096 4096 | 4096 | 256 MB | 1.7 / 5.9 / 19.3 / 80 cm | 9.18 |
  | very high | 6144 6144 6144 6144 | 6144 | 576 MB | 1.1 / 3.9 / 12.9 / 53 cm | 13.63 |
  | ultra | 8192 8192 8192 8192 | 8192 | 1024 MB | 0.8 / 3.0 / 9.7 / 40 cm | 19.48 |

  Texels are what one holds on the ground at the spawn wood; ms are
  `--walk 15 --uncapped` at 2560 x 1440 there (the same still in the open
  meadow at (808.9, 26.6, -873.8) is 6.00 at medium against 6.91 at high).
  Cracks 0 at every level. The shot report's `shadows:` line prints the
  level, the atlas, its memory and every cascade's resolution and texel --
  read it rather than working them out, and note the texels are about twice
  what this file claimed before they were printed.
- **Above medium every cascade is raised together, because the far ones are
  free.** They are drawn every fourth frame and mostly as impostors, and
  the atlas is the biggest cascade however the others are set, so once it is
  paid for there is nothing to save by leaving them behind: 6144 everywhere
  walks the wood in 13.78 ms against 13.73 for 6144 6144 4096 4096, at the
  same 576 MB.
- **The cost above high is FILL, not the memory**, which is worth knowing
  before anyone tries to buy resolution back by shrinking the atlas. Two
  measurements say so. `--no-trees` leaves the atlas and its VRAM exactly
  as they were and collapses the high-to-ultra gap from 10.1 ms to 1.09
  (3.92 against 5.01): nine tenths of it was leaf cards. And
  8192 4096 4096 4096 holds the SAME 1024 MB atlas as ultra -- the atlas is
  the biggest cascade either way -- yet walks in 17.3 ms against 19.4, so
  the allocation is not what is being paid for. There is no VRAM pressure
  at any level on a 20 GB card; the scans (247 MB), the slot pool (~440 MB)
  and the lights' atlas (144 MB) sit beside it.
- **It was NOT at its limit, and high is worth knowing about.** The sun's
  penumbra under a crown 15-20 m up is about 14 cm, and medium's near
  cascade is 3.3 cm -- four texels across the filter, which smears a
  canopy's leaf shadows into soft blobs. At high (1.7 cm) individual leaves
  read on a trunk and the dappling on the ground is leaf-shaped rather than
  cloudy. **High costs no more memory than medium** -- the near two draw
  into a 4096 slice either way and medium uses a quarter of it.
- **The cost is all cascade 0, and it scales with its texel count.**
  Per-cascade walks (`BIOMES_CASCADE_RES="a,b,c,d"` overrides the level's
  mix, for sweeping it), cascade 0 against the rest at 4096: 2048 6.43 ms;
  4096 9.13; 5120 11.02; 6144 13.09; 8192 17.29. Cascade 1 is worth +0.40
  doubled -- it is drawn every second frame and mostly as LOD 1 cards --
  and 2 and 3 are free. It is a smooth curve in texels, which is what makes
  6144 a real level rather than a label. `--gpu-profile`
  at the wood: `shadow c0 trees lod0` 0.940 -> 2.815 ms, its PS invocations
  44.0 M -> 169.3 M, exactly 4x. At 2048 that is **10.5 invocations per
  shadow texel**: the crowns overdraw each other and the leaf test runs on
  every layer, with no early-Z to stop it because the test `discard`s. The
  cost therefore follows how much canopy is in cascade 0 -- +2.8 ms in a
  dense wood, +0.9 in the open.
- **Not done, the next lever if high is wanted for free**: cascade 0's
  split is 28 m and its map is fitted to a sphere round that whole slice.
  Pulling the split in would make its texels finer AND its caster draw
  smaller, both for nothing, at the price of handing 18-28 m to cascade 1's
  coarser map. Untried; it changes the shadows everywhere, not just where
  the resolution is short.
- **Time-sliced** (`cascades_due`): cascade 0 every frame, 1 every second,
  2 and 3 every fourth, interleaved -- two drawn a frame. Each keeps the
  matrix its map was drawn with. **A moving caster past 28 m therefore
  shadows at 30 or 15 Hz.** Nothing moves out there yet; when creatures,
  built structures or particles do, the fix is the usual one: keep each
  cascade's static map as a cached copy and draw the dynamic casters over
  a copy of it every frame (a copy of a 4096^2 D32 slice is ~0.05 ms), so
  static cost stays sliced and moving things never lag. Four cascades every frame at 4096 was +1.4
  ms walking; sliced at 4096, +0.7; with the per-cascade sizes, **+0.1**
  (2.41 -> 2.51 ms) for four times the reach.
- **Filtering (PCSS)**: a 12-tap blocker search, the penumbra from the
  NEAREST blocker times the sun's angular diameter (0.53 degrees), never
  under 1.5 texels, then a 16-tap Poisson PCF. Pixels the search finds
  wholly lit or wholly shadowed stop there. The average
  blocker (textbook PCSS) blurred a trunk's shadow at its foot with the
  crown 15 m up; the nearest one keeps contact hard. Sunflecks now read as
  the soft discs the sun makes, not the pixel-sharp mottling 3x3 PCF drew.
- **The taps are turned PER PIXEL, and that is what TAA needs.** Under a
  canopy the shadow map is dappled at the texel (3.3 cm at cascade 0, at
  the default level) and the penumbra is a crown 15-20 m up, 0.14 m of it:
  16 taps over a disc four texels in radius are a coarse Monte Carlo
  estimate of that coverage, about +-12% a frame. **Where its error lands decides everything.** The
  rotation used to be hashed from `floor(uv * res)`, one draw per shadow
  TEXEL, so every screen pixel over a texel shared one estimate and the
  error came in blocks several pixels wide -- wider than TAA's 3x3
  neighbourhood, so the variance clamp locked on to the block and threw the
  history away. Sunlit forest floor was a mosaic of lit and unlit tiles
  that reshuffled every frame: the one thing in the engine a player calls
  noise. Hashed from `floor(uv * res * 32)` instead -- a sub-texel cell, so
  per pixel at any distance -- each pixel draws its own, the neighbourhood
  spans many draws, the history survives and TAA converges on the real
  coverage. **The estimator did not change; only where its error lands.**
  `BIOMES_PCSS` is the A/B: 0 per texel + frame (the old one), 1 per texel
  fixed, 2 per pixel + frame (the default).
  Measured at the spawn wood (`--cam 736 44.6 -288 0.9 -0.35 --time 8
  --res 1920 1080 --sea-state 0 --wind-dir 40 --ev100 10.03`), eight
  consecutive raw frames from `--shake 90`, mean |dLuma| between frames and
  the share of pixels moving over 20 levels: per texel 2.94 and 3.82%, per
  pixel **1.41 and 0.61%** -- against 1.27 with the shadows off altogether,
  so the shadow's share of the per-frame change went 1.67 -> 0.14, a 92%
  cut. The p99.9 went 99.7 -> 37.0 levels. With TAA OFF both are 9.83: the
  estimator is identical and TAA now removes 7x of it where it removed 3.3.
  What is left is fine grain on the ground, not crawling blocks.
  **Cost: none.** 1440p `--gpu-profile 120`: frame 5.673 -> 5.651 ms,
  terrain pass 0.952 -> 0.973; walking 6.36 / 6.43 against 6.47 / 6.46 ms;
  cracks 0 both; `--leak-test` 0.00101 nits as before. Where the casters are
  low-frequency there is nothing to estimate and nothing changes: an open
  meadow's still differs on 0.31% of pixels by more than 2 levels (max 21),
  and its per-frame change is 0.411 against 0.409.
- **Turn the clock AND the weather off before reading a shake.** Two
  confounds cost an hour here: auto-exposure still adapting (fix it with
  `--ev100`, or the whole frame shifts and swamps the measurement), and
  `--wind 6`, which closes the sky over this wood -- no sun, no penumbra,
  and both variants measure 0.144 and look identical. Check the still's
  mean luma before believing a quiet number.
- **Not the grass, measured.** With the shadow fixed, the per-frame change
  left is the ground cover, so a minimum screen WIDTH for a blade was built
  (`--grass-min-px`, the clamp in `vs_blade`) on the theory that sub-pixel
  blades flip their coverage as they sway. It did nothing: 1.693 at 0 px,
  1.693 at 0.75, 1.651 at 2, 1.549 at 3 -- and at 3 px the sward is visibly
  fatter. `cs_emit`'s `widen` (sqrt of the thinning) already holds a blade
  at about a pixel at every distance, so there was no sub-pixel blade to
  find. Removed; the residual is the estimator's own fine grain, which is
  what it should be.
- **Seams and the far end**: each cascade blends into the next over its
  last 12%; the last blends into the far terrain shadow over its last
  20%, 1.36-1.6 km. It used to end at 520 m with a 52 m fade, and shadows
  switched on in front of the player as they walked -- the "sudden
  appearance".
- Normal offset 1.5 texels, 0.12 m constant bias, as before. The mist's
  `key_visibility` takes one tap of the same four cascades.
- **Past 1.6 km: the far terrain shadow** (`engine/render/far_shadow.{h,cpp}`,
  `shaders/far_shadow.hlsl`; `--no-far-shadow`). The resident clipmap drawn
  depth-only from above into 1024^2 texels of 8 m, 8 km round the camera
  (heap [64]); `cs_far_march` walks each texel towards the key light, 50
  taps growing 13% a step out to ~3.6 km, and keeps the least clearance
  over a penumbra of 0.0093 x distance + a texel -- a visibility map, R16F
  at [65] (t43, a seventh range of every lit table). `far_shadow` in
  `shadow.hlsli` reads it bilinearly by four loads and fades it out over the
  map's last tenth. Rebuilt when the camera moves 256 m, the light a
  quarter of a degree, or every 600 frames. Terrain only: trees past the
  draw distance are not drawn anyway. Cost: walking within noise (2.63 /
  2.65 against 2.64 / 2.65 ms), the 60 m/s flight's median unchanged and
  its worst frame +0.15-0.3 ms -- a rebuild. It shows on far plateaus and
  valleys with the sun behind the camera; looking into the sun, far land is
  backlit already and the A/B is pixel-identical.
  **`BIOMES_FAR_DBG=1` uses the map at every distance**, which is how it is
  judged: its shadows land where the cascades' do, blurred to 8 m.
  EVSM for the far cascade remains the alternative if a prefiltered soft
  far cascade is ever wanted; its light bleeding where terrain and trees
  overlap, and 2-4x the memory, are why it was not the first choice.

The terrain casts with the depth-only draw of every resident chunk (~0.2
ms). Trees re-run their GPU cull per cascade, levelled by the CAMERA's
distance so a shadow matches its tree, with a floor per cascade: the first
keeps LOD 0 and the real leaf pattern (dappled light), the second never
goes finer than LOD 1 and tests cards at their average coverage, the third
and fourth are impostors only (the tree renderer's constant slots are
1 + 4 cascades + the light faces), each fetching one baked view's alpha along the sun. At
full detail in every cascade with four blended views per impostor pixel,
shadows cost 3.1 ms over the densest forest; with the floors, 1.7 ms. An
impostor RECEIVES its shadow from a point just outside its bound on the sun
side: its card runs through its own crown and would otherwise sit in its own
shadow. The sea neither casts nor receives.

**The coarse casters were the wrong darkness, and it showed as a band.**
The second cascade (28-100 m) drew tree shadows visibly lighter than the
cascades either side of it. Its cards kept their average coverage by a
dither keyed on the shadow-map TEXEL alone, so every card in a crown kept
the same texels and a crown could never block more than one card's worth:
the floor under a wood read luma 100 against the real leaves' 81. The
impostors in the third and fourth were solid silhouettes, 54. Now the
dither is offset per card (per tree for an impostor), so layers stack, and
the two constants in `trees.hlsl` (`kCardShadowCover` 0.25,
`kImpostorShadowOpacity` 0.78) are matched to the real leaves: 81.3 real,
79.5 cards, 82.2 impostors, mean floor luma at EV100 13.
`BIOMES_TREE_SHADOW_PATH=1|2` draws cascade 0's trees by cascade 1's or
2's path, which is the instrument: one view, one cascade, only the caster
path changes. Walking cost unchanged.

The renderer owns the one shader-visible descriptor heap -- [0] the shadow
map, [1..2] the impostor atlases -- because only one can be bound at a
time and terrain and trees both sample the shadow.

## The world viewer

`build\view.exe`, from the repository root: the seed as a map, and the
numbers a player meets, for judging seeds by gameplay rather than by eye.
It links `worldgen/` and builds at grid 1024, the engine's own, so its
spawn, biomes and flat sites are the engine's. Sliders off the shipping
values (danger wobble, danger feature size, land fraction, and the layer 0
prototype's) go to the engine with Open in engine, as `BIOMES_L0`.

- **Views** (1-5): biome map, shaded relief, danger, biomes over relief,
  slope. The slope classes are the game's own limits: 0.08 (a flat site's
  `max_grade`), 0.8 (trees' `max_slope`), 50 degrees (the character's
  climb, `physics_world.cpp`). A biome can be highlighted (the combo, or
  click its legend row); the legend gives each biome's quota and preferred
  danger.
- **Overlays**: spawn (and `find_spawn`'s 60 m ring), km rings round it,
  rivers at their channel width, flat sites (pad and shoulder), a
  coordinate grid, and under 3 m a pixel every tree and rock
  (`trees_in_cells`, `rocks_in_cells`, threaded).
- **Wheel** zooms about the cursor, drag pans, Home fits; PgUp / PgDn step
  the seed. Click pins a point for the inspector (height, slope, biome mix,
  danger against the biome's preferred, river, flat site, distance from
  spawn and whether it is on foot); right click drops a measuring marker.
- **This seed** (`tools/shared/seed_metrics.h`, a 50 m grid, ~30 ms): the
  spawn, landmasses, flat sites near spawn, and per biome its share of the
  land, the straight-line distance from spawn to its nearest ground, the
  same counting only land joined to spawn's without crossing water ("on
  foot"; `NO` if none), and how much of it is on spawn's landmass; then
  mean danger in rings of 0-1, 1-2, 2-4, 4-6, 6+ km from spawn.
- **Scan** N seeds from the current one, on every core (32 in ~3 s): one row
  each -- land, landmasses, spawn's landmass share, spawn's distance from
  the centre (`!` a fallback spawn), flat sites within 2 km, biomes not on
  foot, the furthest biome on foot -- and every number per biome in
  `build/seed_scan.csv`. Double-click a row to open that seed.
- **`archetypes.json` is watched**: save it and the world rebuilds (F5 by
  hand). A file that does not load keeps the last good table and says why
  in the status.
- **Open in engine** starts `build\engine.exe --cam ... --seed <n>` 60 m
  back from the pinned point (or the spawn), 40 m up, with `BIOMES_L0` set
  to the sliders' world (cleared at shipping). **Copy command** puts that
  line, prefixed `$env:BIOMES_L0="..."; `, and the matching `validate`
  line on the clipboard and in `build/view_settings.txt`.
- **Layer 0 prototype...** opens the proposal's controls (docs/
  worldgen-design.md, "Layer 0, proposed"): centre floor, the regime, the
  fine noise, per-seed draws, the shelf, the knee, and a button that
  recalibrates the land % slider to them (the status marks the curve
  STALE until then). "This seed" adds the knee, the seed's regime, and
  reach on foot WITH wading under 0.85 m (`tools/shared/reach.h`, what
  validate prints) beside its own no-water "on foot". They can differ a
  lot: an archipelago's islands often meet at saddles just under sea level
  (seed 0 at the 2026-11-23 archipelago settings: 1% without wading, 59%
  with, over ~1 km2 of shallows, with the shelf off).
- `view.exe --shot <bmp> [--seed n] [--mode 1-5] [--view cx cz m/px] [--pin
  x z] [--marker x z] [--highlight b] [--scan n] [--rivers] [--rings]
  [--grid] [--no-props] [--l0-window]` writes the window to a BMP and its panels' text to
  `<bmp>.txt`, and exits. Run it with `Start-Process -Wait`.

The view is drawn +x right and +z down, like the contact sheets; the
engine's +x is WEST, so it is a mirror image of a north-up compass map.

## Testing

`tools/validate` runs 200 seeds and exits non-zero on any failure. It asserts
presence, quota, radial ordering, land fraction, adjacency, edge taper,
boat connectivity, spawn, slope budget, river sanity, LOD agreement, blend seams,
trees, cave entrances, throughput and the golden hashes, and emits a 64-seed contact
sheet — look at it. Automated checks cannot tell you whether a world is
interesting.

It also *measures and prints* **islandness** — landmass count, and the share
of land on the spawn landmass — and asserts nothing about it. That is
deliberate. An earlier `foot connect` check asserted ≥75% of land be walkable
from spawn, which made a single continent a pass condition and forbade the
island clusters layer 3 is specified to produce; the design doc had left the
threshold as a literal `X%`. Whether a world should read as one continent or
an archipelago is a feel call made on the contact sheet, not an assertion.
`tools/validate/README.md` §8 has the measurement. **Land fraction is the
lever** (`--land-target`, `--land-sweep`, and a slider in `view.exe` reading in
percent). It is a **29.4% mean** since v18 (32% in v16-17, 35% before), and
layer 0 itself is now the bigger lever: with no centre floor and a fine
continent noise, a typical seed is ~34 landmasses with the spawn's holding
about a fifth of the land (v17: 8 and two thirds). `home landmass` is a
reading since v18, not a check: a bare islet is an allowed start (40 of 200
seeds start on a landmass under 16 cells). `validate --land-sweep`
tabulates the neighbouring settings. The offset is GLOBAL, never per seed:
solving per seed would flatten the spread the islandness comes from.

It also emits a shaded-relief sheet with `--relief`, and a single seed rendered
large with `--closeup <seed>`. Use the close-up for anything narrower than a
few hundred metres: a contact-sheet tile is ~87 m per pixel, so rivers are
sub-pixel there and an axis-aligned drainage network looked perfectly fine on
it.

**LOD agreement is asserted.** Octave weights fall to zero between four and
two voxels of period, faded rather than cut, and the harness checks that
every LOD from 0.5 m to 64 m matches LOD 0 within its own voxel size. The
worst case is currently 0.41 of a voxel (v10; it was 0.45 at v9). Normalise by the FULL amplitude,
never the faded one: rescaling the survivors changes the shape of what
remains, which is the thing LOD agreement forbids.

Note what the check does and does not catch. Cutting octaves ten times too
early takes it to 4.09 and fails, so it bites on over-aggressive dropping.
It did NOT notice normalising by the faded amplitude, because that error is
smaller than the worst case already present — it is a guard against the loud
version of the bug, not the quiet one.

**Caves are PLACED features since v19** (docs/worldgen-design.md, "Layer 7
-- placed caves"); the v10 ridged tunnels and their shafts stay off (every
`cave_density` 0) and their code is intact, unused. Per biome,
`archetypes.json` `caves.per_km2` (meadows 1, forest 2, the rest 0) sets a
jittered grid of 1 km / sqrt(per_km2) cells; a cell's FIRST spot decides
whether it owes the biome a cave (so the density is per km2 of the biome's
land), and up to `tries` spots find a workable one. Each cave is a pure
function of (biome, i, j), `cave_id` its save identity: a mouth bowl, a
tunnel of 8 m capsules that wanders and descends at a walkable grade, and
chambers of overlapping horizontal capsules -- all carried in layer 7's old
entrance buffers, so the carve, the GPU path and the binning are the shafts'
(`cave_wall_sd` adds wall noise and moves the feathering OUTSIDE the wall:
inside it, a 2 m tunnel pinched shut under 20-30 m of rock).
- **Cover counts the carve's real reach** -- radius, feathering, wall noise
  and layer 6's overhang drop -- every 2 m of tunnel and over a 5 x 5 grid
  per room. Checked at 8 m vertices from the radius alone, a tunnel ran out
  under the seabed and 0.7 m roofs let daylight through the near field's
  1 m grid: both showed as a GLOW deep in a cave.
- **The other glow**: a probe ray leaving the near field's 128 m volume
  through open air took the heightfield's daylight; a forest tunnel is longer
  than that. PR #6's distance to the sky (`underground.sky_falloff_m`) is what
  darkens it -- 69 -> 3 luma at EV100 2, 33 m down.
- Trees, rocks (`cave_mouth_near`) and grass (the foliage bake, within a
  metre of the carve) keep off a mouth; floors under the real top surface
  shade as rock (`mesh.hlsl`, the near field's top map).
- Streaming pads only columns that hold a cave (`cave_bottom_in_box`, in
  `column_band`'s cache and the sky-access flood's columns), not the old
  global 64 m: 808 chunks wanted at spawn, caves on or off.
- `validate`'s `cave entrances` check: the density per biome at 90% of
  per_km2 or more (200 seeds: see the PR), every capsule's axis air from mouth
  to chamber, every mouth open, no breach past the mouth. `--no-caves` A/Bs;
  `--cave-sites <seed>` lists them nearest spawn (and any breach).
- In the engine: **Insert** jumps to the next cave nearest first (Shift
  back); `startup.txt` lists the eight nearest as `--cam` lines;
  `--cave-walk <k>` walks into the k-th nearest unsettled, a frame every
  1.5 s (`build/cave_walk.txt`); `--post-debug 6|7` the direct and ambient
  targets alone. `BIOMES_L0="caves=0"` builds the world without them.
  `view.exe` draws them with the flat sites.

**Forage is pinned the same way** (v20): at the same 24 sites, 5 x 5 cells
of each species, by its plants (the sixth per-layer hash).

**Rocks are pinned the same way** (v14): at the same 24 sites, boulders over
a 64 m tile and outcrops over 3 x 3 of their cells, with their own
per-layer hash (the fifth in the gate's failure report).

**Trees are pinned by their instances**, like layer 7: a 64 m tile at each of
the first 24 land cells, every tree's id, species, variant and quantised
placement. The `trees` check compares what each biome GREW against what it
was configured for (density x cover): rank agreement at least 0.8, treeless
biomes under 5% of the densest, and no treed biome bare on more than 2% of
seeds. It credits each candidate to the dominant biome at ITS OWN position --
crediting by the sampled cell's site put 14 trees a hectare in rift, which
plants none, because the biome lookup is domain-warped by up to 1.3 km.
`validate --tree-reasons` tabulates where candidates go per biome (clearing,
roll, shore, slope, river, altitude, surface); `--tree-sites <seed>` names
the nearest tree of each species and the densest stand, for a `--cam`.

**The golden hash cannot see everything, and layer 7 proved it.** The hash
samples a 1024² grid — one density sample per 156 m — and a world has ~24
entrance shafts a few metres across. Adding the whole layer left all eight
hashes byte for byte unchanged, which would have left every entrance free to
drift through CI. The shaft buffer is now hashed directly. The general rule:
**a layer whose features are smaller than the hash grid is invisible to it and
must be pinned by its buffer.** Layer 5 escapes this only because 428 river
segments per seed carve a wide enough area to land in the samples.

The hash still does not see whether the shafts are *open*: disabling
`entrance_carve` entirely leaves the stored geometry, and so the hash,
unchanged. That is what the `cave entrances` check is for. Neither covers the
layer alone — both mutants were tested.

**Golden hashes are pinned, and since M2 the BUILD checks them.** Any change
to `worldgen/` moves them, and `cmake --build` then fails — you do not have to
remember to run anything. Eight seeds, under a second, re-run only when
something upstream of the hash changed.

The version lives in `worldgen/generator_version.h` as `kGeneratorVersion`;
`golden.json` records the hashes *and the version they were pinned at*. Two
places, because one cannot tell a deliberate bump from an accidental drift.
Three states, and only the first is a pass:

| version vs the pin | hashes | what it means |
| --- | --- | --- |
| same | match | the world is what it claims to be |
| same | moved | **a silent world change** — revert, or bump the version |
| differs | either | the pin is stale: `validate --bless` |

**`--bless` refuses the second row.** It will not write moved hashes back
under an unchanged version, which is what makes the gate a gate rather than a
speed bump — "make the red build green" has to go through editing
`generator_version.h`, where the history comment makes you say what changed.
The sequence is forced, and it was tested by moving `blend_width` 500 → 505:
build red, bless refused, version bumped, bless accepted, build green, all
reverted.

Run the full harness before any commit touching `worldgen/` anyway — the gate
only asks whether the world is the pinned one, not whether it is any good.

### The engine's own checks

The GPU cannot be hashed (invariant 8), so CPU-to-GPU agreement is a tolerance
comparison: `engine.exe --probe-field` meshes one chunk and compares its
density grid against `worldgen::terrain_density` corner by corner. At M1 that
was **6 µm over densities spanning 18 m**. On v18's rougher ground layer 6's
overhang term widens it: 31 µm at the spawn (the default chunk since v18),
up to 20 mm at (-4000, 1500), 0.2 mm there with `--no-overhang` (v17's
layer 0 at the same spot: 1.6 mm). It is also the first thing to run
when the engine renders nothing — it says whether the two disagree, or whether
they agree and the chunk really is empty.

Three engine numbers look alarming and are not, as long as the voxel is
printed beside them: the height residual (worst 10.0 m), the mesh-vertex
residual (worst tens of metres) and the dig test's fall distances. A surface
vertex is placed by linear interpolation along ONE edge, so its density is
only as near zero as the field is linear over one voxel — at an 8 m voxel
beside a cave, where `cave_carve` pins the air at -10, the worst case IS 10.
Every one of these now reports the voxel size it came from, because without it
the number cannot be read at all.

**An engine mode whose window never gets a size renders nothing, classifies
nothing — and its time-driven flags still fire.** One `--overhang` run in
roughly eight came back with 0 frames, 0 chunks and 0 physics bodies, and
concluded "it was STOPPED by the overhang" from that. The report now says so
at the top when `chunks_classified == 0`. Treat an all-zero engine report as a
void run, not a pass, and check the counts before believing a conclusion.

**The near-black patches on distant terrain are geometry, not a fault**, and
the way to tell is worth keeping because the eye gets it wrong. They read as
black beside lit ground; measured, the darkest pixel in frame is (22, 29, 21)
and `mesh.hlsl` cannot go below `rock * 0.22` ≈ (19, 26, 16), which is a
normal pointing straight down. Three causes are distinguishable without
guessing. (The colours below are the pre-L1 display path; under exposure
and AgX they no longer hold, so reproduce them with `--tonemap 0` -- or,
better, count holes with the undrawn test, which is tonemap-proof.)

| what you see | cause |
| --- | --- |
| the clear colour (102, 128, 168) below the horizon | a hole — missing geometry |
| pure black | a NaN normal, which `saturate` collapses to zero |
| the shader's floor, ~(19–32, 26–42, 16–40) | a down-facing face, correctly shaded |

It is the third: zero pure-black pixels, zero sky-coloured pixels below the
horizon, nothing below the arithmetic floor. And they are a coarse-LOD effect
rather than a mesher fault — per terrain pixel there are **280× more** of them
in a 1400 m vista than in a ground shot, where inverted normals would appear
equally at every level.

**Not settled, and twice now a confident explanation has been wrong.** The
first was "aliased overhangs"; chasing it found that `field_normal` was using a
32 m arm (below), which really was a bug and really did produce faces lit as
though they pointed away from the sun — and fixing it did not remove the
patches. What is ruled out, by measurement: holes, NaN normals, inverted
normals, and the normal formula. What is left is that they are geometry —
zoomed in they have a lit sunward rim and a dark shadow face, so they read as
coherent raised shapes.

**`--shot ... --pick px py ...` now gives that world position**, and it moved
this forward. On v9's 1400 m vista, 8 dark blobs picked: all on LOD 6–7 (32 m
and 64 m voxels), and at 6 of the 8 the TRUE field surface under the pixel
faces UP (normal y 0.90–0.98, gentle ground). So the dark is not in the
terrain. It is in the coarse mesh drawn over it, whose vertices sit up to
tens of metres off the real surface. Untested next guess: such a vertex
samples the field gradient somewhere it does not describe the ground, such
as the cave band. The near-field dark SEAMS were a different bug (the blend
seams below), and fixing them left these blobs where they were.

**Both pixel detectors above stop working once water is in frame.** The sea
sits near the clear colour, so a crack count on a seascape returns thousands of
"cracks" that are just water. Mask the water before trusting either number.

**The sea** is `engine/render/shaders/water.hlsl` over
`engine/render/water_wave.h`. That header compiles as C++ AND as HLSL, the same
trick `worldgen/shaders` uses, so `--sea-report` measures the function the
shader draws. It is **not** the ADR-001 dialect and invariant 9 does not reach
it: the sea feeds no hash, no save and no generator decision, so `sin` and
`cos` are allowed here where they are forbidden four directories away.

Wave height is set by the WEATHER (below), because storms are a planned
mechanic. The surface mean stays at sea level to 4.5 mm in every case
`--sea-report` tries -- the one thing worth asserting, because a sea that rose
with the weather would flood coastlines and no screenshot of a rough sea would
look wrong.

Controls: `--sea-shot` (stands the camera off the nearest coast, over 4-12 m of
water), `--water-debug 1..8` (depth, foam, normal, displacement, surviving
significant height, SSR source: green screen / red sky, the folding measure J,
the gusts' ripple strength;
marked as debug, so shown unexposed), `--no-water`, `--no-ssr`,
`--no-wave-detail`, and `K` / `F7` / `F4` in the window.
Weather controls are under the weather section.

**The sea draws AFTER the composite (ADR-004 L7)**, onto the lit scene, in
`record_water` between `cs_composite` and TAA. It reads copies of the scene
and depth (heap [79..80]) and writes colour opaquely and its own depth, so TAA
reprojects the surface and the crack test sees water as drawn. The sea, not
the composite, now applies the haze and mist over itself (`haze.hlsli`,
shared with post.hlsl). This makes the older notes about the sea blending into
the two scene targets, zeroing the contact mask, and breaking the pixel
detectors historical.
- **Refraction**: the seabed from the scene copy, shifted by the normal's
  tilt (`refraction` 0.04), refusing shifts onto things in front of the water,
  dimmed per channel by `extinction` along the view leg AND the leg down to
  the seabed, filled by `deep_colour` x the light. `storm_turbidity` makes a
  rough sea murky. All in archetypes.json's `water` block.
- **SSR**: a world-space march (28 steps growing 1.35x, to ~3.8 km) against
  the depth copy, binary refinement, and a hit only if the refined point is AT
  the surface: rays that cleared a hilltop from the water but ran behind it as
  the camera sees it gave every headland a slab of reflection. Skipped where
  Fresnel < 0.06, faded out as the wind rises. Misses take the sky from the
  composited scene where the reflected direction lands on sky
  (`sky_reflection`); the sky table alone has no mist and left a blue band
  under a grey horizon. Reflections leaving below a roughness-dependent
  horizon take the water's colour (`open`): clamped, they lit every gale back
  slope with the horizon's beige and sent SSR along the sea into headlands.
- Shore foam is drifting value-noise lace, thick at the waterline.
- **Cost** at 2560 x 1440, the sea filling the frame: 0.83 ms light wind
  (SSR 0.39 of it), ~1.0 ms gale; was 0.25 / 0.38. Walking where the sea is
  incidental: +0.25 ms (the two full-frame copies and the grid).
- **Not done**: a faint strip just under the horizon where SSR misses land.

### The shoreline: swash and wet ground

The trains die over the shoal, so the sea's edge used to sit at the
still-water line in every weather, the sand dry to it in a gale as in a calm.
One function in `water_wave.h` now gives the water running up the beach and
back -- `ww_swash` -- and three things read it, so they agree on where the
water is: the sea's sheet (water.hlsl), the wet ground under it (mesh.hlsl)
and the swimmer (`ww_sea_at`, physics_world.cpp).

- **The sheet**: within 1.5 m of water depth (`ww_swash_reach`) and only on
  the LOW beach (`ww_swash_band`: gone 0.6 m over the run-up, so an inland
  column never carries it) the sea's vertices rise by the swash; the terrain
  in front hides it where the beach is higher. Its height rides the eight longest trains' elevation at the
  point over their rms, so the uprush comes in sets and varies along the
  shore, times ~7 m lobes of fixed noise (a single contour drew a
  straight-edged slab). Reach: Stockdon's gentle-beach run-up, 0.043
  sqrt(Hs x 100 m), 0.12 to 1.2 m (`Renderer::update_weather`). 0.7 Hs, a
  steep beach's, ran a 2 m sheet up a meadow coast in a storm.
- **Never below the bed**: where wet, the sheet is clamped 3 cm over the
  ground, so storm troughs over shallow water no longer bare the seabed.
- **The edge is made per pixel from the GROUND** (`edge_fade` in
  water.hlsl): the ground point behind the pixel comes from the depth copy,
  the swash is evaluated right there, and the water fades into the scene
  over its last 8 cm of depth under sea level + swash -- a true contour of
  the beach. The sheet's geometry reaches 30 cm of depth past the edge,
  riding 12 cm over the generated ground, so there is always water drawn for
  the pixel test to fade. No clip: where the sheet is drawn is the depth
  test's (below).
  Two attempts drew a staircase, worst looking along the shore: the sheet
  a few cm over the beach is nearly coplanar with it, so where they cross
  follows both meshes' triangles (and the vertex-interpolated clip stepped
  with the grid); then fading by the thickness between the SHEET and the
  ground only softened the steps, because near the shore the sheet's own
  height is interpolated across grid cells.
- **Foam** is the contact foam (below): the water's vertical thickness over
  the ground behind the pixel. A band keyed to 2.2 m of water turned the
  whole swash sheet white.
- **Wet ground** (mesh.hlsl, before lighting; the swash rides in the shadow
  constants, `l_swash_a/b`, `l_swash`): damp from the waterline to the full
  run-up + 0.2 m, fading over 0.4 m -- albedo x 0.55, roughness to 0.35 --
  and a film in the 30 cm the water has just left -- roughness 0.06, the
  scan's normal flattened. At 0.85 of the run-up the damp band ended about
  where the sheet stood and barely showed. Grass shows it modestly; this
  session had no sandy beach to judge it on.
- **Physics**: water is present where the running sheet covers the ground
  (`ww_sea_at`, without the swash where the feet are dug);
  `--swim 40 --wind 20` PASS, eye under 2.3%, fewer frames wading (the sheet
  covers the shallows). The `--swim` turn still shows a strip of grass at the
  bottom edge, as it did before this change; not run down.
- Cost: none measurable (the terrain branch only runs within ~1.5 m of sea
  level; walking 7.96 / 7.74 ms with and without water, as before).
- Not done: the swash in motion has only been judged in stills; no sandy
  beach checked; no drying time (wetness is the run-up band, not a history).

### The sea's ripples and whitecaps

`engine/render/ocean_detail.{h,cpp}`, `ocean_spectrum.h` (compiled as C++ and
HLSL, so the CPU sums the spectrum the GPU synthesises), `shaders/ocean_fft.hlsl`.
F4 / `--no-wave-detail` for A/B.

- **Two 256^2 FFT tiles a frame**, 31 m and 7.7 m (not multiples, so the
  repeats never line up), carrying 4.5 m down to ~6 cm and splitting at 0.6 m
  over a 15% overlap whose two bands sum to one. Phillips round the weather's
  wind (`sea_wind`), 7% kept against it. Each texel's frequency is fixed and
  the weather only reweights amplitudes, so a veering wind slides no phase.
  The random field is a hash of the texel: nothing stored. Three complex
  IFFTs a tile (two real fields packed in each): slope x + i slope z, the
  choppy displacement's derivatives, then the output (slope x, slope z,
  J - 1, foam). Rows and columns are one group a line, all eight radix-2
  stages in group memory. `half` is an HLSL type: not a variable name.
- **Never geometry, never physics.** The trains (`water_wave.h`) stay the
  surface the grid, the swimmer and every height query use.
- **Normalised to Cox-Munk**: the CPU scales the ripples so the trains' mean
  square slope plus theirs is 0.003 + 0.00512 U, at least 30% of it ripples.
  Re-summed only when the wind moves 2% or half a degree, only where each
  band is (~33k spectrum evaluations). Every shot report prints the budget:
  at 6 m/s, Cox-Munk 0.034, trains 0.015, ripples 0.018.
- **Distance**: a tile fades as the pixel's footprint (stretched along a
  grazing view) reaches a quarter to a half of its shortest ripple -- 0.6 m
  and 6 cm -- and the slope it no longer resolves widens the sun's lobe
  (Beckmann: exponent ~2 / m^2 - 2, floor ~900). Near the camera every facet
  is its own sparkle; far off they merge into a glitter track. This replaced
  a hand-set exponent on the wind (900 to 60, then 250).
- **Whitecaps where the surface FOLDS**: the trains' Jacobian
  (`ww_surface_with`'s new out parameter) plus a third of the ripples'. The
  threshold is MEASURED: `--sea-report` 3b gives the trains' J at Monahan's
  whitecap share (3.84e-6 U^3.41), 0.63 at 4 m/s to 0.84 at 20, and the shader
  uses a quadratic through it. A gale frame's lower half measured 10.6% foam
  against Monahan's 10.5%. At full weight the ripples' J made 25-30%, white
  speckle all over; without the ripples the old thresholds made none.
- **Trailing foam** lives in the 31 m tile's alpha, laid where the ripples
  compress hard (J < 0.85) and decaying over ~2 s IN THE TILE, so it streaks
  behind the moving compressions; it textures the whitecaps and, past 10 m/s,
  is laid lightly over the sea. Laid at any compression, it covered the tile.
- **Reflections**: SSR's ray and the "closed by the next wave" test (`open`)
  use the trains' normal, not the ripples': with the ripples' tilt in `open`,
  calm water at a grazing view was pocked with dark spots.
- **Cost: +0.03 ms** (1440p flight over the sea, 4.62 / 4.59 light wind,
  4.87 / 4.84 gale); ~3 MB of fields and a 1 MB output.
- **Gusts on the water (cat's paws)**: the sea samples the same gust field
  the grass and trees bend in (`wind.hlsli`, `wind_v` / `wind_drift` in the
  water constants, at p - drift), and scales the ripples by the local wind's
  Cox-Munk slope over the mean's, with a threshold -- none under 1.2 m/s,
  full by 3.8 (a softer 0.5-2.5 never let a light air's lulls go glassy).
  The 7.7 m tile answers in full, the 31 m by 60%; the trains not at all.
  Where the ripples are below a pixel, their unresolved rms slope still
  raises the angle Fresnel sees (+0.8 x) and lifts the reflected sky ray
  (+1.0 x): that is what makes a ruffled patch read dark against glassy water
  far off, where the glint width alone showed nothing. SSR fades by the
  local wind, and foam's wind is half local. `--water-debug 8` shows the
  gusts' ripple strength (grey = the mean). Cost: nothing measurable. Gale
  foam came down to 8.3% of the frame against Monahan's 10.5%: the gust
  field is skewed, most of the sea a little under the mean.
- Once in this session a shot failed at init with "ocean detail pso cs_cols"
  and the next run hung; every run since, cold and cached, was clean. Not
  reproduced or explained.

### Weather: the wind, and the sea it raises

`engine/render/weather.{h,cpp}`, tuned by archetypes.json's `weather` block.
One state that grass, trees, the mist and the sea all read; before it, grass
and trees blew one fixed way, the mist another, and the waves ran on headings
no wind chose. Presentation only -- validate and the golden gate passed with
the block added.

- **A pure function of the seed (`field_id`) and the frame's clock**, the
  clock the waves and the swimmer use, so saves, replays and stills agree.
  The storm dial (0..1) is a slow noise (period 8 min); wind = 0.5 + 23.5 x
  storm^2 m/s; the heading wanders +-100 degrees over 15 min. The wind sea
  answers the wind averaged over 90 s; the swell (0.25-3 m Hs, ~110 m long,
  its own heading) the storm averaged over 10 min, so it outlasts a gale.
  Six hours of four seeds: calm 31%, light 25%, moderate 13%, fresh 19%,
  strong 6%, gale 6%.
- **Drift** is the integral of the wind, cached and advanced each call.
  Anything the air CARRIES (the mist's patches, the gust field) moves by it,
  never by wind x t, which jumps as soon as the wind changes.
- **`wind.hlsli`**: gust cells ~90 x 45 m (stretched along the wind) and
  faster turbulence, sampled at p - drift, averaging 1x the mean speed.
  Grass bends with speed^2 to lying over (`wind_at` in `foliage.hlsl`).
  Trees lean downwind (speed^2, 3% of height in a 20 m/s gale), swing about
  the lean (speed^1.5) and flutter, worked out in WORLD space and turned into
  the model's frame, sampled at the trunk so a gust crosses a wood tree by
  tree. Frequencies stay fixed: one that followed the wind would slide its
  phase by t x the change. The mist drifts at `mist_drift` (0.5) of the wind.
- **The sea is 32 FIXED trains** (`water_wave.h`): golden-angle headings,
  log-spaced 157 m to 4.1 m. The weather sets only their AMPLITUDES. A
  heading or wavelength that changed at runtime would slide the phase by
  k x |x| x the change, faster than the wave travels 5 km out, so a veering
  wind hands energy to the trains already pointing its way. Sixteen trains
  was too few: two carried a gale's peak 85 degrees apart, an egg-crate
  lattice of crests that the crest foam drew as a grid of squares.
- **Spectrum**: Pierson-Moskowitz capped by fetch-limited (JONSWAP) growth
  over `fetch_km` (150), its peak frequency times `peak_shift` (1.4) -- a
  real gale's 120 m, 1-in-30 peak reads as a slow heave -- cos^2 spread round
  the wind; the swell a narrow lump, cos^8. Each normalised to its Hs, so
  the height does not depend on which trains point downwind. Trains under
  1% of the biggest height and 2% of the steepest slope are zeroed and
  skipped. Deep-water Hs: 0.26 m at 3.5 m/s, 0.77 at 6, 1.98 at 10, 3.96 at
  20; a 20 m/s gale with its swell is 7.2 m crest to trough in the report's
  patch.
- **Depth tames the waves** (`ww_depth_factor`): each train times
  tanh(2 k h) -- long swell dies first over a shoal, short chop survives
  into a lagoon -- then the local Hs is capped at 0.55 x depth
  (`WW_BREAK_HS`), so the biggest wave is about the depth, where it breaks.
  The first cap was on the SUM of amplitudes (every crest at once), ~3x what
  a random sea shows: a coast in a gale went flat. Plain tanh(k h) also took
  half the swell off coast-side cameras. Speed does NOT vary with depth:
  w(h) inside `k x - w t` winds the wavefronts into stripes as t grows.
- **Physics floats on the same sea**: `SeaParams.sea`, depth from the column
  height at the swimmer (the shader reads it at the rest point; centimetres
  apart on a beach). `--swim 40 --wind 20`: 0 frames standing in deep water,
  eye under 2.3% of frames (11% on the old storm).
- **Cost** (`--flight 10 --settle --flight-from 1638 12 838 --res 2560
  1440`, the sea filling the frame): the old 4-train sea 0.14 ms, now 0.25 ms
  in light air and 0.38 ms in a gale. The per-pixel normal takes its
  breaking scale and Q from the vertex, and the depth term uses a rational
  tanh (`ww_tanh_pos`): HLSL's tanh at 32 trains three times a pixel was
  +0.69 ms on its own.
- **Controls**: `--sea-state <0..1>` fixes the storm dial, `--wind <m/s>`
  the speed, `--wind-dir <deg>` the heading (0 = +x, 90 = +z), `--swell <m>`
  the swell's Hs; in the window `[` `]` step the dial, `;` turns the wind
  45 degrees, `\` goes back to automatic. The title shows wind and Hs.
  Every `--shot` report has a `weather:` line (and depth and Hs 25-400 m
  ahead); `--sea-report` tabulates wind, swell, depth and six hours of
  automatic weather, and asserts the mean, the inversion, growth with depth
  and no wave taller than 1.1 x its depth.
- **Trees in the wind.** The sway is `engine/render/tree_sway.h` and the
  gusts `engine/render/wind.h`, both compiled as C++ AND HLSL (`wind.hlsli`
  only adapts them) -- so the CPU could lean the RT scene's instances by the
  formula `trees.hlsl` bends the mesh with, while RT existed (`rt-last`). `small` is a Windows macro (`#define small char`): not a
  variable name in a header the C++ side includes.
  - **Impostors** move rigidly by the offset at h^2 = 0.45 (the crown's
    mass), main and shadow passes alike.
  - **Species bend**: archetypes.json `shape.bend` (presentation only),
    scaling lean and swing: birch 1.4, willow 1.3, alder 1.1, maple 1.0,
    beech 0.9, oak 0.85, pine 0.75, scrub 0.7, fir 0.6, spruce 0.55, snag
    0.25. In `SpeciesLook` for the GPU, `TreeLook` on the CPU.
  - Not done: the canopy map (1 m crowns for the sky shade) is static;
    leaf flutter is raster only (centimetres).
- **Not done**: no rain, cloud or fetch shelter behind islands.

The water asks the SHARED FIELD FUNCTION how deep it is, so the shoreline is
the real one at every LOD. That is M1 paying off somewhere other than terrain.

**Water is a HALF-SPACE at sea level: every bit of air below sea level is
water** -- the sea, a channel dug in from the coast, a pit dug inland, a
tunnel, a placed cave that descends below sea level (those flood and are
swimmable; worldgen is untouched). No connectivity, no air pockets, no
seepage, no fill, no water simulation of any kind, and no map of where water
is: the sheet lies at sea level everywhere and the DEPTH TEST decides --
terrain over sea level hides it, so any air under sea level shows water, at
any distance. This replaced ADR-006's per-pocket machinery (a connectivity
flood on the edit lattice, a timed fill, a surface mask, a roof test for
physics, a top-down capture of the bed, and the cuts between them), which
kept producing edge cases; `docs/adr-006-water-below-sea-level.md` is marked
superseded.

**The surface is two layers** (`water_wave.h`, `water.hlsl`):
- **The base, the same everywhere** -- the sea, pits, tunnels, channels:
  flat at sea level, plus the wind's ripples (the FFT tiles and the gusts'
  patches), which are normals in the pixel shader, not height.
- **The top, the deep sea's**: the trains and swell, weighted by the
  GENERATED depth (`real_terrain_height`, at the wave's rest point): none on
  land columns, rising over the shelf (`ww_depth_factor`), full in deep
  water, under the breaking cap (`WW_BREAK_HS` x depth). Storms scale this
  layer only, and the whitecaps belong to it (weighted by the generated
  depth's first metre, none in dug water).
- So inland pits and caves are calm by construction, a channel dug across a
  beach gets almost nothing (its generated depth is shallow), a dredged
  harbour stays calm, and storm surf does not funnel into dug channels --
  all accepted.
- **The swash** rides on the low beach only (`ww_swash_band`), and is
  suppressed per pixel where the pixel is DUG: the ground behind it (the
  depth copy) lies 0.3-0.6 m and more under the generated ground. A sheet
  pixel the swash or the beach's 12 cm lift raised over sea level is then
  shaded where its view ray meets sea level instead (`P` in `ps_main`), so a
  channel cut through a beach holds still water at sea level, not the
  beach's moving sheet hanging over it. Physics does the same by the feet's
  own depth under the generated ground.
- **No z-fighting on low beaches**: on the beach band the sheet rides 12 cm
  over the generated ground, and wherever it meets the ground the per-pixel
  edge has faded it into the scene, so which surface wins the depth test
  does not show. Raw frames (`--shake 30 --no-taa --sea-state 0`) at a
  grazing view of an untouched beach change only in the wind-blown grass.

**The sea is swimmable, and it is still not in residency.****The sea is swimmable, and it is still not in residency.** Physics has no
water body. The character asks `ww_sea_at` in `water_wave.h` each step -- the
same functions the shader draws, with the same level, sea state and clock:
sea level, plus the deep sea's layer at the generated depth (a trough never
under the generated bed), plus the swash on the low beach unless the feet are
0.3-0.6 m and more under the generated ground. The character is in the water
wherever its feet are below that surface. The eye uses the same call
(`Renderer::camera_underwater()`). `streamer.h` still contains no reference to
the sea. Keep it that way: `--band-report`'s "worst miss" compares the band
against `worldgen::terrain_height`, and a water surface feeding residency would
leave that check passing while it measured something else. Adding swimming
left the band report byte-identical.

`--swim [s]` walks off the gentle beach nearest SPAWN, swims out, turns and
comes back (from the origin until v19: v18's archipelago put the origin in
the sea, and every run found no beach and swam nothing). Its headline is **frames standing on the seabed in water too deep to
stand in**, printed beside the number of deep-water frames it is out of. It
also prints where the eye sat relative to the water when the shot was taken.

Three things that cost time here and will again:

- **`ww_surface` returns where the water that STARTED at (x, z) went to,**
  not the surface above (x, z). Gerstner waves move water sideways. The
  shader never needs the inverse and physics always does. Reading the surface
  at the swimmer's own point is 1.41 m wrong in a storm. `ww_rest_point`
  inverts the throw with Newton, and `--sea-report` prints the miss at every
  sea state (about 0.05 mm, float precision).
- **Buoyancy drag must act on velocity relative to the water.** Damping
  absolute velocity is a force holding the swimmer down while a crest rises
  past. In a storm it put the feet 1.9 m under against a balance of 1.35,
  with the eye under for 60% of frames. Damping relative to the water gives
  1.29 m and 11%.
- **The projected water grid assumed the camera was above every trough.** A
  swimmer's eye floating in a trough is above the water around it and below
  sea level. Every downward ray missed the plane, and the water nearest the
  swimmer was never drawn: 1.1% of the bottom quarter of the frame was water,
  and 99.6% after the fix. A camera more than 0.5 m above sea level gets the
  old plane (A/B: 1 pixel in 921,600 differed, by 2 levels).

**Under the water.** The eye is under when it is below the water's surface
over it (`cam_water`, `ww_sea_at` as physics asks it): in the sea, in a dug
pit, in any tunnel or cave below sea level. `Renderer::camera_underwater()` /
`camera_water_depth()`; every shot report has an `underwater:` line. Then:
- **The composite lays no air over a surface** (no haze, height fog, mist or
  cloud in front): the path is water.
- **`cs_underwater`** (post.hlsl), after the sea draws and before TAA, over
  every pixel by the depth the sea left: the view leg and the sunlight's leg
  down to what is seen dim per channel (the sea's own `extinction`, murkier
  with the roughness, as from above), and the water's body glows into the
  gap -- its in-scatter at depth h is the surface's (`deep_colour` x level
  water's light, the sun's part under the clouds' shadow) times
  exp(-ext h), integrated along the ray in closed form, so looking down is
  darker and bluer, looking up brighter, and a deeper eye sees a darker sea.
  0.053 ms at 1440p; the whole underwater frame 4.0 ms.
- **The surface from below** (water.hlsl, only while the eye is under, so
  no view from the air changes): Snell's window -- the sky the refracted ray
  meets (`sky_reflection`, the composited sky, clouds and all) and the sun
  spread by the ripples' unresolved slope as the glint is -- and outside it,
  total internal reflection of the water UNDER the surface: the seabed by
  SSR through the reflected ray's water, or, where SSR finds nothing
  (looking steeply up, the reflected rays leave the screen), the seabed's
  authored colour (`g_arch`'s last row set) lit as level ground. A flat deep
  colour there drew a navy ceiling over bright sand.
- No rain, splashes or lightning bolt are drawn while the eye is under.
- Stills: `--cam 575 -2.4 -288 -1.5708 0.25` (seed 0, 3 m of water west of
  spawn), `--sea-state 0.8` for a storm; `--dig-water`'s eye in an inland pit.
- **Not done**: the waterline across the lens -- the whole view switches at
  once as the eye crosses the surface, so a swimmer's eye in a crest flips
  it for a frame; caustics on the seabed; light shafts in the water; any
  particles; in a shallow storm the sheet, clamped just over the bed in a
  trough, shows from below as dark patches on the sand.

**Edges and foam, per pixel.** Everything the sheet needs to know about the
real (dug) ground comes from the ground point behind the pixel in the depth
copy (`gp` in `ps_main`):
- **The edge** fades the water into the scene over its last 8 cm of
  vertical depth (to the swash's surface on the beach), and **over the last
  1.2 m of the view ray under water where the water is dug**, so a dug wall
  shows through its own first centimetres of water: the waterline on a wall
  standing near vertical at sea level is the mesh's half-metre facets,
  toothed, and 0.4 m left them showing.
- **Contact foam**, one rule for every shore: where the water over the
  ground behind the pixel is THIN, vertically (10-30 cm), broken up by a
  drifting lace and never whole (0.7 of the lace's threshold: whole, a
  gentle beach's swash drew a solid white ribbon metres wide at a grazing
  view) -- at a beach's edge, round a trunk standing in a pit, at the foot of
  a dug wall. Faint scum on calm water; surf where the natural sea reaches
  (`sea_w`: the generated column is sea or its beach, and not dug) as the
  weather roughens, its band widening to ~0.8 m of water in a gale. Faded
  where the ray under water is long (2-6 m), which is an occluder rather
  than a shallow. It replaced a shore foam keyed to the vertices' generated
  depth, which could not see a dug edge. Whitecaps belong to the deep
  sea's layer (weighted by the generated depth's first metre, none in dug
  water): the ripples' folding is everywhere and laid gale whitecaps in pits.
- **Foam's fine structure: bubbles, by the pixel's footprint.** The lace
  has nothing under half a metre, and close to the water foam read as a
  smooth smear. `engine/render/foam_pattern.{h,cpp}` bakes a tileable
  512^2 RGBA8 texture with mips at startup (~20 ms; `startup.txt` prints
  its mean): 16 x 16 cells, one jittered point each, the centre of a hole
  of its own radius (0.45-0.9 of a cell) -- R the distance over radius,
  saturated (0 a hole's centre, 1 the film), GB its gradient, A whether a
  texel is a hole. The sea reads it at three octaves, 0.5 m, 12 cm and 3 cm
  cells (heap `kFoamSrv`, t7), each turned its own way and drifting a little
  faster than the one above. **Seamless by construction**: each octave is
  whole while its cell spans 8 pixels of water (`fp`) and gone at 4, where
  it is replaced by the pattern's MEAN (`foam_mean`, which the mip chain
  also filters towards), so coverage -- and the far look -- does not move;
  the threshold's softness goes from the lace's 0.25 to what antialiases
  the finest visible octave. Up close **density** (how far over its
  threshold) makes thin foam a translucent, greyer film and dense foam
  opaque white, and the foam is lit by its own **relief** (the holes'
  gradient; the finest octave's film rises to a honeycomb of ridges). The
  crests' foam is eroded by the same bubbles, cross-faded in with the
  coarse octave. Measured: the 12 m coast differs from before on 2% of
  pixels (the nearest foam); a dolly from 1.2 m to 18 m over one patch
  shows bubbles, then lace with holes, then the old lace, with no step.
  Two traps on the way: holes of different radii straddle cell borders, so
  the nearest point must be chosen by distance OVER radius (by distance, b
  jumped and shaded straight-edged facets); and the film ridges of the
  coarse octaves shaded as metre-wide facets, so only the finest has them.
  **Cost**: the same pattern evaluated per pixel (nine points an octave)
  cost +0.4 ms of the sea pass where foam filled the view -- most of it
  execution, ~0.15 ms the bigger shader's registers -- the texture
  +0.03-0.07 ms (sea pass at 1440p: open sea 1.022 -> 1.069 ms, the coast
  0.570 -> 0.597, close over the swash ~0.98 -> 1.05). The reads are gated
  to where foam can form.
- **Two fixes close to the water**: the grid's first 12 rows reach far
  below the screen (ndc -4) for an eye over the water: a vertex lifted over
  a beach (the swash and its 12 cm) moves UP the screen near a low camera,
  and at 1.2 m over the swash the grid's bottom row showed as a straight
  edge across the water. And **dug** compares the ground behind a pixel
  with the generated ground AT that ground point, extrapolated along the
  generated depth's own screen-space gradient: compared with the ground
  under the surface point, a seabed falling away offshore read as dug at a
  grazing view, and storm crests were flattened to sea level in a slab.
- **The terrain's shore wet band skips dug ground**: the swash's damp and
  film (mesh.hlsl, `l_swash`) go by height alone, so the mesher flags a
  vertex its edits put more than 0.3 m inside the generated rock (the lowest
  bit of the rock roughness byte, `o.dug`) and the band leaves it -- a pit
  dug to just over sea level wore surf on its floor.
- **`--dig-water`** (`build/dig_water.txt`) is the look and the check: from
  the coast nearest spawn, a pit dug into the beach, undercut on its inland
  side, opened to the sea by a channel (`dig_water.bmp`, `_top.bmp`,
  `_channel.bmp` along the channel from the sea, `_beach.bmp` a grazing view
  of the untouched beach beside it); 42 m inland a pit dug 2.5 m under sea
  level and walled off from the sea, and 54 m in one dug to 0.4 m over it
  (`dig_water_inland.bmp`, trees off, `_wet.bmp`, `_dry.bmp`). It asserts the
  inland pit and the channel are water to physics with a surface still at
  sea level, the higher pit dry, and an eye a metre under sea level in the
  inland pit drawn as underwater (`dig_water_under.bmp`).
  `BIOMES_PIT_DREDGE=1` also dredges a strip 18 m wide across the
  shoreline; `BIOMES_PIT_UNDERCUT=<m>` sets the undercut's centre under sea
  level (2.5, its roof tangent to the water); `BIOMES_PIT_PICK="px py ..."`
  names what the coast's still shows at a pixel, marched through the EDITED
  density (`--pick` reads the generated field). The instruments,
  `--water-debug` with `--tonemap 0`: `9` (red dug, green the natural sea's
  reach, blue the land side), `10` (red the water's vertical thickness to
  0.3 m, green the contact foam, blue the ray under water to 6 m), `13` (the
  edge's fade, the ray under water, dug).
- **Cost**, `--gpu-profile 120` at 2560 x 1440 (the sea pass now has its own
  mark, "sea"): open sea 1.034 -> 1.022 ms, the coast 0.581 -> 0.570,
  spawn 0.241 -> 0.245; the bed capture's 0.09 ms rebuilds are gone.
- **Not done**: a deep narrow pit's water reflects the sky where SSR finds no
  wall on screen, so it looks pale from above; light in a flooded tunnel is
  still the near field's air (no light through water); a dredged harbour or
  a beach channel never gets the sea's waves (by choice).

`--overhang` has no landmark to aim at: it searches for a real ceiling
(phase A), reports what it found and how the capsule left it, then digs a
cavity of its own (phase B), which is the decisive test. See the gotcha on
overhang collision above.

## Style

- Shaders: `snake_case`. C++: the existing style in `engine/`. TS: the eslint config.
- Prefer data in `archetypes.json` over constants in code.
- Commit in logical steps, with messages explaining why rather than what.
