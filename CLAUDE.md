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
| `tools/validate/` | Headless N-seed harness. CI entry point. |
| `tools/view/` | Native world viewer (Win32). Debug tool, Windows only. |
| `tools/shared/` | Presentation, shared by the harness and the viewer. |
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

The engine's headless modes all write a text file under `build/` and exit:
`--shot`, `--flight <s>`, `--walk <s>`, `--overhang`, `--dig`, `--dig-save`,
`--dig-verify`, `--dig-stress`, `--check-dialect`, `--probe-field`,
`--band-report`, `--sea-report`, `--swim`, `--blend-report`, `--tab-test`,
`--sky-report` (CPU only, no device: the atmosphere against clear-sky
references), `--lights-report` (ADR-004 L3: local lights at night, six
configurations timed) and `--fell` (with `--fell-save` / `--fell-verify`).
Every mode takes `--time <hours>` (ADR-004 L2: 14 by default, which is the
pre-L2 sun), `--torch` (carry one) and `--no-light-shadows`; `--shot` also
takes `--lights <n>` (campfires round the camera) and `--ground` (the
`--cam` x and z at eye height on the ground). `--flight` also takes `--settle`, `--holes` and
`--flight-speed <m/s>` and `--flight-from x y z`; `--shot` takes
`--slot-report`; every mode takes `--mesh-debug 1|2` (curtain, creases) and
the streaming A/B switches `--no-lod-align`,
`--no-lod-hold`, `--no-prioritise`, `--adaptive-budget` and
`--lod-fade <frames>`, and the tree switches `--tree-distance <m>`,
`--tree-debug 1|2|3|4` (tint by level; only LOD 0 / LOD 1 / impostors),
`--check-rt` (ADR-004 L0: DXR end to end on one triangle),
`--tree-lod0 <m>`, `--no-trees`, `--no-shadows`, `--no-foliage`,
`--foliage-debug 1`, and the post chain's `--no-taa`, `--no-ao`,
`--no-contact`, `--no-post` and `--post-debug 1|2` (AO alone, contact
shadows alone), and exposure's `--ev-comp <stops>`, `--ev100 <v>` (fixed,
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

`T` in the window, or `--texture 0|1|2|3`: flat, noise detail (default), a
metric grid (1 m lines, heavier every 10 m), both. Procedural and
triplanar, in `mesh.hlsl`, antialiased with `fwidth` and faded where a pixel
covers the pattern. It costs ~0.1 ms of a 0.3 ms uncapped frame. `M` toggles
wireframe. The window title shows both.

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

**Felling** (`TreeFelling`): trunks within 40 m of the player are static
capsules; a click swings from the eye; 2 + one per 6 cm of diameter fells it.
The tree's id is marked felled, a stump (static) and a log (Jolt rigid body:
a 650 kg/m^3 trunk capsule and a 6 kg/m^3 crown sphere) replace it, both drawn
from the same mesh split at `kCutHeight`. F6/F9 save/load `build/edits.bin`
and `build/trees.bin` (same field id). `--fell` walks into a trunk, fells
it and watches the log; `--fell-save` then `--fell-verify` in a second
process checks the log is where it rested and the tree does not stand.

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
- **Night is a full moon opposite the sun**, 0.19 lux on the ground,
  exposed like a camera: the meter bottoms out at EV100 -2, which draws a
  moonlit landscape as a dim blue day. No stars, no moon phases, no Purkinje
  shift yet.
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

### Shadows

Sun shadows, three cascades of 2048^2 out to 35, 140 and 520 m, cast by the
terrain and the trees and received by both through one function,
`shadow_factor` in `engine/render/shaders/shadow.hlsli`. Each cascade is an
orthographic view down the sun fitted to a sphere round its slice of the
view frustum -- a radius that depends only on the splits and the lens, so it
never changes size -- with its centre snapped to whole texels, so edges do
not crawl as the camera moves. 3x3 PCF, a normal offset of 1.5 texels and
0.12 m of constant bias; past 520 m the shadow fades out under the fog.
`L` in the window, `--no-shadows` for A/B.

The terrain casts with the depth-only draw of every resident chunk (~0.2
ms). Trees re-run their GPU cull per cascade, levelled by the CAMERA's
distance so a shadow matches its tree, with a floor per cascade: the first
keeps LOD 0 and the real leaf pattern (dappled light), the second never
goes finer than LOD 1 and tests cards at their average coverage, the third
is impostors only, each fetching one baked view's alpha along the sun. At
full detail in every cascade with four blended views per impostor pixel,
shadows cost 3.1 ms over the densest forest; with the floors, 1.7 ms. An
impostor RECEIVES its shadow from a point just outside its bound on the sun
side: its card runs through its own crown and would otherwise sit in its own
shadow. The sea neither casts nor receives.

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

Wave height is a runtime value, because storms are a planned mechanic. The
dial is `sea_state`, 0 to 1, and `--sea-report` says what it buys: **0.20 m
crest-to-trough calm, 7.51 m at full storm**, with the surface mean staying at
sea level to 2.4 mm — the one thing worth asserting, because a sea that rose
with the weather would flood coastlines and no screenshot of a rough sea would
look wrong.

Controls: `--sea-state <0..1>`, `--sea-shot` (stands the camera off the nearest
coast, which is the only vantage a 7 m swell reads as a swell from),
`--water-debug 1..5` (depth, foam, normal, displacement, surviving amplitude;
with `--tonemap 0` since L1),
`--no-water`, and `[` `]` `K` in the window.

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
