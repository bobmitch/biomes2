# Validation harness

A headless measurement of the claim the whole design rests on: *every biome,
every seed, within ±10% of its quota, ordered calm-to-hostile*. No graphics —
it scatters cells, rasterises the disc for real cell areas and a land mask,
runs `worldgen/assign/`, and reports what the validation harness will later
assert.

```bash
tools\validate\build.bat                       # from the repo root
build\validate.exe --seeds 200 --grid 1024
build\validate.exe --mode 0              # the original gate, for comparison
build\validate.exe --detail              # per-biome breakdown, one seed
build\validate.exe --sweep               # the three quota gates
build\validate.exe --sweep-cells         # cell size vs quota accuracy
build\validate.exe --sweep-bands         # band width vs candidate count
build\validate.exe --sweep-w             # w vs patch size
build\validate.exe --contact-sheet       # 64 seeds -> build/contact_sheet.bmp
build\validate.exe --test-adjacency      # are adjacency rules honoured?
```

Useful flags: `--w`, `--danger-feat <metres>`, `--cellarea <m2>`, `--mode 0|1|2`,
`--tile <px>`, `--out <path>`.

The contact sheet is written as an uncompressed 24-bit BMP. A PNG writer needs
zlib framing and CRCs, which is more code than a debug image justifies, and the
harness is meant to stay dependency-free. Convert in CI if the artifact viewer
insists on PNG.

`tools/validate/noise_standin.h` is a **stand-in**, not the authority. It exists
only so layer 3 has a plausible land mask to chew on. Delete it when
`worldgen/shaders/noise.wgsl` exists rather than keeping the two in sync — two
implementations of a float-heavy function will not agree.

## What it found

All figures R = 10 km, 200 seeds, 1024² sample grid, 9 biomes, ~235 cells.

### 1. The assignment as specified misses its own quota target on every seed

| Config | mean err | p95 | max | seeds failing ±10% |
| --- | --- | --- | --- | --- |
| As specified in the design doc | 41.8% | 62.3% | 67.5% | **200 / 200** |
| With the half-cell gate, 0.32 km² cells | 3.5% | 7.0% | 8.9% | 0 / 200 |

> Figures in §1 were re-measured after layer 0 became a floor rather than a
> dome, which lifted land fraction to 55.4% and so gave each biome a few more
> cells. The tables in §2–§6 below predate that change. Their conclusions are
> unaffected — they are about layer 3 and the danger field, neither of which
> touches the continent mask — but the absolute land fractions in them read
> about half a point low.

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

This is now the **default** in `AssignParams`, and the design doc's layer-3
pseudocode has been corrected to match. `--mode 0` reproduces the original
behaviour for comparison; the tables below name whichever gate was used.

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

> **Since fixed.** The danger bands in `archetypes.json` have been widened ×2
> (see §6), which takes the candidate set to 2.5 and relaxation to zero. The
> paragraph below describes the narrow bands as originally authored, which is
> how the inert cost function was found.

The mean candidate set in pass 1 was **1.0**. Quota exhaustion closes biomes out
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

### 5. The world reads as a dartboard at the specified `w`

This is what the contact sheet is for, and it earned its place on the first
run. At the design doc's `w = 0.20` and a 4.5 km danger feature size, all 64
seeds show the same silhouette: a solid bright-green meadows blob in the
centre, a ring of mixed terrain, glacier hugging the coast, and rift present
only as a thin fringe at the rim. Three of the exact failures the doc says the
sheet catches — same silhouette every seed, one biome always on the coast,
radial structure visible — are all present at once.

**Spearman reads 1.00 throughout.** No assertion in the harness fires. The only
reason we know is the picture.

The cause is structural. `D = saturate(r² + w·fbm)` with `w = 0.20` means the
noise can move `D` by at most ±0.20 against a term that sweeps the full 0→1.
Worse, `r² < 0.2` for all `r < 0.45`, so the inner fifth of the world's area is
locked to the calmest biomes on every seed regardless of noise. To put a
hostile pocket within sight of spawn you would need `w ≈ 0.9`.

Raising `w` alone trades the dartboard for confetti, because `w` is also the
patch-size control from §3:

| `w` | danger feature | mean patch | Spearman mean/min | quota err | reads as |
| --- | --- | --- | --- | --- | --- |
| 0.20 | 4.5 km | 4.4 cells / 1.41 km² | 1.00 / 1.00 | 3.9% | clear dartboard |
| 0.35 | 4.5 km | 3.4 cells / 1.09 km² | 1.00 / 1.00 | 3.8% | still radial |
| 0.60 | 4.5 km | 2.6 cells / 0.84 km² | 1.00 / 0.98 | 3.8% | confetti |
| **0.60** | **9 km** | **3.8 cells / 1.22 km²** | **0.99 / 0.95** | **3.8%** | **best so far** |

Raising the feature size with `w` is what separates them: fewer, larger
non-radial lobes break the rings without shredding the patches. Quota accuracy
is untouched throughout, and radial ordering still passes with margin.

**Settled at `w = 0.60` with a 2 km danger feature**, not the `0.15–0.25` at
3–6 km the doc first guessed. Note this table recommended 9 km, on the
reasoning that a larger feature was the only way to keep patches from
shredding. §7 removed that constraint: with the repair pass restoring patch
size independently, the small feature became the better choice — it gives
granular zones that still read as places. A feel judgement, taken by the
author on the contact sheet rather than from any assertion.

Even so, meadows is still a central blob on most seeds and glacier still
favours the rim. Breaking that needs the danger field to stop being a pure
function of radius plus noise.

### 6. Adjacency constraints were not honoured — fixed by widening the bands

Following from §3: with a candidate set of 1.0 the cost function was inert, so
the `adjacency_mask` rules in `archetypes.json` were decorative. They held only
because the danger ordering keeps `meadows` and `rift` apart anyway.

`--test-adjacency` forbids the two biomes whose danger bands overlap most, then
counts how often that rule is broken over 96 seeds:

| danger bands | pair | cell borders | violations | seeds with ≥1 | verdict |
| --- | --- | --- | --- | --- | --- |
| as originally authored | forest/wetland, overlap 0.22 | 119,111 | 3,921 | 95 / 96 | **FAIL** |
| widened ×2, now in the table | forest/wetland, overlap 0.51 | 119,111 | 0 | 0 / 96 | PASS |

The constraint mechanism worked all along — it never got a choice to apply it
to. **The bands in `archetypes.json` have been widened ×2 around each
`danger_preferred`, clamped to [0,1].** Measured before and after over 200
seeds at a 1024² grid:

| | quota err | Spearman | mean patch | relaxed/seed | candidates |
| --- | --- | --- | --- | --- | --- |
| narrow bands | 3.9% | 1.00 / 1.00 | 4.4 cells / 1.41 km² | 104.2 | 1.0 |
| widened ×2 | 3.9% | 1.00 / 1.00 | 4.4 cells / 1.41 km² | **0.0** | **2.5** |

Everything that matters is unchanged, relaxation is gone, and the cost function
finally has a choice to make. The band-coverage invariant cannot break, since
widening only increases coverage.

It also changes the *output* almost not at all: re-rendering the contact sheet
with the old and new bands differs in 237 bytes of 7.2 million, 0.003% of
pixels. The relaxation branch had been placing those cells in the same biomes
the widened bands now permit legally. So this is a correctness fix with no
effect on how the world looks — and in particular it does **not** help the
dartboard problem in §5, which is entirely about `w`.

### 7. Patch size is a knob again — repair pass

§3 left patch size with no working control: the contiguity bonus is inert, so
the only thing moving it was `w`, which is also the control for how radial the
world reads. Wanting small coherent zones meant turning `w` up, which bought
granularity by *fragmenting* the assignment — granularity as noise rather than
as places.

`--repair N` adds a post-pass that swaps biome **labels** between pairs of
cells. A boundary cell moves into a neighbouring biome and pays for it by
moving some cell of that biome back, so every biome's cell count is identical
before and after. Presence cannot break. Only the area difference between the
two cells drifts, inside a budget. Swaps that leave a cell outside its new
danger band, create a forbidden border, or exceed the budget are rejected.

| passes | mean patch | quota err | Spearman mean/min | seeds breaking adjacency |
| --- | --- | --- | --- | --- |
| 0 | 1.9 cells / 0.63 km² | 3.5% | 1.00 / 0.98 | 70 / 200 |
| 2 | 6.5 cells / 2.10 km² | 3.5% | 0.99 / 0.93 | 1 / 200 |
| **4** | **7.6 cells / 2.46 km²** | **3.6%** | **0.99 / 0.93** | **0 / 200** |
| 8 | 7.9 cells / 2.57 km² | 3.6% | 0.99 / 0.93 | 0 / 200 |

Measured at `w = 0.60`, 2 km danger feature, 200 seeds at 1024². It converges
by about four passes, triples patch area at every `(w, feature)` pair tried,
leaves quota error untouched, and costs well under a second over 200 seeds.

**It also repairs adjacency, and that turned out to be load-bearing.** The
greedy pass treats `adjacency_mask` as a cost rather than a veto — by design,
so it cannot deadlock — so it can leave forbidden borders behind. A
fast-varying danger field makes that far likelier: at these settings 70 seeds
in 200 had a `meadows` cell touching `glacier` or `rift`, against 0 at the old
`w = 0.20` / 4.5 km. Scoring violation removal as a gain, and refusing any
swap that raises the count, takes it to 0 in 200. **Without the repair pass
these settings would not be usable at all**, which was not obvious before the
harness started asserting the authored rules on every run rather than only in
`--test-adjacency`.

The price is radial ordering: Spearman 1.00 → 0.92–0.99, still comfortably
above the 0.8 the harness asserts. That is the real trade — not quota, which
the label-swap construction protects, but how strongly calm-to-hostile reads.

**On by default** at 4 passes, together with `w = 0.60` and a 2 km danger
feature. Those three go together: the small feature gives granular zones, `w`
breaks the radial banding, and the repair pass is what stops the first two
reading as noise. Changing any of them moves every seed, which is free only
until A3 pins the golden hashes.

## What this implies for A2

- The half-cell gate is in and is the default. Done.
- Budget ~900 cells at R = 10 km, not ~245. Cell lookup cost roughly 4×; still
  nothing on the GPU, but the 64×64 bucket grid should be re-checked.
- Either accept ~1.4 km² patches at R = 10 km, or add a mechanism that
  genuinely controls patch size — the contiguity bonus as specified is not one.
  The cheapest candidate is a second pass that swaps cells between
  over- and under-filled biomes to grow patches at constant quota.
- Bands widened ×2 and relaxation is now zero, so `--test-adjacency` passes.
  Done. Re-run it whenever the biome table changes — the failure mode is
  silent, and nothing else in the harness would catch it.
- Decide `w` by looking at `--contact-sheet`, not at Spearman. Spearman cannot
  see the failure (§5).
- The contact sheet belongs in CI as a build artifact from A3 onward, exactly
  as the design doc says. It is the only check here that caught anything the
  assertions missed.

## Caveats

- The land mask is a stand-in, not the real layer 0. Land fraction is
  calibrated to 54.9%, and cell count and land-cell count are therefore
  realistic, but the *shape* of the coastline is not the shipping one.
- Ocean cells are left unassigned. The doc's island clusters draw from the same
  quota pool and would shift these numbers somewhat.
- Rivers, height composition and 3D density are absent — they do not affect
  layer 3.
