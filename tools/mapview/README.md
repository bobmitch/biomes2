# mapview — Track A, milestone A1

A top-down 2048² map of the world disc, rendered by a WebGPU compute shader
calling the canonical field function.

## Running it

The WGSL under `src/generated/` is a build product. Generate it first, then
start the dev server:

```bash
tools\spike\build.bat
```

```bash
cd tools/mapview && npm install && npm run dev
```

Then open http://localhost:5173. Needs a browser with WebGPU — Chrome 113+,
Edge 113+ or Safari 18+. There is no fallback path: the field function runs in
a compute shader or not at all.

## What it shows

- **World seed**, with prev/next. Re-renders in a few milliseconds once the
  pipeline is warm.
- **Layer**: continent mask, danger field D, raw fBm, layer-0 height.
- **Land fraction** for the current seed, counted on the GPU.

## What is deliberately missing

**Superseded:** `w` and the danger feature size now have sliders. The rest of
the layer-0 and layer-1 parameters still do not, for the reason below.

**Tuning sliders.** The starter prompt put them in A1, but `w` — the parameter
they most matter for — can only be judged against a biome map, and biomes are
A2. A slider you cannot evaluate is UI you rebuild once you can. They land with
A2.

## Where terrain comes from

Nothing in `src/` defines terrain. `src/generated/*.wgsl` is emitted by
`tools/wgslgen` from `worldgen/shaders/`, which is the canonical source and
also compiles as C++ for `tools/spike` and as HLSL for the engine
([ADR-001](../../docs/adr-001-field-function-authority.md)). If a number that
shapes the world shows up in this directory, it is in the wrong place.

`src/kernel.wgsl` is hand-written and stays that way: storage textures,
workgroups and atomics have no CPU counterpart and are outside the dialect's
scope. It calls the field function; it does not define any of it.

## Why this page is also a test

Browsers compile WGSL with a real frontend (Tint, in Chromium), so loading this
page is the only check that `tools/wgslgen` emits valid WGSL — the generator
itself only knows it produced text. `main.ts` reads
`getCompilationInfo()` and fails loudly with the line and column rather than
leaving a blank canvas to be mistaken for a worldgen bug.

It caught one on the first run: WGSL rejects a mix of `*` and `^` without
parentheses instead of applying C's precedence, which the C++ build accepts
silently. The fix went into the dialect source, where it is correct for all
three backends.

The land-fraction readout is the other half of that check. It should match
`build\quota_spike.exe --dump-seeds <seed> --grid 2048` — the CPU build is the
determinism authority, and this is the tolerance comparison ADR-001 requires.
At world seed 0 both report 54.33%. It can never be an exact hash comparison:
WGSL §15.7.5 permits reassociation and FMA fusion with no opt-out.
