# Layer-3 quota spike

A headless measurement of the claim the whole design rests on: *every biome,
every seed, within ±10% of its quota, ordered calm-to-hostile*. No graphics —
it scatters cells, rasterises the disc for real cell areas and a land mask,
runs `worldgen/assign/`, and reports what the validation harness will later
assert.

```bash
tools\spike\build.bat                       # from the repo root
build\quota_spike.exe --seeds 200 --grid 1024
build\quota_spike.exe --detail              # per-biome breakdown, one seed
build\quota_spike.exe --sweep               # the three quota gates
build\quota_spike.exe --sweep-cells         # cell size vs quota accuracy
build\quota_spike.exe --sweep-bands         # band width vs candidate count
build\quota_spike.exe --sweep-w             # w vs patch size
```

`tools/spike/noise_standin.h` is a **stand-in**, not the authority. It exists
only so layer 3 has a plausible land mask to chew on. Delete it when
`worldgen/shaders/noise.wgsl` exists rather than keeping the two in sync — two
implementations of a float-heavy function will not agree.

## What it found

All figures R = 10 km, 200 seeds, 1024² sample grid, 9 biomes, ~235 cells.

### 1. The assignment as specified misses its own quota target on every seed

| Config | mean err | p95 | max | seeds failing ±10% |
| --- | --- | --- | --- | --- |
| As specified in the design doc | 44.1% | 62.7% | 78.5% | **200 / 200** |
| With the half-cell gate, 0.32 km² cells | 3.9% | 6.8% | 10.3% | 1 / 200 |

This is a systematic bias, not granularity noise. `--detail` shows the shape of
it: every biome overshoots a little (+1.5% to +9.3%), and the entire
accumulated deficit lands on whichever biome sorts last by danger.

```
meadows    14 cells   target 14.0%   actual 14.9%    +6.5%
...
rift        8 cells   target  9.0%   actual  4.2%   -53.2%
```

The cause is the quota gate. `quota[b] > 0` lets a biome with a sliver of
quota left take a whole cell, so every biome overshoots by up to one cell, and
the last one in sort order absorbs the sum of everyone else's overshoot.

**Fix — one line in `assign.cpp`:** stop accepting cells once less than half a
cell of quota remains (`QUOTA_HALF_CELL`). The rounding error becomes symmetric
instead of always positive and stops accumulating. At R = 20 km this alone
takes the failure count from 21/48 to 0/48.

A softer, gateless variant (`QUOTA_SOFT_FILL`, cost rises as a biome fills) was
also tried and is **much worse** — 83% mean error. The hard quota gate is what
*makes* the guarantee; it cannot be relaxed into a preference.

### 2. R = 10 km needs roughly 4× the cells the scale table specifies

With the half-cell gate, sweeping cell size at R = 10 km:

| Cell area | Cells | Land cells | mean err | seeds failing | mean patch |
| --- | --- | --- | --- | --- | --- |
| 1.28 km² (the doc's figure) | 236 | 125 | 13.6% | 34 / 48 | 2.48 km² |
| 0.64 km² | 464 | 247 | 7.4% | 8 / 48 | 1.74 km² |
| 0.45 km² | 656 | 351 | 5.2% | 1 / 48 | 1.52 km² |
| 0.32 km² | 919 | 493 | 3.8% | 0 / 48 | 1.42 km² |

The doc's own granularity estimate (±7% at R = 10 km) was right about the
arithmetic but assumed the error is centred. It isn't, so the real requirement
is about 4× tighter. R = 20 km passes at the specified cell size.

### 3. The contiguity bonus does nothing, and patch size is not a free knob

The design doc calls patch size and quota granularity "independent knobs,
which is the whole trick". They are not independent as specified.

The mean candidate set in pass 1 is **1.0**. Quota exhaustion closes biomes out
so fast that there is usually exactly one legal biome per cell, which makes the
entire cost function — `danger_preferred`, the adjacency penalty, the
contiguity bonus — inert. Sweeping the contiguity bonus from 0.00 to 0.40
changes the output *not at all*, to four significant figures.

Widening the danger bands does restore real choice (candidates 1.0 → 3.8,
relaxation 104 cells → 0.3). The contiguity bonus **still** changes nothing,
because cells are processed in global danger order: a cell's already-assigned
neighbours are the ones at slightly lower D, which the `danger_preferred` term
would have given the same biome anyway. The bonus only ever reinforces a
decision already made.

The ~4.4-cell patches that do appear come from the danger field's own spatial
correlation. `w` is the actual patch-size control:

| w | mean patch (cells) | mean patch (km²) | quota err |
| --- | --- | --- | --- |
| 0.05 | 5.3 | 1.69 | 4.0% |
| 0.10 | 5.1 | 1.64 | 3.8% |
| 0.20 | 4.4 | 1.41 | 3.9% |
| 0.35 | 3.4 | 1.09 | 3.8% |

Usefully, `w` trades against patch size and **not** against quota accuracy, so
the two really are separable — just not by the mechanism the doc names.

### 4. The radial ordering is solid, and its assertion is not binding

Spearman is 1.00 in nearly every configuration, dipping to 0.98 once at
w = 0.35. The harness threshold of ≥ 0.8 never fires. Defining D on normalised
area (r²) works exactly as intended.

The flip side: the Spearman check will not catch a world that reads as a
dartboard. That judgement needs the contact sheet, not an assertion.

## What this implies for A2

- Take the half-cell gate before building anything on top of layer 3.
- Budget ~900 cells at R = 10 km, not ~245. Cell lookup cost roughly 4×; still
  nothing on the GPU, but the 64×64 bucket grid should be re-checked.
- Either accept ~1.4 km² patches at R = 10 km, or add a mechanism that
  genuinely controls patch size — the contiguity bonus as specified is not one.
  The cheapest candidate is a second pass that swaps cells between
  over- and under-filled biomes to grow patches at constant quota.
- Relaxation runs at ~21% of land cells in the passing config. Radial ordering
  survives it, but a fifth of cells ignoring their danger band means the bands
  are not really governing placement. Worth a look when biome counts change.

## Caveats

- The land mask is a stand-in, not the real layer 0. Land fraction is
  calibrated to 54.9%, and cell count and land-cell count are therefore
  realistic, but the *shape* of the coastline is not the shipping one.
- Ocean cells are left unassigned. The doc's island clusters draw from the same
  quota pool and would shift these numbers somewhat.
- Rivers, height composition and 3D density are absent — they do not affect
  layer 3.
