# Starter prompt

Paste into a fresh session in an empty repository. It bootstraps the tree and
delivers Track A milestone A1. Design reference: `docs/worldgen-design.md`.

---

Set up this repository and deliver milestone A1 — a browser WebGPU map view of
the base terrain field with a seed scrubber.

## What the finished system does

- A disc world of radius R (parameter; 10 km default, 20 km supported), tapering
  to deep ocean and then to void past r = 1.
- Every biome present in every seed, each within ±10% of a target share of LAND
  area. Guaranteed by construction, not by tuning.
- Biomes ordered by distance from centre: calm in the middle, hostile at the rim,
  with enough noise that it never reads as concentric rings.
- Volumetric and destructible: caves, overhangs, digging.
- Base terrain is a PURE FUNCTION, never stored. Only player edits persist.

## Architecture constraints — do not deviate without asking

1. The field function lives in `worldgen/shaders/` as a RESTRICTED WGSL SUBSET
   that translates mechanically to HLSL. Plain functions over vec/float plus a
   small set of structs. No WGSL-only idioms, no pointers.
2. Noise uses an analytic integer hash (PCG or xxhash-lite). No permutation
   textures — they are not bit-reproducible across our two backends.
3. Per-feature seeds: `seed_feature = hash64(world_seed ^ FEATURE_ID)`, with a
   compile-time FEATURE_ID per layer. Retuning one layer must not reshuffle any
   other layer.
4. `worldgen/` depends on nothing in `tools/` or `engine/`. Enforce with a lint rule.
5. World coordinates are centred on the origin: ±R, never 0..2R.
6. Use simplex-family noise, not classic Perlin. Perlin's axis-aligned artifacts
   are invisible at 10 m and glaring at 5 km, which is the scale we work at.
7. All biome tuning lives in `worldgen/data/archetypes.json`. The generator knows
   nothing about what any biome means. No biome names in shader code.

## Create this tree

```
worldgen/shaders/      noise.wgsl, field.wgsl, cells.wgsl
worldgen/assign/       quota assignment, portable C++, no host deps
worldgen/data/         archetypes.json
worldgen/golden/       pinned seed hashes (empty for now)
tools/mapview/         TypeScript + WebGPU, Vite
tools/validate/        headless harness (stub for now)
engine/                empty, with a README stating its scope
docs/                  the design doc
CLAUDE.md, README.md, .gitignore
```

## Milestone A1 — the deliverable

A Vite + TypeScript page that renders a 2048x2048 top-down map of the world disc
via a WebGPU compute shader calling into `worldgen/shaders/`, with:

- A seed input and prev/next buttons. Changing the seed re-renders in under 100 ms.
- A layer selector: continent mask, danger field D, raw fBm, final layer-0 height.
- Live sliders for the layer-0 and layer-1 parameters, including the danger noise
  weight `w`, which is the most important feel parameter in the generator.
- A readout of land fraction for the current seed.

```
Layer 0:  h = fbm(p * fc)
            + A * (1 - smoothstep(0, 0.25, r))
            - B * smoothstep(0.80, 1.0, r),  and h -= LARGE when r > 1

Layer 1:  D = saturate(r2 + w * fbm_low(p))
          where r2 is normalised AREA (r^2), not normalised radius — area grows
          as r^2, and using radius here breaks every quota downstream.
```

## Out of scope for this session

Region cells, quota assignment, rivers, 3D density, the C++ engine. Those are
A2 onward. Do not start them.

## Definition of done

The page runs via `npm run dev`, the map responds to seed and slider changes
without a reload, and `README.md` explains how to run it. Commit in logical steps.
