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
| `engine/foliage/` | Ground cover -- grass, flowers, bushes -- generated on the GPU each frame (`foliage.hlsl`). |
| `engine/rocks/` | Layer 9's rocks: procedural meshes, the CPU tile streamer, instanced drawing on the terrain's root signature, static colliders. |
| `tools/validate/` | Headless N-seed harness. CI entry point. |
| `tools/view/` | Native world viewer (Win32). Debug tool, Windows only. |
| `tools/shared/` | Presentation, shared by the harness and the viewer. |
| `assets/textures/` | `scans.json`, the CC0 surface scans' manifest. Downloads and packs are fetched, never committed. |
| `tools/textures/` | `fetch_scans.py`: downloads the scans and packs them for the engine. |
| `engine/` | Track B runtime (D3D12, `docs/adr-003-*`). Since M1 it evaluates the REAL field: `engine/field/real_field.hlsl` is the caller's side of ADR-001's line (which cells, rivers and shafts are near a point), and `world_gpu.{h,cpp}` uploads them. |

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

```bash
build\engine.exe
```

`build.bat` is a thin wrapper over cmake and builds everything; there is no
`CMakePresets.json`, and the `cmake --preset dev` line that used to be here had
never worked. To drive cmake directly:

```bash
cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake
```

`validate.exe`, `view.exe` and `engine.exe` all read data by paths relative to
the repository root, so run them from there. `engine.exe --shot <path>` renders
one frame at a fixed camera and writes a BMP without opening a window.
Every mode renders at 1280 x 720 unless given `--res W H` (borderless, so
2560 x 1440 fits a 1440p screen exactly); measure costs at the resolution
that will be played -- per-pixel passes scale 4x from 720p to 1440p.

The engine's headless modes all write a text file under `build/` and exit:
`--shot`, `--flight <s>`, `--walk <s>`, `--overhang`, `--dig`, `--dig-save`,
`--dig-verify`, `--dig-stress`, `--check-dialect`, `--probe-field`,
`--band-report`, `--sea-report`, `--swim`, `--blend-report`, `--tab-test`,
`--sky-report` (CPU only, no device: the atmosphere against clear-sky
references), `--rt-report` (ADR-004 L5: the ray-tracing scene at three
views, and L6's sky visibility with and without crowns), `--leak-test`
(ADR-004 L6: a sealed chamber, unlit and torch-lit, DDGI against the
fallback), `--gi-report` (DDGI at three views), `--lights-report` (ADR-004 L3: local lights at night, six
configurations timed) and `--fell` (with `--fell-save` / `--fell-verify`).
Every mode takes `--time <hours>` (ADR-004 L2: 14 by default, which is the
pre-L2 sun), the weather's `--sea-state <0..1>`, `--wind <m/s>`,
`--wind-dir <deg>` and `--swell <m>` (automatic weather otherwise), `--torch` (carry one), `--no-light-shadows`, `--no-fog` and
`--fog-density <k>` (a multiplier on the authored mist), `--no-far-shadow`, `--no-pom`, `--no-rocks`,
`--no-forest-shell`, `--no-clouds`, `--no-rain`, `--rain <0..1>`, `--wet <0..1>`,
`--no-lightning`; `--shot` also
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
`--check-rt` (ADR-004 L0: DXR end to end on one triangle),
`--tree-lod0 <m>`, `--no-trees`, `--no-shadows`, `--no-foliage`,
`--foliage-debug 1`, and the post chain's `--no-taa`, `--no-ao`,
`--no-contact`, `--no-post` and `--post-debug 1|2|4|5` (AO alone, contact
shadows alone, how each puddle pixel found its reflection, the clouds' shadow on the key light), and exposure's `--ev-comp <stops>`, `--ev100 <v>` (fixed,
no meter), `--tonemap 0|1|2` (raw pre-L1 path / AgX / fitted ACES),
`--agx-sat` and `--agx-power`. `--sea-shot` only PLACES the camera: pair it with `--shot`,
or it falls through to the window loop with no window and never exits. `--walk` takes `--uncapped` to time frames at ground
level, where the grass is. It is a GUI subsystem binary, so a shell does not wait
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
- **Never read back from an upload heap.** It is write-combined, uncached
  memory. The impostor mip chain averaged each level by reading the one
  above out of the upload buffer it had just written: a 20 s startup for
  work that takes 180 ms in ordinary memory.
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
  so a biome looks the colour it was tuned to and the GI, the RT scene and
  the heightfield's top-down pass -- which still read the per-vertex
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
- **Not done**: anti-tiling beyond what the mips and the retint give (no
  repetition shows from 160 m up on the badlands), BC compression (4x less
  memory), scans for the grass, leaves and placed rocks, silhouettes.

**Parallax occlusion** (`engine/render/shaders/pom.hlsli`; `P`, `--no-pom`)
on the scans' height channel, terrain and bark. No silhouettes and no depth
write: outlines stay the mesh's, so nothing downstream (TAA, AO, the crack
count, physics) sees it. `depth_m` per set in `scans.json` says how many
metres the height map's range stands for: ground 3-5 cm, rock 15-18 cm,
bark 1.5-3 cm.

- **Terrain**: the heaviest of the four candidates, in the triplanar plane
  the normal faces most; its shift moves every candidate's sample point.
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
the near level's do. Cost: +0.25 ms walking in a willow wood; the RT
cutouts 1.87 -> 2.07 M triangles, 292 -> 323 MB. Sky through the
crowns' upper third of a woodland still: 10.7% before, 10.6% after. Lit pass
only -- the shadow casters and `leaf_cutout.cpp`'s port keep
`leaf_coverage`. Walking cost within noise. The level cross-fades and far
fade still dither on the screen (walking per frame), and the grass's clump
cards fade against the blades the same way.

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
  does; the RT scene and DDGI do not see the shell (it starts where they
  stop mattering); no altitude gate was needed.

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
  authored rock colour exactly. **The RT scene** has a BLAS per model from
  its far level, appended to the tree buffers with its info at
  `kRockInfoBase` (1024) + model, and an instance per rock within 300 m in
  the trees' mask: InstanceID `0x200000 | rgb565(sqrt(albedo)) << 4 |
  model`, so `gi_trace` decodes the biome's rock colour and the face normal
  with no new binding. RT shadows and DDGI see rocks; `--rt-report` counts
  them. **The far shadow does not**: it applies past 1.36 km at 8 m texels,
  and rocks stream to ~760 m.
- **Not done**: grass grows through a rock's footprint -- the bake would
  need the rocks bound and its cached tiles re-baked when rocks stream in
  after them, so it waits; the LOD switch pops (no cross-fade, by choice
  for now). Shots do not run physics: the collision probe is in `--walk`.
- **A pre-existing cascade artifact** turned up beside a badlands boulder:
  blocky texel-sized squares of shadow on gently sloped sand at 16:00
  (`--cam 2452 123 3366 0.99 -0.2 --time 16`), there with rocks and
  parallax off and gone under `--rt-shadows` -- shadow acne in a middle
  cascade, not investigated.

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
against the blades; leaf-card bushes; flowers on a few blade tips. A 6-8 m
tuft noise varies height and density inside a patch. Same sun, shadows and
fog as everything else. `G` toggles it. Cost: +0.3 ms a frame walking
through a meadow (`--walk 15 --uncapped`, with and without `--no-foliage`);
about 24k near blades, 13k far, 13k cards in a woodland-edge still.

The bake kernel is the third time the inlining lesson has bitten: written
longhand -- five heights and three density probes -- it exceeded FXC's 4096
temp registers (X4505). One call site per field function, each in a
`[loop]`, the probes as one loop with a little state machine.

**Tufts.** A placed blade draws THREE (`tuft`, `kTuftMax` in `foliage.hlsl`;
the index ranges hold three strips each): the placed one and two more 2.5-7
cm off its root, each with its own angle, 65-105% of its height, leaning
out -- one placement reads as a clump. Blades are also 1.4x as wide. The
spawn meadow went from stems over pale ground to a sward. `--grass-tuft
1|2|3`, `--grass-width <k>`, `--grass-density <blades/m^2>` (96) for A/B.
Cost walking at 2560 x 1440: all ground cover ~1.0 ms (5.19 with
`--no-foliage`, 6.23 before); tufts of 3 +0.26, with the width +0.32,
tufts of 2 with the width +0.18. Tinting the terrain under grass toward
the sward (a 256 m grass map from the cells, read by the terrain shader)
was built and judged not to look right; left for a later quality pass.

Not handled: grass does not know about dig edits, and floats over a dug hole.

### The frame: HDR targets and the post chain

Scene shaders write TWO R16G16B16A16F targets through `scene_out.hlsli`: the
sun's direct light, and everything else (sky, bounce, fog). Then
`post.hlsl`: horizon-based AO at half resolution from depth; a composite of
direct x screen-space contact shadow (a 0.5 m march towards the sun through
depth) + ambient x AO; TAA (Halton 2,3 jitter over 8 frames, reprojection by
depth with the unjittered previous matrix, YCoCg variance clipping, 1:9
blend); the exposure meter; then exposure, a light contrast-adaptive
sharpen and AgX into the back buffer. Y / O / J toggle TAA, AO and contact
shadows. A still renders 16 frames so TAA converges. Cost: ~0.3 ms a frame
at ground level, 0.09 of it the effects.

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
no-RT fallback: sky irradiance from above, the ground's bounce from below.
Terrain, trees, grass and the sea all light through it. Trees and grass
keep a little extra fill as a STAND-IN for inter-leaf bounce; DDGI (L6)
should remove those multipliers.

**Auto-exposure** meters a 256-bin log2 histogram of what TAA produced and
puts the mean of its 10-90% band at middle grey 0.18, adapting like an eye
(faster into light than out). A still adapts instantly. Every `--shot`
writes the EV100, the band and the histogram to `<shot>.txt`: spawn EV100
13.7, the badlands hillside 14.9, a sea view 15.4. `,` `.` step the
compensation; `U` cycles AgX / ACES / raw.

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
  underwater the rain still draws; no rain in the RT or GI passes (nothing needs it);
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
  to a dim blue day; the floor is now **EV100 -1** (`cs_exposure`'s clamp),
  so moonlight renders a stop under middle grey -- ambience -- and a torch
  or a fire, far above the scene's mean, carries the picture. EV100 0 was
  tried: two stops read as near black. No stars, no moon phases, no
  Purkinje shift yet.
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
- **Shadows: a 4096^2 atlas of 512^2 tiles**, a cube of six for a point
  light. The eight most important shadowed lights hold slots and keep
  them while they stay in the eight, so their faces stay cached; at most
  twelve faces are redrawn a frame -- never drawn or moved first, then a
  static light's faces every 240 frames so a dug hole or a felled tree
  reaches its shadow. A face draws only the chunks within the light's range
  (`Streamer::slots_near`, one ExecuteIndirect a slot) and the trees through
  their own cull (`TreeRenderer::record_light_shadow`, the first cascade's
  floor: real leaves).
- **`--lights-report` is the instrument** -- at night, at ground level,
  frame mean over 400 frames: none 2.33 ms; torch held still 2.40; **torch
  carried at walking pace 2.51** (all six faces redrawn every frame, 72 chunk
  draws); 64 campfires 2.98 unshadowed, 3.55 with eight shadowed; 255
  campfires 4.42, 46.5 lights per lit cluster, none overflowed. Two things
  it caught in itself: a frozen clock cached the torch's shadow and called
  it free, and 255 campfires in the 64-campfire disc overflowed every
  cluster; the report now runs a clock and keeps density constant.
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

### The ray-tracing scene (ADR-004 L5)

DXR 1.1 inline RayQuery. **Denoised RT key-light shadows are the DEFAULT
wherever DXR 1.1 is** (`Renderer::init` turns them on after `create_rt`);
without it the cascades and PCSS, as before. `--no-rt-shadows` (`R` in the
window) for the cascades -- it also stops building the RT scene when
neither `--rt-sky` nor DDGI wants it; `--rt-shadows` still parses and
changes nothing. Every headless mode now runs with RT shadows unless told
otherwise, so a figure measured before this is a cascades figure;
`--rt-report` measures. `engine/render/rt_scene.{h,cpp}` keeps a BLAS per
drawn chunk at LOD 0-3 (+-512 m, the cascades' reach) -- pointing at the
whole vertex pool and the slot's own index range, because the pool's
indices are absolute -- and one per tree model from its LOD 1 mesh (bark
opaque, leaf cards non-opaque); the TLAS is rebuilt every frame from the
drawn chunks and the standing trees in range. `gi_trace.hlsli` is the
ADR's tracer interface: `gi_occluded` and `gi_trace` (hit, normal,
albedo), SM 6.5, compiled with DXC. `rt_shadow.hlsl` is the pass.

- **DXR 1.1 has no indirect build**, so a chunk's index count must reach the
  CPU before its BLAS: read back three of RtScene's OWN ticks after it is
  meshed (a still's warm-up records frames without advancing the renderer's
  frame index), at most 32 builds a frame. **Three, not two**: a chunk is
  noted before the tick its frame takes, and "two on" read the slot's old
  zero before that frame's counter copy landed -- 834 of 858 chunks at spawn
  were judged empty for good.
- **Chunks are queued by looking, not by being told.** Headless modes fill
  residency in a loop that meshes without recording a scene, so an event
  queue never heard of those chunks. The TLAS pass walks every drawn slot
  and queues any whose BLAS (or empty verdict) is not for its current key.
- **A GPU hang reads back as zeros everywhere.** The first run's counts,
  stats and timers were all 0 and looked like a queue bug;
  `GetDeviceRemovedReason` said `DXGI_ERROR_DEVICE_HUNG`. The cause was the
  tree builds' scratch going into the deferred-release list stamped 0 for
  "not seen yet" -- which the release loop, running first, read as ancient,
  freeing it while the build ran. Bisected with `BIOMES_RT_DBG` (1 no
  trace, 2 no terrain BLAS, 4 no trees in the TLAS, 8 no scene at all, 16
  no tree BLAS). `--rt-report` now says whether the device is alive.
- **Leaf cards are alpha-tested in the ray query** with the raster's own
  `leaf_coverage` at the card's uv (per-vertex uv, kind and seed uploaded
  beside the positions). A flat 55% transmission per card was tried first:
  a dawn sun crosses so many cards that the wood's floor lost every trunk
  shadow under a uniform dimness.
- **The key-light shadow ray meets leaves as opaque CUTOUTS**, not cards
  (`engine/trees/leaf_cutout.{h,cpp}`; `--rt-card-leaves` for A/B). At
  startup each model's LOD 1 cards are cut along that same pattern (blur
  0.1, kept at 0.5) into triangles: a broad leaf is a pentagon found by
  bisecting its own coverage along five rays from its centre, grown by
  sqrt(2 pi / (5 sin 72 deg)) to the leaf's area; needles are marching
  squares in the sheared frame (|u - 0.5|, 9v + 6|u - 0.5|) where every
  stroke is an axis-aligned band. One more BLAS per model holds them AND
  the bark, instanced beside each tree under mask 0x8, which rocks carry
  too; the shadow ray traces terrain | 0x8 only. Every other ray keeps the
  cards. 1.87 M triangles, 292 MB of BLAS, cut in ~180 ms on every core.
  At 2560 x 1440 (trace + denoise): the 17:30 wood 6.09 -> 4.87 ms, the
  wood from 45 m 4.96 -> 4.03, the close floor 4.21 -> 3.48; BLAS + TLAS
  +0.06 ms. The floor at EV100 13 46.7 against the cards' 46.4, and
  agreement with the cascades within a point on every `--rt-report` view.
  - **This GPU (RX 7900 XT, RDNA3) traverses in shader code**, so the
    usual "skip the any-hit calls, opaque is 2-4x faster" does not hold:
    a non-opaque candidate in an inline RayQuery is a branch in the same
    loop. `BIOMES_RT_LEAVES` split the old cost in the wood: every card
    opaque 2.53 ms, cards culled 3.22, the alpha test with a trivial
    pattern 4.87, the real one 5.75 -- the pattern itself 0.4-1.3 ms of it.
    Dense geometry costs real traversal: the first cut (8 rays, a fine
    needle grid) was 4.0 M triangles, 623 MB, and traced SLOWER than
    the coarse one; a coarser still let rays run further and was slower
    again.
  - **The first build saved a tenth, not a fifth**: the cutouts sat
    beside the cards while the shadow ray still entered each tree's own
    BLAS for its bark, walking every card's BVH nodes to cull them. Bark
    went into the cutouts' BLAS and the ray stopped seeing mask 0x2.
  - `BIOMES_RT_LEAVES` overrides the leaf path: 1 every card opaque, 2
    cards culled, 3 the alpha test with a trivial pattern, 4 the cards.
  - `leaf_coverage_cpu` is a hand port of `leaf.hlsli`'s `leaf_coverage`:
    change one, change both, or the shadows are cut from old leaves.
- **Terrain shadow rays start from Hanika's lifted point** ("Hacking the
  Shadow Terminator"): the pixel's own triangle comes from a terrain-only
  primary ray, and the start is lifted onto the curve its vertex normals
  imply. Smooth normals over facets up to 8 m wide streaked every grazing
  slope with self-shadow before it.
- **Penumbra**: each shadow ray aims at a different point on the sun's (or
  moon's) disk per pixel and frame, and TAA integrates the true penumbra.
- **`--rt-report`** at three views: primary rays meet the drawn terrain
  within 1% of the depth buffer on 93% of the badlands hillside (the rest
  is trees, water and LOD 4+), nearer only 0.5%; `gi_trace`'s normals within
  2.5-3.2 degrees of the depth buffer's near the camera; shadow agreement
  with the cascades 94% on the hillside, 76-79% in woodland, where the
  traced canopy is the leaf pattern and the cascades' is coarser.
- **Cost**: BLAS + TLAS 0.18-0.23 ms steady; the shadow ray 0.29 ms on bare
  ground to 1.4 ms through a forest (alpha-tested leaves). The cascades still
  render under `--rt-shadows` -- the mist reads them -- so this is an A/B,
  not a replacement yet. Memory: 94-270 MB of terrain BLAS, two 94 MB TLAS
  sized for 67k instances, 103 MB scratch.
- **The shadow is denoised** (`engine/render/rt_denoise.{h,cpp}`,
  `shaders/rt_denoise.hlsl`; on with `--rt-shadows`, `--no-rt-denoise` /
  `F8` for A/B). One ray a pixel a frame is grain that TAA cannot remove --
  it clips history to each pixel's 3x3 neighbourhood, and a 1-sample
  shadow's neighbourhood holds 0 and 1 -- so the shadow is filtered on its
  own between the trace and the composite, SVGF's shape for a scalar: a
  temporal pass through TAA's own clip-to-last-frame matrix (this frame's
  jitter off, last frame's on, so the history stays on last frame's grid),
  a long mean and second moment up to 24 frames, a fast 3-frame mean that
  cuts the long one short where they part (anti-lag), and disocclusion by
  the distance stored with the history; then four edge-aware a-trous
  passes (steps 1, 2, 4, 8) written back into the visibility's x. Heap
  [72..78], UAVs only, ~35 MB at 1280 x 720, ~140 MB at 2560 x 1440.
  - **Full resolution is the default; two cheaper traces were tried.**
    *Half resolution for everything* (one ray per 2x2 quad, rotating,
    denoised on the quads, upsampled by distance) cut the RT pass ~4x at
    2560 x 1440 (the 17:30 wood 5.7 -> 1.6 ms) and looked right in stills,
    but BOILED on trees in motion, Lumen-like: a quad's history mixes four
    pixels' leaves and a swaying crown keeps resetting it. Removed.
    *Sparse terrain* (`--rt-sparse-terrain`, Shift+F8) keeps trees at
    every pixel and traces terrain -- what the trace's terrain-only primary
    ray says is terrain -- at one pixel of each quad a frame (`kRtQuad`,
    the same table in `rt_shadow.hlsl` and `rt_denoise.hlsl`); a skipped
    pixel carries its history and one with none is filled by the a-trous.
    It must be TWO dispatches -- the quads' pixels, then the rest, which
    leave after the terrain test -- because skipping three pixels in four
    inside one dispatch saved a fifth, not three quarters: a wave waits for
    its slowest lane. Two passes: close floor 4.0 -> 2.5 ms, the wood 5.9
    -> 5.7, the forest from the air 6.1 -> 6.6, and walking 7.85 ms
    against full resolution's 7.63 -- grass is not terrain to the trace,
    and it covers the ground. Off by default.
  - **Under RT shadows the near two cascades are not drawn**
    (`first_cascade`, `l_shadow_frame.z`): no surface reads them, and the
    mist's one tap starts at the third, fitted round the camera at 0.19 m
    texels. They are marked invalid and redraw the frame RT goes off; the
    RT instruments keep all four. Walking at 1440p, full-resolution RT
    7.95-8.15 -> 7.60-7.63 ms.
  - Edge-stopping is SVGF's, made gentler on converged pixels: 1.5 sigma
    of the MEAN's variance (the sample's over n), a depth tolerance of 5%
    of the local plane's prediction, and a pass runs only while its step
    is within 8 / sqrt(frames) pixels (ReBLUR's shrinking reach). The
    history test is 3% + 0.3 m, loose enough for swaying crowns. These
    were chasing crowns that looked soft beside raw RT; together they
    moved the image about 1 luma, and the softness turned out to be the
    point: raw RT under TAA keeps 1-sample sparkle (TAA's 3x3 clip lets
    it through), which reads as leaf detail. A wrong lead worth knowing:
    `--no-taa` stills render ONE frame, so they show the denoiser with no
    history at all.
  - **The disk samples are an R2 sequence in time, offset per pixel by a
    hash**, not interleaved gradient noise: IGN's diagonal structure
    survived accumulation as hatching the filter could not tell from
    shadow. Raw (`--no-rt-denoise`) is now white grain under TAA.
  - Instruments: `BIOMES_DEN_DBG=1` the temporal pass alone, `=2` each
    pixel's history length in place of the shadow (white is 24 frames);
    `BIOMES_DEN_NOHIST=1` no history ever -- a just-disoccluded pixel, the
    spatial filter alone, which is what motion shows at edges.
  - Measured: still-frame flicker on the close floor view (`--cam -30 0 -60
    3.14 -1.0 --ground --time 16 --no-foliage --shake 60`): cascades
    1.77-3.50, RT raw 2.3-5.6, RT denoised 0.76-0.95; the 17:30 wood with
    grass, cascades 2.12-2.90, raw 3.86-4.86, denoised 1.58-2.38.
    Walking at **2560 x 1440** (`--walk 15 --uncapped --res 2560 1440`):
    no shadows 4.49, PCSS 6.20-6.25, RT denoised 7.60-7.63 with the near
    cascades skipped. The RT pass in stills: close floor 4.0 ms, the 17:30
    wood 5.9, the forest from the air 6.1 -- the denoiser ~0.9 of each,
    the rest the trace, most of it alpha-tested leaf cards.
  - **RT woods are darker than the cascades' and it is the trace, not the
    denoiser**: raw and denoised agree from the air; the RT scene shades
    crowns with every tree's LOD 1 cards (see the 76-79% agreement).

### Canopy shade: crown volumes and a traced sky (ADR-004 L6, step 1)

`--rt-sky` (Shift+R) scales the AMBIENT target by a traced sky visibility:
one cosine-weighted ray a pixel a frame through `gi_transmittance`
(`gi_trace.hlsli`), which is 0 at any opaque hit and Beer-Lambert through
the CROWN VOLUMES -- one procedural unit-sphere BLAS, instanced per standing
tree as an ellipsoid round its crown (mask 0x4, InstanceID `0x400000 |
model`), with the species' `leaf_density` from archetypes.json as the leaf
area density (extinction 0.5 x LAD). `--no-crowns` keeps terrain and trunks
only, for A/B; `--rt-sky-rays <n>` traces more.

- **Crowns are for diffuse rays only.** The key-light shadow (`gi_occluded`,
  mask 0x3) never sees them, so the alpha-tested cards keep every sunfleck.
- **A ray skips the crown it starts inside** unless it starts on terrain:
  charging a leaf for the whole crown behind it drew every crown dark from
  outside. A ray heading down stands for the ground's bounce and does not
  see terrain, or every trunk in an open field would lose half its light.
- `--rt-report` prints the sky visibility with and without crowns and the
  cost: wood 0.90 -> 0.50, spawn 0.93 -> 0.59, +1.5 ms in a dense wood at
  full resolution. The ambient target also holds LOCAL light, which this
  dims too until DDGI separates them.

### Canopy shade without RT: the canopy map (ADR-004 L6, step 2)

On by default (`--no-canopy`, `B` for A/B); the traced sky replaces it
under `--rt-sky`. `engine/render/canopy.{h,cpp}` splats every standing
tree's crown ellipsoid (`crown_volume` in `tree_mesh.h`, the same volumes
the RT scene traces) into a 512 m camera-centred map of 1 m texels on the
GPU -- vertical optical depth, tau-weighted crown bottom and top, vertical
transmittance -- then blurs it a-trous into five wider slices
(`canopy.hlsl`). `canopy.hlsli`, included by `shadow.hlsli`, reads it in
`gi_sky_visibility`, so it darkens the SKY term of `gi_diffuse` and
`gi_specular` only: bounce and local lights are untouched. The map is at
heap [38] (t25), a THIRD range of every lit pipeline's shadow table; its
corner and reference height ride in `l_canopy` (ShadowConstants, 432 bytes).

- **It is judged against the traced crowns.** `--rt-report` traces 16
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

### DDGI probes (ADR-004 L6, step 3)

Ray-traced probe GI, **OFF by default** since its +2.8 ms walking was judged
too dear (`--gi`, `I`; `--gi-report` and `--leak-test` switch it on
themselves). Off, `gi_diffuse` is the heightfield GI (below) with the
canopy map. It stays as the reference anything cheaper is measured
against.
`engine/render/ddgi.{h,cpp}`, `shaders/ddgi_update.hlsl` (the update),
`ddgi.hlsli` (the reader), `ddgi_common.hlsli` (the layout both share).
Three camera-centred cascades of 24 x 12 x 24 probes, 2, 8 and 32 m apart,
addressed by world grid coordinate and stored at it mod the dims, so they
scroll; a slot whose stored coordinate is not the one it should hold is
"not yet traced" and is never sampled. 128 rays a probe, a quarter of the
probes a frame, hysteresis 0.92; 8x8 irradiance and 16x16 depth-moment
tiles, gutters written by the blend from group memory.

- **Probe rays** go through `gi_trace_probe`: the nearest OPAQUE hit
  (terrain, bark; cards culled) and the crowns' optical depth before it.
  A miss is the sky (below the horizon, the lit ground); a hit is lit by
  the key light (a `gi_transmittance` ray), the first 8 local lights through
  their shadow atlas, and last frame's probes (every further bounce). A
  terrain face seen from behind is rock: distance negative, no radiance.
- **Probes in rock** (more than a quarter of rays backfacing) step through
  the nearest wall or switch OFF and trace 32 rays; probes with nothing in
  reach go IDLE (32 rays). 22-57% of a cascade is rock, and at spawn from
  46 m up all but 53 of cascade 0 are idle.
- **The leak is decided by what an off probe means.** First version: a
  point whose cascade-0 neighbours were all off fell through to cascade 1,
  whose nearest probes stand outside in daylight; the Chebyshev floor (0.05)
  and the weights' normalisation handed their light back -- 210 nits in a
  sealed chamber. Now only NOT-YET-TRACED or idle neighbours justify asking
  a coarser cascade; all-off is "walled in", irradiance 0.
- **The air leaked too.** The atmosphere's aerial perspective and the
  mist's sky term assume sunlit, sky-lit air: 3.4 and 10 nits in the
  chamber with the walls black. `cs_ddgi_camera` measures the probes' share
  of an open sky's light at the eye (six directions), the CPU reads it back
  two frames late (`Ddgi::camera_sky`), and post and fog scale their
  in-scattering by it over the first 64 m, back to full by 256 m. A fade
  starting at the eye left 6% of the haze at 4 m: 0.2 nits, the residue.
- **`--leak-test`**: a 4 m sphere dug 10 m under flat ground; it settles
  1200 frames inside, then measures each config, outside LAST. Sealed and
  unlit 0.001 nits (the histogram's floor) with or without mist, the
  chamber's 19 cascade-0 probes all ACTIVE, against 566 on the fallback and
  2,985 outside; a carried torch's bounce adds 1.08x its direct light,
  which in a closed chamber means walls of albedo 0.07 -- the meadow
  material it was dug from. Luminance is read from the meter's histogram
  (`band_nits`): the meter's own target stops at EV100 -2. It prints, per
  config, what the probes say at the eye and the chamber's probe states.
- **Its first pass was an accident, twice over.** (1) `field_normal`
  ignored the player's edits, so every DUG surface had the undug rock's
  normal -- "up" ten metres down -- and the chamber's ceiling and walls
  faced into rock: every probe inside read "in rock", switched off, and
  the walled-in rule returned black, which is what a sealed chamber should
  look like. The raster has shaded dug surfaces with those normals since
  edits existed. A cell that touches an edit now takes its gradient from
  its own eight (edited) corners. (2) A still's residency loop meshed
  without telling the RT scene, whose drawn-slot scan only catches changed
  KEYS; a dig re-meshes the same key, so the BLAS kept the undug rock.
  `capture()` now calls `note_meshed`. The probe states line is what
  showed both: "19 off in rock" in a hole dug for the purpose.
- **Re-entry transient:** stepping out of the chamber and back scrolls its
  probes out of cascade 0 and swaps the LODs round it; the fresh probes
  catch light through the half-rebuilt scene and it decays at 0.92 an
  update -- 10 lux at the eye after 240 frames, under 0.01 after ~1000.
  Not fixed.
- **Sky rays were thrown away for the first version.** A miss stored
  distance 1e27 and an untraced ray 2e27, and the readers skip anything at
  or past HALF of the latter -- exactly a miss. The probes saw only the
  ground and its bounce: facing up in open air a third of the sky's light
  and a twentieth of its blue, and shaded birch trunks read olive (RGB 36,
  48, 8). `kDdgiUnused` is 1e30 now; the trunk is (35, 53, 29) and the
  probes' sky colour matches the fallback's facing up (B/G 1.54 v 1.50).
  `--gi-report` prints the probes' six-way irradiance at the eye beside
  the fallback's, which is how it was found.
- **`--gi-report`**: luminance with DDGI against the fallback (bare
  hillside 1.03, wood 0.72, spawn 0.87), update ms, the eye's six-way
  irradiance against the fallback's, per-cascade active / off in rock /
  idle / not yet traced.
- **The bounce is warm in the badlands and it is physical**: shaded sand
  takes ~14% of sunlit sand's red (linear), where half a hemisphere of
  sunlit sand at that albedo predicts ~18%; the probe rays decode albedo as
  the raster does. The fallback's grey ground bounce never had it.
- **Trees and grass read the probes per VERTEX** (`g_gi_vertex` in
  `lighting.hlsli`, set from an interpolant); `gi_diffuse` also leaves
  `g_gi_all` for `gi_specular`, so a probe-lit cave reflects no sky.
- **The 1.3x fill on trees and grass is gone where the probes answer**
  (`gi_fill`): it stood in for bounce between leaves and blades and off the
  floor, which the probes compute. The fallback keeps it.
- **Cost, walking** (`--walk 15 --uncapped`): 2.46 -> 5.24 ms. Split with
  `BIOMES_GI_DBG` (1 samples stale probes and skips the update, 2 updates
  and never samples): the update ~1.1 ms, sampling ~1.0 ms (terrain per
  pixel -- removing trees or grass does not change the delta), the RT scene
  now built every frame ~0.65 ms. Not optimised yet.
- **Not done:** the mist's sky term per froxel rather than at the camera;
  logs and stumps are not in the TLAS; the re-entry transient.

### Heightfield GI (ADR-004 L6, step 5) -- the default diffuse GI

On by default (`--no-hfgi`, `Z`); DDGI replaces it where on. The world is
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
- **The air**: `cs_hf_camera` is the heightfield's `cs_ddgi_camera` -- the
  map's share of an open sky at the eye, read back, scaling the haze and
  the mist near the camera. Without it the sealed chamber's walls were
  black and its air 13.8 nits.
- **Measured**: the badlands' shade (63, 37, 25) -> (70, 31, 19), DDGI
  (81, 30, 16); the wood's shaded floor 58.1 -> 55.1 (DDGI 44.8), the birch
  trunk (83, 93, 69) -> (57, 75, 54) (DDGI 35, 53, 29). `--leak-test`'s
  no-RT config: sealed and unlit **0.001 nits** (the histogram's floor),
  where the bare fallback leaks 566. **Cost +0.05 ms walking** (2.36 ->
  2.41), against DDGI's +2.8.
- **Not done**: crowns as bouncers (the canopy map dims the sky, nothing
  sends the leaves' green), local lights in the bounce, anything past
  256 m, and true caves seen from inside a big cavern (it sees only the
  nearest opening's light).
- Heap [48..52] the scene's SRVs (t38..t42, a sixth range of the lit
  tables), [53..55] the update's inputs, [56..61] its UAVs; the heap is 64.

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

(Without DXR, or with `--no-rt-shadows`. Under RT shadows -- the default
-- only the far two cascades are drawn, for the mist.)

Sun shadows, **four cascades out to 28, 100, 380 m and 1.6 km**, cast by
the terrain and the trees and received by both through one function,
`shadow_factor` in `engine/render/shaders/shadow.hlsli`. `L` in the window,
`--no-shadows` for A/B.

- **Fitting.** The near two are fitted to a sphere round their slice of the
  view frustum (a radius that depends only on the splits and the lens, so
  it never changes size); the far two to a sphere round the CAMERA, so a
  turn between their redraws cannot leave the view outside them. Centres
  snapped to whole texels, so edges do not crawl.
- **Resolution per cascade, in one 4096^2 x 4 array**: each draws into a
  corner of its slice at its own size (`kCascadeRes` = 2048, 2048, 4096,
  4096; `shadow_scale` maps uv). The near two at 2048 are already 1.8 and
  4.9 cm a texel, about the sun's own penumbra; the far two at 2048 were
  38 cm and 1.6 m. 4096 everywhere cost +0.65 ms walking, nearly all of it
  the near cascades' leaf cards.
- **Time-sliced** (`cascades_due`): cascade 0 every frame, 1 every second,
  2 and 3 every fourth, interleaved -- two drawn a frame. Each keeps the
  matrix its map was drawn with. Four cascades every frame at 4096 was +1.4
  ms walking; sliced at 4096, +0.7; with the per-cascade sizes, **+0.1**
  (2.41 -> 2.51 ms) for four times the reach.
- **Filtering (PCSS)**: a 12-tap blocker search, the penumbra from the
  NEAREST blocker times the sun's angular diameter (0.53 degrees), never
  under 1.5 texels, then a 16-tap Poisson PCF. Pixels the search finds
  wholly lit or wholly shadowed stop there. The rotation is per shadow
  texel AND walks per frame (`l_shadow_frame`): fixed, it left a speckle of
  dark dots in every penumbra that TAA could not average. The average
  blocker (textbook PCSS) blurred a trunk's shadow at its foot with the
  crown 15 m up; the nearest one keeps contact hard. Sunflecks now read as
  the soft discs the sun makes, not the pixel-sharp mottling 3x3 PCF drew.
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

## Testing

`tools/validate` runs 200 seeds and exits non-zero on any failure. It asserts
presence, quota, radial ordering, land fraction, adjacency, edge taper, home
landmass, boat connectivity, spawn, slope budget, river sanity, LOD agreement, blend seams,
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
percent). It is now a **35% mean** — below the percolation threshold, so a
typical seed is an archipelago of about 7 landmasses with the spawn mass
holding roughly three quarters of the land, and the worst seeds far less.
The offset behind it is solved GLOBALLY for the mean and baked as a literal:
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

**Caves are OFF since v11.** Every `cave_density` in `archetypes.json` is 0,
so layer 6 carves nothing, layer 7 builds no shafts (`worldgen::caves_enabled`
gates it: a shaft must end in cave air) and `column_band` drops its 64 m cave
padding (1129 → 736 chunks wanted). The `cave entrances` check asserts ZERO
shafts while caves are off. What follows about layer 7 describes the code,
which is intact, and applies again when the densities come back.

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
was **6 µm over densities spanning 18 m**. It is also the first thing to run
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
- **Not done**: a faint strip just under the horizon where SSR misses land;
  no underwater rendering.

### The shoreline: swash and wet ground

The trains die over the shoal, so the sea's edge used to sit at the
still-water line in every weather, the sand dry to it in a gale as in a calm.
One function in `water_wave.h` now gives the water running up the beach and
back -- `ww_swash` -- and three things read it, so they agree on where the
water is: the sea's sheet (water.hlsl), the wet ground under it (mesh.hlsl)
and the swimmer (`ww_water_at`, physics_world.cpp).

- **The sheet**: within 1.5 m of water depth (`ww_swash_reach`) the sea's
  vertices rise by the swash; the terrain in front hides it where the beach
  is higher. Its height rides the eight longest trains' elevation at the
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
  riding 12 cm over the ground, so there is always water drawn for the
  pixel test to cut; the clip is only a coarse cut (`i.depth + 0.3`).
  Two attempts drew a staircase, worst looking along the shore: the sheet
  a few cm over the beach is nearly coplanar with it, so where they cross
  follows both meshes' triangles (and the vertex-interpolated clip stepped
  with the grid); then fading by the thickness between the SHEET and the
  ground only softened the steps, because near the shore the sheet's own
  height is interpolated across grid cells.
- **Foam** keys to the last 0.6 m of water, not 2.2: at 2.2 the whole sheet
  was white as snow; now foam at its leading edge, clear water behind.
- **Wet ground** (mesh.hlsl, before lighting; the swash rides in the shadow
  constants, `l_swash_a/b`, `l_swash`): damp from the waterline to the full
  run-up + 0.2 m, fading over 0.4 m -- albedo x 0.55, roughness to 0.35 --
  and a film in the 30 cm the water has just left -- roughness 0.06, the
  scan's normal flattened. At 0.85 of the run-up the damp band ended about
  where the sheet stood and barely showed. Grass shows it modestly; this
  session had no sandy beach to judge it on.
- **Physics**: water is present where the running sheet covers the ground;
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
- **Trees in the wind, everywhere they are drawn or traced.** The sway is
  `engine/render/tree_sway.h` and the gusts `engine/render/wind.h`, both
  compiled as C++ AND HLSL (`wind.hlsli` only adapts them), so the mesh
  `trees.hlsl` bends and the instance the RT scene leans move by one formula
  in one gust. `small` is a Windows macro (`#define small char`): not a
  variable name in a header the C++ side includes.
  - **RT**: every standing tree's instances -- cards, leaf cutouts, crown
    ellipsoid -- are SHEARED by the crown's offset (`RtScene::set_sway`, in
    the TLAS rebuild it already did every frame): an instance transform is
    affine, the mesh bends as h^2, so the shear is the least-squares line
    through h^2 over the crown pinned at the base (`TS_SHEAR_FIT` 0.8).
    `--rt-report` at 20 m/s, key-light agreement with the (swaying)
    cascades in the two woodland views: 75.8 / 78.7% upright, 76.5 / 81.0%
    leaning, 77.8 / 81.4% in calm air. Cost: nothing measurable (1440p walk,
    7,419 instances). `--rt-no-sway` for A/B. DDGI's probe rays and the
    traced sky see the same instances.
  - **Impostors** move rigidly by the offset at h^2 = 0.45 (the crown's
    mass), main and shadow passes alike.
  - **Species bend**: archetypes.json `shape.bend` (presentation only),
    scaling lean and swing: birch 1.4, willow 1.3, alder 1.1, maple 1.0,
    beech 0.9, oak 0.85, pine 0.75, scrub 0.7, fir 0.6, spruce 0.55, snag
    0.25. In `SpeciesLook` for the GPU, `TreeLook` for the RT scene.
  - Not done: the canopy map (1 m crowns for the no-RT sky shade) is static;
    leaf flutter is raster only (centimetres).
- **Not done**: no rain, cloud or fetch shelter behind islands.

The water asks the SHARED FIELD FUNCTION how deep it is, so the shoreline is
the real one at every LOD. That is M1 paying off somewhere other than terrain.

**The sea is swimmable, and it is still not in residency.** Physics has no
water body. The character asks `ww_height_at` in `water_wave.h` each step,
the same function the shader draws, with the same level, sea state and
clock. Whether there is sea in a column at all is the shader's own rule:
`real_terrain_height` below sea level. `streamer.h` still contains no
reference to the sea. Keep it that way: `--band-report`'s "worst miss"
compares the band against `worldgen::terrain_height`, and a water surface
feeding residency would leave that check passing while it measured something
else. Adding swimming left the band report byte-identical.

`--swim [s]` walks off the nearest gentle beach, swims out, turns and comes
back. Its headline is **frames standing on the seabed in water too deep to
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
  **A submerged camera has no underwater rendering:** the seabed is lit like
  dry land, with the surface overhead.

`--overhang` has no landmark to aim at: it searches for a real ceiling
(phase A), reports what it found and how the capsule left it, then digs a
cavity of its own (phase B), which is the decisive test. See the gotcha on
overhang collision above.

## Style

- Shaders: `snake_case`. C++: the existing style in `engine/`. TS: the eslint config.
- Prefer data in `archetypes.json` over constants in code.
- Commit in logical steps, with messages explaining why rather than what.
