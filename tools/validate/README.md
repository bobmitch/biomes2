# Validation harness

CI entry point. Runs 200 seeds, asserts every check it can, pins the world
against golden hashes, emits a contact sheet, and exits non-zero on failure.

```bash
tools\validate\build.bat && build\validate.exe
```

```
  presence           PASS   0 seeds missing a biome
  quota              PASS   max 8.2% off target, tolerance 10%
  radial order       PASS   Spearman min 0.93, threshold 0.80
  land fraction      PASS   mean 55.4%, target 55% +/- 5
  adjacency          PASS   0 seeds break an authored adjacency rule
  edge taper         PASS   0 seeds with land past r=0.95 or a rim above the void
  home landmass      PASS   smallest spawn landmass 460 cells, floor 16
  boat connect       PASS   0 seeds with a biome unreachable by sea
  islandness         n/a    2.2 landmasses/seed (max 7), spawn landmass holds 98.7% of land (min 76.0%)
  spawn              PASS   0 seeds without a coastal spawn in the calmest biome
  throughput         PASS   267 ns/sample, scalar CPU reference
  golden seeds       PASS   8 seeds pinned at generator_version 1
  contact sheet      PASS   build/contact_sheet.bmp
```

The `islandness` row is deliberately `n/a`. It is a reading, like the contact
sheet — see §8.

**Golden hashes are a gate, not a formality.** Any change to `worldgen/` moves
them and the build goes red. Revert, or decide the world should move and bump
`generator_version` in `worldgen/golden/golden.json`, then `--bless`. Hashes
are taken at a canonical 1024² sampling grid so they pin the generator rather
than whatever `--grid` a run happened to use, and they come from this C++ build
with contraction disabled — [ADR-001](../../docs/adr-001-field-function-authority.md)
makes it the determinism authority, because WGSL permits reassociation and FMA
fusion and so cannot produce a stable hash at all.

`--relief` renders the same 64 seeds as shaded relief instead of biome colour;
CI uploads both. `--closeup <seed>` renders one seed large.

**Use the close-up for anything narrower than a few hundred metres.** A
contact-sheet tile is about 87 m per pixel, so a river 40-170 m wide is
sub-pixel on it. The first drainage network shipped was four-connected, which
makes every river a right-angled staircase; it looked entirely fine on the
contact sheet and like a circuit board the moment one seed was rendered large.

One check the design doc wants is **not** asserted yet: **LOD agreement**,
which needs octave dropping at Nyquist. Asserting it now would measure the
wrong thing and report false confidence.

Two thresholds are judgement calls worth knowing about. **Home landmass** is
16 cells, about two mean biome patches — a guard against spawning on a bare
rock, not a requirement for a mainland (§8 explains why that distinction
matters, and what the 75% foot-connectivity assertion it replaced was really
doing). **Throughput** is gated at 400 ns/sample as a regression guard; the
design doc's 20 ns is a serial-equivalent budget for the complete field
function on the shipping path, and a scalar CPU reference build of layers 0–1
cannot be judged against it.

---

## How the checks were arrived at

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
`--tile <px>`, `--out <path>`, `--land-offset <f>` (§8 — the islandness lever;
it moves the world, so expect land fraction and the golden hashes to go red).

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

## 8. The world is a Pangaea on every seed, and an assertion was holding it there

The contact sheet reads as one continent on every seed. That is not a tuning
accident — it is the land-fraction target sitting on the wrong side of a
percolation threshold, with a validation assertion that had quietly made the
result a pass condition.

**Land shape is entirely layer 0.** `world.cpp` defines land as
`layer0_height(...) > sea_level`; nothing downstream moves a coastline, since
layer 4 fades the biome profiles out at the shore precisely so it cannot. So
islandness has exactly four controls: `land_offset`, `continent_freq`,
`continent_octaves` and `centre_floor`.

Sweeping `land_offset`, measuring connected components of land pixels at
768², 12 seeds per row:

| `land_offset` | land % | biggest mass, % of land | worst seed | masses ≥ 1 km² |
| --- | --- | --- | --- | --- |
| **0.17 (shipping)** | **55.4%** | **99.4%** | **97.7%** | **1.2** |
| 0.14 | 51.6% | 96.6% | 83.7% | 1.5 |
| 0.10 | 47.5% | 94.4% | 83.6% | 1.9 |
| 0.07 | 44.5% | 88.4% | 74.0% | 2.6 |
| 0.04 | 41.5% | 84.6% | 47.6% | 3.5 |
| 0.00 | 37.5% | 78.2% | 47.9% | 3.9 |
| −0.08 | 29.4% | 64.2% | 32.4% | 5.6 |

At the shipping offset, **12 seeds out of 12 are a single continent holding
99% of all land.** The transition is sharp and sits between 0.06 and 0.10 —
2D percolation flips around 45% area fraction, and the 55% land target is
comfortably on the connected side of it. The land-fraction goal and any
sailing-led design are the same dial pointed in opposite directions.

**Finer continent noise does not help**, which is worth knowing before anyone
spends a day on it. Re-solving `land_offset` to hold 55% land at each
frequency:

| feature size | biggest mass | masses |
| --- | --- | --- |
| 9000 m | 95.7% | 1.3 |
| 5200 m (shipping) | 99.3% | 1.2 |
| 2200 m | 99.2% | 1.3 |

Still one continent, just a wigglier one. Percolation depends on area
fraction, not feature scale.

**Seed-to-seed variety is free near the threshold.** At `land_offset = 0.07`,
with no per-seed parameter at all, seeds separate on their own — seed 9 gives
one 149 km² continent, seed 1 gives `99, 21, 7, 6`. Five of twelve stay
continental. The variance is already in the noise; the current offset is high
enough to crush it.

`centre_floor` is a second, independent dial, inert at 55% land but
load-bearing near the threshold:

| offset | floor | biggest % | worst seed | smallest home mass |
| --- | --- | --- | --- | --- |
| 0.07 | 0.20 | 88.4% | 74% | 99 km² |
| 0.07 | 0.00 | 81.5% | 46% | 62 km² |
| 0.00 | 0.20 | 78.2% | 48% | 57 km² |
| 0.00 | 0.00 | 59.0% | 42% | 47 km² |

So the two compose: `land_offset` breaks the world up, `centre_floor`
decides whether you still spawn on a mainland worth leaving. Below about 0.0
the floor saturates — it is a `wg_smax`, so it stops biting once it is under
the noise floor.

#### What was asserted, and why it was wrong

`foot connect` asserted that a flood fill from spawn over land cells reached
≥ 75% of all land. Three things were wrong with it:

1. **The threshold was never derived.** The design doc's validation table
   left it as a literal `X%`. 75% was filled in at implementation time, set
   just under what the world then produced (the worst seed measured 83.8%),
   and both commit `44c764b` and this README called it a judgement call.
2. **It did not test what it claimed.** The doc's "catches" column says
   *biomes stranded behind cliffs*, but the fill consults `is_land` and never
   slope. It was measuring ocean separation while wearing a cliff detector's
   label. Cliffs are already covered by the slope budget.
3. **It forbade a feature this document asks for.** Layer 3's spec calls for
   island clusters drawn from the same quota pool, and calls a biome landing
   island-only "free seed variety at no cost". An archipelago fails a 75%
   foot-connectivity gate by construction.

It is now split in two. **`home landmass`** asserts the spawn landmass holds
≥ 16 cells — roughly two mean biome patches (§7), enough to start a game on,
and a bar an archipelago clears easily. **`islandness`** reports landmass
count and the spawn landmass's share of land, and asserts nothing. Which of
those a world should be is a feel judgement taken on the contact sheet, the
same way `w` was settled in §5.

Verified by running the harness at `--land-offset 0.04`, 48 seeds — a world
averaging 4.4 landmasses with the spawn landmass down to 39.3% on the worst
seed:

```
  home landmass      PASS   smallest spawn landmass 151 cells, floor 16
  boat connect       PASS   0 seeds with a biome unreachable by sea
  islandness         n/a    4.4 landmasses/seed (max 10), spawn landmass holds 86.5% of land (min 39.3%)
  spawn              PASS   0 seeds without a coastal spawn in the calmest biome
  slope budget       PASS   worst seed has 78.8% of land under 30 degrees
  land fraction      FAIL   mean 41.7%, target 55% +/- 5
  golden seeds       FAIL   8 of 8 moved: revert, or bump generator_version
```

Everything that is a genuine requirement passes. The only two failures are
the land-fraction target — a real design decision, taken deliberately or not
at all — and the golden hashes, which is the gate working.

#### What is not done here

The world has **not** been moved. `land_offset` is still 0.17 and the golden
hashes are untouched, because choosing how islandy the world should be is
exactly the kind of call this project takes by looking rather than by
assertion. `build\view.exe` now has a **land offset** slider with a live
landmass readout for making it, and the harness takes `--land-offset` for
measuring it across seeds.

Island clusters themselves remain unimplemented: `assign.cpp` skips ocean
cells in both passes, and `allow_island` is parsed from `archetypes.json` and
never read. Lowering `land_offset` today gives you more coastline and more
islands, but those islands get their biomes from the ordinary land assignment
— there is no island-specific placement, and no biome can land island-only.

## 9. The caves had no way in, and the golden hash could not see the layer that fixed it

`--cave-access` walks random land columns, finds the first solid going down
and then the first air below it, and reports that thickness: the rind of rock
sealing the cave system off from the surface. Over 12 seeds and 400 land
columns, 43% had subsurface air beneath them, 83% of those under 25 m or more
of rock, and the shallowest rind anywhere was 1 m. Never zero.

That is not bad luck. `cave_depth_mask` forbids carving within
`cave_depth_min` of the heightfield, which is what stops the terrain reading
as swiss cheese and equally what stops any cave reaching daylight. The few
1–5 m rinds were not cave mouths either, only places where the layer-6
overhang term happened to drop real rock below the heightfield the mask is
measured against — an accident of a term that exists for another reason.

Layer 7 answers it with shafts traced from the surface down into existing
cave air, kept only if the walk reached it. `--entrances` walks every stored
shaft: over 200 seeds, 20 to 24 trunks per seed (mean 23.8), zero of 220k
subsurface segments still solid at their midpoint, zero of 4769 mouths sealed
a metre under the lip, mean reach 86 m.

**The hash was blind to the whole layer.** Adding layer 7 left all eight
golden hashes byte for byte unchanged. The hash samples a 1024² grid — one
density sample per 156 m — and a world has ~24 shafts a few metres across, so
a sample lands in one about never. Every entrance would have been free to
drift through CI. The shaft buffer is hashed directly now, and the general
rule is that **a layer whose features are smaller than the hash grid must be
pinned by its buffer.** Layer 5 escapes only because 428 river segments per
seed carve a wide enough area to land in the samples.

Both halves were mutation-tested, and they catch different things:

| Mutant | `cave entrances` | `golden seeds` |
| --- | --- | --- |
| `entrance_carve` disabled — shafts stored but never opened | FAIL | **PASS** |
| walk never terminates in air — no shafts at all | FAIL | FAIL |

So the hash pins *where* the shafts are and the check pins *that they are
open*. Neither covers the layer alone.

What the gate does not catch: it samples every 16th segment, because
exhaustive costs about a minute over 200 seeds. It will catch a systemic
pinch, not one shaft in a thousand closing up halfway down. `--entrances` is
the exhaustive version.

`--entrance-section <seed>` draws one shaft, and is an *unrolled* section —
the horizontal axis is distance along the shaft's own ground track, not a
fixed plane. A shaft tilts up to 1.8 lateral metres per metre of drop and
wanders, so it leaves any plane within a few metres; a plane section through
the mouth shows the bowl and nothing else, which was the first thing tried.
Both axes are drawn at the same metres per pixel, or every shaft looks like
the same gentle ramp whatever its real gradient.

Note that `--cave-access` still reports a sealed system, and correctly so: 24
shafts in a 314 km² disc will never be hit by 400 random columns. It measures
the part of the world that remains sealed, which is nearly all of it, and that
is the intended shape — the caves are sealed except where an entrance says
otherwise.

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
