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
cd tools/validate && npm run check
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

`tools/validate` runs 200 seeds and asserts biome presence, quota tolerance,
radial ordering, connectivity, slope budget, LOD agreement and determinism. It
also emits a 64-seed contact sheet as a build artifact — look at it. Automated
checks cannot tell you whether a world is interesting.

Run it before any commit touching `worldgen/`.

## Style

- Shaders: `snake_case`. C++: the existing style in `engine/`. TS: the eslint config.
- Prefer data in `archetypes.json` over constants in code.
- Commit in logical steps, with messages explaining why rather than what.
