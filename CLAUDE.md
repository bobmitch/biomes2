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
`--band-report`, `--sea-report`, `--swim`, `--blend-report` and
`--tab-test`. `--flight` also takes `--settle`, `--holes` and
`--flight-speed <m/s>`; every mode takes the streaming A/B switches
`--no-lod-hold`, `--no-prioritise`, `--adaptive-budget` and
`--lod-fade <frames>`. It is a GUI subsystem binary, so a shell does not wait
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
  vertex two voxels into the solid along −normal, and `cs_quads` hangs a
  double-sided curtain from every mesh-border edge on an X/Z face
  (`border_side`). A skirt vertex's normal is stored at HALF length; shading
  normalises it, and `is_skirt_vertex` tests the length exactly. Do not
  normalise normals in the kernel. The physics patch is compacted without
  skirt vertices, so nothing downstream needs a predicate. It costs +12%
  vertices and +19% triangles, and slots are now 4608 vertices.
  `g_cell_vertex` packs the surface index (low 16 bits) and the skirt index
  (high 16 bits).
- **The toothed ledges along curved lines are LOD seams, not chunk seams.**
  Tinting the curtain magenta on a rough hillside showed it visible almost
  only along the ring boundaries, 3.7% of the frame: the coarse level's mesh
  sits up to about a coarse voxel off the fine one, and the cover shows as a
  zigzag-topped wall. The lip showed the same rims. Covering the crack cannot
  remove a step; transition cells (Transvoxel-style) or morphing the fine
  side's border onto the coarse surface would.
- **Rough terrain runs the 768-slot pool dry.** The hillside shot at
  (2420, 118, 3390) ends its capture with 0 free slots, 96 chunks never
  meshed and 5 overflowing a slot, with the old lip as well as the skirt.
  Those are holes, and nothing flags them outside the shot's `.txt`.
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
- **The first valid overhang collision test fails.** v9's terrain gave
  `--overhang` a real ceiling at last: a slab 3.75 m thick, solid 1 m out in
  every direction. Through v8 it only ever found spikes too thin to test
  (0.0 m wide). The capsule launched upward passes straight THROUGH it, with
  every mesh shape delivered and with physics at all LODs. This predates the
  LOD 2 cap (A/B'd) and is not yet investigated.
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

## Testing

`tools/validate` runs 200 seeds and exits non-zero on any failure. It asserts
presence, quota, radial ordering, land fraction, adjacency, edge taper, home
landmass, boat connectivity, spawn, slope budget, river sanity, LOD agreement, blend seams,
cave entrances, throughput and the golden hashes, and emits a 64-seed contact
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
worst case is currently 0.45 of a voxel (0.41 before v9's blend fix). Normalise by the FULL amplitude,
never the faded one: rescaling the survivors changes the shape of what
remains, which is the thing LOD agreement forbids.

Note what the check does and does not catch. Cutting octaves ten times too
early takes it to 4.09 and fails, so it bites on over-aggressive dropping.
It did NOT notice normalising by the faded amplitude, because that error is
smaller than the worst case already present — it is a guard against the loud
version of the bug, not the quiet one.

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
guessing:

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
`--water-debug 1..5` (depth, foam, normal, displacement, surviving amplitude),
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

`--overhang` no longer has a landmark to aim at: it searches for a real
ceiling and reports what it found. Near spawn the layer-6 overhangs are
spikes — 4.75 m thick and under 0.5 m wide — so a launched capsule rises
beside them, and the report says so rather than claiming the collision failed.
11.6% of land columns overhang, so the world has them; it does not yet have
one wide enough to be a test.

## Style

- Shaders: `snake_case`. C++: the existing style in `engine/`. TS: the eslint config.
- Prefer data in `archetypes.json` over constants in code.
- Commit in logical steps, with messages explaining why rather than what.
