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
`--dig-verify`, `--dig-stress`, `--check-dialect`, `--probe-field` and
`--band-report`. It is a GUI subsystem binary, so a shell does not wait on it:
run it with `Start-Process -Wait` or the file you read will be the last run's.

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
- Most of the clipmap meshes to nothing — nine chunks per LOD 0 column and the
  surface crosses one. A chunk that meshes to zero vertices gives its slot back
  and keeps its key: dropping the key would requeue it every frame. Any code
  path that meshes outside the frame loop has to reclaim too: `capture()` did
  not, and from 1400 m up it spent the whole pool on the air around the camera
  and returned an empty sky over 483,590 triangles it never reached.
- There was no skirt pass for five years' worth of commits, while a comment in
  `streamer.h` said there was. Nothing contradicted it because at five LOD
  levels the seams were near the camera and sub-pixel. Eight levels took a
  ground shot from 71 cracks to 569. The fix is a lip in `cs_vertices`: the
  outermost ring of cells drops two voxels, so neighbouring chunks overlap
  vertically. Measured 0 sky pixels below the horizon afterwards.
  **Any check that measures a vertex against the field must skip the lip** —
  `is_lip_vertex` in `streamer.h`. It is off the isosurface by construction,
  and counting it took slot 0's worst residual from 0.60 m to 1.33 m, which
  would have masked any real disagreement up to a metre.
- A chunk's index cap must track its vertex cap at about **6 per vertex** — a
  closed quad surface has roughly as many quads as vertices. A flat 16384
  against 4096 vertices overflowed on chunks the vertex cap was happy with,
  and an overflow is dropped geometry, which is a hole.
  `--flight` prints what the worst chunk actually wanted.

## Testing

`tools/validate` runs 200 seeds and exits non-zero on any failure. It asserts
presence, quota, radial ordering, land fraction, adjacency, edge taper, home
landmass, boat connectivity, spawn, slope budget, river sanity, LOD agreement,
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
worst case is currently 0.41 of a voxel. Normalise by the FULL amplitude,
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
equally at every level. Consistent with sub-metre overhang spans aliasing at a
64 m voxel, though that last step is inference, not measurement.

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
