# CLAUDE.md

Procedural voxel world generator for a bounded disc world. Full design in
`docs/worldgen-design.md` — read it before changing anything in `worldgen/`.

## Layout

| Path | What |
| --- | --- |
| `worldgen/shaders/` | The field function. Restricted C dialect: compiles as C++ and HLSL. |
| `tools/wgslgen/` | Emits WGSL from the dialect. See `docs/adr-001-*`. |
| `worldgen/assign/` | Quota assignment (layer 3). Portable C++, no host deps. |
| `worldgen/data/` | `archetypes.json` — the biome table. All tuning lives here. |
| `worldgen/golden/` | Pinned seed hashes. Changes must be deliberate. |
| `tools/mapview/` | Browser prototype (TS + WebGPU). |
| `tools/validate/` | Headless N-seed harness. CI entry point. |
| `engine/` | C++ runtime: svdag, meshing, streaming, physics, render. |

## Build

```bash
cd tools/mapview && npm run dev
```

```bash
tools\validate\build.bat && build\validate.exe
```

```bash
cmake --preset dev && cmake --build --preset dev
```

## Invariants — breaking one of these is a bug, not a tradeoff

1. `worldgen/` imports nothing from `tools/` or `engine/`.
2. The field function is a RESTRICTED C DIALECT that compiles unmodified as
   C++ and as HLSL; `tools/wgslgen` emits the WGSL. One statement per line,
   functional casts, no structs/pointers/overloads, only the builtins in
   `field_types.h`. Never hand-edit generated WGSL. Kernel code
   (`surface_nets`, anything with workgroup memory, atomics or barriers) is
   OUTSIDE the dialect and stays hand-written per backend.
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
   is a TOLERANCE comparison, never a hash — WGSL §15.7.5 permits
   reassociation and fusion with no opt-out, so an exact check is impossible.
9. The field function uses ONLY `+ - * /`, `sqrt`, `floor` and comparisons.
   No `pow`, `exp`, `log`, `sin` or `cos`: libm transcendentals are not
   exactly specified and glibc disagrees with the MSVC CRT by an ULP. Layer 5
   is a discrete function of the heightfield, so one ULP moves a river — this
   broke three of eight golden seeds across platforms and only the two-OS CI
   matrix caught it. Shape curves are built from arithmetic; see
   `flatness_curve` in `worldgen/shaders/field.h` for the pattern.

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

## Testing

`tools/validate` runs 200 seeds and exits non-zero on any failure. It asserts
presence, quota, radial ordering, land fraction, adjacency, edge taper, foot
and boat connectivity, spawn, slope budget, throughput and the golden hashes,
and emits a 64-seed contact sheet — look at it. Automated checks cannot tell you whether a
world is interesting.

It also emits a shaded-relief sheet with `--relief`, and a single seed rendered
large with `--closeup <seed>`. Use the close-up for anything narrower than a
few hundred metres: a contact-sheet tile is ~87 m per pixel, so rivers are
sub-pixel there and an axis-aligned drainage network looked perfectly fine on
it. Not yet asserted: **LOD agreement**, which needs octave dropping at
Nyquist.

**Golden hashes are pinned.** Any change to `worldgen/` moves them and the run
fails. That is the gate working: either revert, or bump `generator_version` in
`worldgen/golden/golden.json` and re-pin with `build\validate.exe --bless`.
Never re-bless to make a red build green without deciding the world should
move.

Run it before any commit touching `worldgen/`.

## Style

- Shaders: `snake_case`. C++: the existing style in `engine/`. TS: the eslint config.
- Prefer data in `archetypes.json` over constants in code.
- Commit in logical steps, with messages explaining why rather than what.
