# Voxel World Generation — Design Doc

Draft · last updated 2026-11-10

Living version (comment and edit there): https://claude.ai/artifact/8nuPt4GoZuViRseetj2gvu

## Summary and key decisions

A single-seed, radially-structured voxel world of 10–20 km radius, generated as a pure function and stored only where the player has changed it. Biome placement is a constrained assignment over Voronoi regions rather than noise thresholding, which is what makes "every biome, every seed, in a known proportion" a guarantee instead of a hope.

| Decision | Choice | Rationale |
| --- | --- | --- |
| Biome placement | Quota-assigned Voronoi regions, ordered by a radial danger field | Presence and area are guaranteed by construction; noise thresholds can't promise either |
| Radial banding | Equal-**area** rings, `r_k = R·√(k/N)` | Disk area grows as r²; linear rings give the inner band a few percent of the world |
| Base terrain storage | None — regenerated on demand from the field function | A 20 km world at 0.5 m is 6.4×10⁹ columns; even the heightmap can't be precomputed |
| SVDAG contents | Player edits only, as a sparse delta over the function | Edits dedup extremely well; noise terrain does not |
| Field function language | WGSL, ported to HLSL; one source of truth | It ends up in a compute shader regardless — write it there from day one |
| Prototype environment | Browser (WebGPU) for worldgen, C++ for the voxel pipeline | They de-risk different things and neither translates to the other's environment |
| Repository | One repo, several build systems | The shader source is shared verbatim by both tracks; a repo boundary through it guarantees drift |
| Physics shapes | `HeightFieldShape` by default, `MeshShape` only for overhangs and edited chunks | Cooking and query cost differ by an order of magnitude, and most chunks are 2.5D |

The two prototype tracks are independent and can run in parallel. Track B (C++ pipeline) should be built against a trivial analytic world — a sine heightfield and a sphere — so that pipeline bugs are never confused with worldgen bugs.

## Goals, constraints and non-goals

The generator must satisfy six hard requirements; everything else in this document is in service of them.

1. **Bounded disc world.** Radius `R` is a parameter, 10 km or 20 km. 10 km is Valheim scale (314 km²); 20 km is 1,257 km², four times that.
2. **The edge tapers.** Land falls away to deep ocean before `r = 1`, and there is a hard terminator past it. No invisible wall.
3. **Every biome appears in every seed.** Not probabilistically — by construction, verified by an automated test over N seeds.
4. **Roughly equal measure per biome.** Each biome's share of land area falls within a stated tolerance of its quota, every seed.
5. **Calm near the centre, hostile at the rim.** Biome placement is ordered by distance from world centre, with enough noise that it never reads as concentric rings.
6. **Volumetric and destructible.** Caves and overhangs exist; the player can dig. This rules out a pure heightmap representation, though most of the world is locally 2.5D and that fact is exploitable.

**Soft goals.** Deterministic from a 64-bit seed on any machine. World generation parameters tunable without a recompile during prototyping. Generation cheap enough to run per-chunk on demand rather than precomputed.

**Non-goals for this document.** The gameplay loop, progression and the concrete biome list — all of which are expected to churn. The generator is therefore written against an abstract biome *contract* (see below) and knows nothing about what a biome means. Networking, persistence format and art direction are also out of scope here.

**Known tension.** Requirements 4 and 5 fight each other if handled naively, because disc area grows as r². This is addressed in the area budget section and is the single most likely thing to be got wrong.

## Generation pipeline overview

The expensive, global reasoning happens once per seed on the CPU and produces a small buffer. Everything after that is a pure function of position, evaluated per sample on the GPU, with the edit overlay as the only state in the system.

```mermaid
%%{init: {'flowchart': {'wrappingWidth': 340}}}%%
flowchart TB
  subgraph INIT["World init · CPU"]
    direction LR
    C["<b>Region cells</b><br/>blue-noise scatter<br/>warped Voronoi"]
    Q["<b>Quota assignment</b><br/>sort cells by danger<br/>fill per-biome quotas"]
    RV["<b>River tracing</b><br/>steepest descent to ocean<br/>coarse grid"]
    C --> Q --> RV
  end
  subgraph FIELD["Per sample · GPU"]
    direction LR
    L0["<b>Continent mask</b><br/>fBm, centre bias<br/>edge taper"]
    L1["<b>Danger field</b><br/>normalised area<br/>plus noise wobble"]
    L4["<b>Height blend</b><br/>per-biome profiles<br/>over a shared base"]
    L6["<b>3D density</b><br/>overhangs and<br/>the cave field"]
    L0 --> L1 --> L4 --> L6
  end
  E["<b>Edit overlay</b><br/>player edits only, absolute density"]
  M["<b>Edits override the field function</b>"]
  SN["<b>GPU surface nets</b><br/>one dispatch per dirty chunk<br/>emits a 2.5D classification bit"]
  R["<b>High-resolution mesh</b><br/>to the graphics pipeline<br/>chunk-local 16-bit vertices"]
  P["<b>Low-resolution mesh</b><br/>async readback, then Jolt<br/>HeightField, or Mesh if edited"]
  INIT -- "cell, biome and river buffer" --> FIELD
  FIELD -- "field(p)" --> M
  E --> M
  M --> SN
  SN --> R
  SN --> P
```

The layers are specified individually below. Note where the two phases meet: the CPU half decides *which biome goes where*, the GPU half decides *what the ground looks like there*, and they communicate through one read-only buffer of a few thousand records. That boundary is what keeps the per-sample cost low enough for a world that is never stored.

## Layer specification

Seven layers, each independently tunable and independently testable. Layers 0–2 are evaluated per-sample in the shader; layer 3 is a CPU preprocess at world init; layers 4–6 are per-sample again.

### Layer 0 — continent mask and edge taper

Normalise `r = length(p.xz) / R`, then:

$$
h_{base} = \mathrm{fbm}(p_{xz} \cdot f_c) + A \cdot \bigl(1 - \mathrm{smoothstep}(0,\,0.25,\,r)\bigr) - B \cdot \mathrm{smoothstep}(0.80,\,1.0,\,r)
$$

The fBm term is 4–6 octaves with a base feature size of 2–8 km. The second term guarantees a home landmass at the centre regardless of seed. The third is the taper. Past `r = 1.0`, subtract a large constant so terrain drops away into void — the Valheim world-edge waterfall, which is the cheapest honest terminator.

Use OpenSimplex2 or simplex-family noise, not classic Perlin. Perlin's axis-aligned gradient artifacts are invisible at 10 m and glaring at 5 km, which is exactly the scale this layer works at.

### Layer 1 — danger field

`D(p) = saturate(r + w · fbm_low(p.xz))`, with `w` around 0.15–0.25 and a feature size of 3–6 km.

This is the "distance from centre" axis, softened. The wobble buys an occasional hostile pocket within sight of spawn and a calm valley at the rim — the thing that stops a radially-ordered world from reading as a dartboard. `w` is the single most important feel parameter in the generator; expose it prominently in the prototype.

### Layer 2 — region cells

Poisson-disc or blue-noise scatter of `N` points across the disc, giving a Voronoi partition. Boundaries are then domain-warped by 1–2 km of low-frequency noise, so cells read as organic coastlines and treelines rather than straight bisectors.

At runtime this is a buffer of `N` cell records plus a coarse uniform bucket grid (say 64×64 over the world bounds) so a lookup touches a handful of candidates. Cheap enough to evaluate per voxel.

### Layer 3 — quota assignment (CPU, world init)

This is the layer that makes the hard requirements true. It runs once per world, in well under a millisecond, and uploads its result as a small buffer.

```
land_cells  = cells where layer-0 height > sea level
for each biome b:  quota[b] = b.area_fraction * total_land_area

sort land_cells ascending by D(centroid)

for cell in land_cells:
    candidates = biomes where D(cell) within b.danger_band and quota[b] > 0
    if candidates empty: candidates = biomes where quota[b] > 0   # relax
    pick b minimising  |D(cell) - b.danger_preferred|
                     + adjacency_penalty(b, already-assigned neighbours)
    assign cell -> b;  quota[b] -= cell.area
```

Sorting by danger before assigning is what produces the radial ordering; the quota decrement is what produces equal measure; the relaxation branch is what guarantees presence even on a pathological seed. The adjacency penalty is where rules like "never place the hardest biome adjacent to the spawn cell" live, as a cost rather than a hard constraint, so the solver can never deadlock.

Cells left over in ocean become island clusters, drawn from the same quota pool. A biome occasionally landing island-only is free seed variety at no cost.

### Layer 4 — height composition

Each biome supplies a height profile: base elevation, amplitude, octave count, and a warp between ridged and billowy. Final height is a weighted blend of the neighbouring cells' profiles, weighted by smoothed distance to the cell boundary.

**Blend the resulting heights over a shared continental base — do not blend the noise parameters.** Interpolating octave counts and lacunarity produces mush at every boundary, and the boundary is exactly where the player is looking.

### Layer 5 — rivers

After the heightfield is defined, trace steepest-descent paths from high cells to the ocean on a coarse grid (256² over the world is ample), then carve the result with a distance-to-polyline SDF subtracted from the height.

Noise-faked rivers are cheaper and always read as fake once the player has seen real drainage. The polyline set is small, computed once at world init, and lives in the same buffer as the cells.

### Layer 6 — 3D density

```c
density(p) = height(p.xz) - p.y
           + biome.overhang_strength * ridged3d(p * f_o)
           - cave_field(p)
```

`cave_field` is a gyroid or worley-based tunnel network, masked by biome and by depth so caves do not open onto cliff faces at random. Surface nets consumes `density` directly; the sign change is the surface.

The critical property for LOD: this function must be *filterable*. Octave count has to fall with voxel size, or distant chunks will disagree with near chunks and pop on LOD transition. See the meshing section.

## Area budget and the equal-measure problem

Disc area grows as r², so radial bands of equal *width* have wildly unequal *area*. With five linear bands the innermost gets 4% of the world and the outermost 36% — a ninefold difference, and the calm starting biomes are the ones that lose.

The fix is equal-area rings: `r_k = R · √(k / N)`.

| Ring, inner → outer | Linear `r_k/R` | Linear area share | Equal-area `r_k/R` | Equal-area share |
| --- | --- | --- | --- | --- |
| 1 | 0.20 | 4% | 0.447 | 20% |
| 2 | 0.40 | 12% | 0.632 | 20% |
| 3 | 0.60 | 20% | 0.775 | 20% |
| 4 | 0.80 | 28% | 0.894 | 20% |
| 5 | 1.00 | 36% | 1.000 | 20% |

The same applies to the danger field: `D` should be defined on normalised area, not normalised radius, or every quota the assignment layer computes will be fighting the geometry. In practice that means `D ∝ r²` before the noise is added.

**Ocean is a separate budget line.** Quotas are fractions of *land* area, not world area, with ocean taking a flat 40–50% of the disc off the top. Mixing them means a seed with more coastline silently shrinks every biome.

### Scale and cell count

Quota accuracy is limited by cell granularity: you cannot assign half a cell. Size the cell count so each biome receives enough cells for the tolerance you want, then recover large contiguous patches with a **contiguity bonus** in the assignment cost — a negative penalty for matching an already-assigned neighbour. Many small cells give fine quota control; the bonus clumps them into patches of three to six. Patch size and quota granularity are then independent knobs, which is the whole trick.

Rule of thumb: `cells ≈ 15 × biome_count / land_fraction`, then check the implied cell diameter lands between 1 km and 2.5 km.

| Metric | R = 10 km | R = 20 km |
| --- | --- | --- |
| Disc area | 314 km² | 1,257 km² |
| Land at 55% | 173 km² | 691 km² |
| Per biome at 9 biomes | 19 km² | 77 km² |
| Region cells, total | \~245 | \~960 |
| Mean cell area / width | 1.3 km² / 1.3 km | 1.3 km² / 1.3 km |
| Land cells per biome | \~15 | \~59 |
| Quota granularity | ±7% | ±2% |
| Typical patch, 3–6 cells | 4–8 km² | 4–8 km² |

Holding cell size constant across both radii keeps biome patches the same size on the ground, which is what the player actually experiences; only the count changes. A thousand cells in a bucket-grid lookup is nothing on the GPU.

**On choosing R.** 1,257 km² is a great deal of ground to fill. Unless content density is high or there is fast travel, 20 km will read as empty where 10 km reads as dense. Build at 10, keep `R` a parameter, and decide once there is something to do in the world.

## Biome archetype contract

The generator knows nothing about what a biome *is*. It consumes a table of archetype records, and the biome list is data — adding, removing or reordering biomes is a data edit, never a code change. Since the biome set is expected to churn as gameplay develops, this boundary is the most valuable structural decision in the document after "edits only in the DAG".

```c
struct BiomeArchetype {
    // --- placement ---
    float  area_fraction;      // share of LAND area; all entries sum to 1
    float2 danger_band;        // [min, max] of D this biome may occupy
    float  danger_preferred;   // target D; drives the assignment cost
    uint   adjacency_mask;     // bitmask of biomes this one must not border
    bool   allow_island;       // may be placed on an ocean cell cluster

    // --- height profile (layer 4) ---
    float  base_elevation;     // metres above sea level at the cell centre
    float  amplitude;          // metres of relief
    uint   octaves;
    float  lacunarity, gain;
    float  ridge_blend;        // 0 = billowy, 1 = ridged
    float  flatness;           // post-shaping power curve; plains vs peaks

    // --- volumetric (layer 6) ---
    float  overhang_strength;
    float  cave_density;
    float2 cave_depth_band;    // metres below surface

    // --- surface ---
    uint   material_id;        // index into the material/texture array
    float  water_level_offset; // swamps sit at +0, lakes carve below
};
```

**Invariants the loader must assert at startup**, not at runtime:

- `area_fraction` sums to 1.0 within 1e-4.
- Every biome's `danger_band` is non-empty and `danger_preferred` lies inside it.
- The union of all `danger_band` intervals covers \[0, 1\] with no gap — otherwise some cells have no legal candidate and fall through to the relaxation branch, which silently degrades the radial ordering.
- `adjacency_mask` is symmetric, and the graph it forbids is not so dense that assignment is over-constrained. A cheap check: no biome forbids more than half the others.

**Deliberately absent from the contract:** spawn tables, loot, mob lists, ambient audio, weather, fog colour. Those hang off `material_id` or a parallel presentation table, keyed by biome id but owned by gameplay code. The generator must not grow a dependency on them, or the gameplay churn you are expecting will reach into terrain generation.

**Author it as data on disk** — JSON or TOML, hot-reloaded in the browser prototype, baked into a buffer at build time for the engine. The prototype's whole value is iterating on these numbers without a rebuild.

## Determinism, seeding and generator versioning

When the base terrain is a pure function and the save file holds only deltas against it, the function *is* the save file. Any change to it silently corrupts every existing world. This section is cheap to implement now and very expensive to retrofit.

### Per-feature seed derivation

Never thread one RNG through the layers in order. Derive an independent seed per feature:

```c
seed_feature = hash64(world_seed ^ FEATURE_ID)
```

with `FEATURE_ID` a compile-time constant per layer (`CONTINENT`, `DANGER`, `CELLS`, `RIVERS`, `CAVES`, …). Retuning river carving then leaves the continent and biome layout byte-identical, which turns parameter iteration from "every change reshuffles the world" into "every change is a controlled experiment". This matters enormously during prototyping and is nearly free.

### Hash noise, not texture noise

Use an analytic integer hash — PCG or a small xxhash variant — rather than a permutation texture. Texture lookups differ in filtering and precision between the browser prototype, the compute shader and any CPU-side evaluation; integer hashes are bit-exact everywhere, and they cost less than a dependent texture fetch.

### CPU/GPU parity

**Prefer a single authority: the GPU generates, the CPU reads back.** If anything on the CPU must evaluate terrain independently — server-side validation, spawn placement, pathfinding — you now have two implementations of a float-heavy function that must agree, and they will not. Mitigations, in order of preference:

1. Don't. Read back what the GPU produced.
2. If you must, compile the same source for both and disable fast-math on the CPU build (`-ffp-contract=off`, no `/fp:fast`). Fused multiply-add is the usual culprit.
3. Accept divergence and only use the CPU path for decisions tolerant of a few centimetres.

### Generator versioning

Store `generator_version` in the world save. Pin it per world and never migrate silently.

The failure mode is specific: a player digs a tunnel, you retune cave density, they load the save and their tunnel now opens into solid rock or empty air, because the edits are voxel deltas anchored to a base that moved. There is no good automatic fix — remeshing deltas against new terrain is ill-posed.

Practical policy:

- Keep old generator versions compiled in behind a switch. They are a few kilobytes of shader each.
- Bump the version on *any* change to layer parameters or noise, not just structural changes.
- A hash of the full parameter block plus shader source, computed at build time, makes the bump automatic and un-forgettable.
- During prototyping, before there are saves worth keeping, this is a no-op. Build it anyway while it costs nothing.

## Runtime storage: the SVDAG holds edits, not terrain

The base terrain must never be stored. Not in the DAG, not as a heightmap, not on disk. The numbers settle it:

| Quantity, 0.5 m voxels | R = 10 km | R = 20 km |
| --- | --- | --- |
| World bounds | 20 × 20 km | 40 × 40 km |
| Surface columns | 1.6 × 10⁹ | 6.4 × 10⁹ |
| Heightmap alone, 2 bytes/sample | 3.2 GB | 12.8 GB |
| Surface voxels, \~1.5× columns | 2.4 × 10⁹ | \~10¹⁰ |
| Surface voxels at 1 bit — a hard floor | 300 MB | 1.25 GB |

That last row is a floor no compression beats, and real SVDAG nodes cost far more than a bit. Worse, SVDAG's compression comes from *repeated structure*: it is excellent on architectural and voxelised-CAD scenes and poor on noise-driven terrain, where leaf-level subtrees are nearly all unique. The dedup you would get is mostly from fully-empty and fully-solid subtrees, which is real but nowhere near enough.

**So: base terrain is regenerated on demand in the fill compute shader, and the SVDAG stores only player edits as a sparse overlay.**

This inverts the economics completely. An unedited world is an empty DAG. A dug tunnel is perhaps 10⁴–10⁵ voxels; even a heavily-played world is maybe 10⁷ edited voxels scattered through 10¹³ of volume, and every unedited subtree collapses to one shared node. Save files go from "impossible" to kilobytes.

### Overlay semantics

Each edited leaf stores an **absolute** density and material, not a delta against the base function.

The reason is the versioning problem from the previous section. With deltas, any change to the generator moves every edited voxel. With absolute values, the excavation itself survives a generator change intact and only the seam where it meets regenerated terrain is wrong. That is the difference between a cosmetic artefact and an unloadable save.

Sampling is then: `density = edit_overlay.contains(p) ? edit_overlay.read(p) : field(p)`, with the containment test being the DAG descent, which bails out at the first empty node — usually immediately.

### Residency and streaming

The DAG lives in a virtualised structured buffer with a page table: fixed-size pages, resident set driven by player position, cold pages evicted to disk. Because the overlay is sparse, the resident working set is small and the page table rarely moves — most of the world has no pages at all.

The base-terrain side of streaming is simpler still: there is nothing to stream. A chunk entering range is dispatched to the fill shader, meshed, and that is the whole pipeline. No I/O, no decompression, no cache coherence between players.

### What this buys and what it costs

- **Buys:** no disk footprint for terrain, trivially parallel generation, instant world creation, tiny saves, no cache invalidation, and a world size limited by float precision rather than by storage.
- **Costs:** every chunk load pays full generation cost, so the field function's per-sample budget is a hard constraint rather than a soft one. Budget it explicitly — see the parameter table — and profile the field function as a first-class concern from the first week.

## Meshing, LOD and the physics handoff

### The per-chunk compute pass

One dispatch per dirty chunk, producing three outputs: a high-resolution vertex/index buffer for rendering, a low-resolution buffer for physics, and a one-word **classification flag** read back with the geometry.

The classification is the important addition. During meshing, test whether the chunk is locally 2.5D — exactly one sign change per column, no edit overlay present. That single bit decides which physics shape gets built, and on a typical world the large majority of chunks pass.

### Noise filtering and LOD

**Octave count must fall with voxel size.** A chunk meshed at 2 m voxels must not evaluate octaves whose period is below 4 m, or it samples detail it cannot represent, and the result disagrees with the same ground meshed at 0.5 m. The symptom is terrain that visibly changes shape as you approach it.

Drop octaves at the Nyquist limit for the LOD's voxel size, and **fade the last surviving octave's amplitude to zero across the transition** rather than cutting it. A hard cut pops; a fade is invisible and costs one multiply.

This makes the field function's LOD behaviour a correctness property, not a performance tweak. Test it: mesh the same region at every LOD and assert the heightfields agree within the coarser LOD's voxel size.

### Seams

Surface nets across an LOD boundary leaves cracks. In order of effort:

1. **Skirts** — extrude a downward collar at chunk borders. Ugly under grazing light, invisible most of the time, twenty minutes of work. Correct choice for the prototype.
2. **Boundary cells at the finer resolution** — the coarse chunk generates its border row at its neighbour's resolution. Clean, moderate complexity, the usual production answer for dual methods.
3. **Transvoxel-style transition cells** — designed for marching cubes; adaptable but it is real work.

Decide this after the pipeline runs end to end, not before.

### Async readback

Triple-buffered staging. Expect two to three frames of latency, so physics trails rendering by 35–50 ms at 60 Hz.

For static terrain that is harmless. The one case where it bites is **active digging**, where the player expects the hole to be walkable the instant it appears. Mitigation: on an edit, immediately disable collision within the edited volume's AABB and restore it when the new shape lands. The player falls into the hole slightly early rather than bouncing off geometry that no longer exists, which is the right failure direction.

### Jolt shape construction

| Chunk class | Shape | Notes |
| --- | --- | --- |
| 2.5D, unedited — most of the world | `HeightFieldShape` | Readback is a column of heights, not a vertex buffer. Build cost and query cost both far below a mesh. Needs a power-of-two sample grid, which a chunk already is. |
| Overhangs, caves, or any edit | `MeshShape` via `MeshShapeSettings` | Cook on the background pool. Tune `mMaxTrianglesPerLeaf`. |

Cook off the main thread, then swap on it:

- Build shapes on a worker pool, never on the main thread.
- Queue completed shapes and apply them in a batch at a fixed point in the frame, with a hard cap on swaps per frame — each `SetShape` touches the broadphase.
- Static terrain bodies take `EActivation::DontActivate`, and mass properties need no update.

### A caution on physics resolution

A lower-resolution physics mesh means the player visually stands above or below the ground they are colliding with. At half resolution over 0.5 m voxels the error is small; at quarter resolution it is not.

Use full-resolution physics for the chunks the player can actually touch — a small radius — and the low-resolution mesh only beyond it, where it serves projectiles and distant AI. This costs a little more readback near the player and removes a whole class of "I'm floating" bug reports.

## Numerical precision and world coordinates

A 20 km radius world is right at the edge of what float32 handles gracefully, and the symptoms (shimmer, z-fighting, jittering physics) are easy to misdiagnose as bugs elsewhere.

float32 has a 24-bit significand, so the spacing between representable values grows with magnitude:

| Coordinate magnitude | float32 ULP |
| --- | --- |
| 1,000 m | 0.06 mm |
| 10,000 m | 1.0 mm |
| 20,000 m | 2.0 mm |
| 40,000 m | 3.9 mm |

**Centre world coordinates on the origin** — ±R, not 0…2R. That alone halves the worst-case error for free.

### Rendering

Use camera-relative transforms: subtract the camera position on the CPU in double, upload the camera-relative model matrix, and let the shader work in small numbers. Without this, 2 mm of vertex jitter is amplified by the projection and shows as shimmer on distant geometry and as shadow acne.

### Mesh vertices

Store vertices **chunk-local**, quantised to 16-bit normalised over the chunk's extent. A 32-voxel chunk at 0.5 m spans 16 m, so 16-bit gives 0.24 mm of resolution — better than float32 at world scale, at half the bandwidth. The chunk origin goes in the draw constants as a double-derived offset. This sidesteps the precision problem and shrinks the vertex buffer in one move.

### Physics

Jolt supports a double-precision body-position mode (`JPH_DOUBLE_PRECISION`, `RVec3`). At 10 km float is adequate; at 20 km it is marginal, and the failure shows up as slow drift and jitter in resting contacts far from the origin.

Two options, pick one deliberately:

1. **Enable double precision in Jolt.** Simple, costs some memory and a little throughput, and removes the whole class of problem. Preferred unless profiling says otherwise.
2. **Origin rebasing.** Shift the world origin when the player crosses a threshold. Cheaper at runtime, but every system holding a world position must be notified, and the ones that are missed fail subtly and late.

### Field function

The generator itself is fine in float32 even at 20 km: with a 1 m finest octave there are still \~500 representable positions per lattice cell. No special handling needed — just do not compile it with fast-math, for the parity reasons in the determinism section.

## Validation harness and acceptance criteria

Requirements 3 and 4 say "every seed". That is a testable claim, and it should be tested automatically from the first week rather than checked by eye on whichever seed happened to be open.

The harness runs the generator headless over N seeds at coarse resolution — a 1024² sample grid over the disc is plenty — and asserts the following. Fast enough to run 200 seeds in CI on every commit touching the generator.

| Check | Assertion | Catches |
| --- | --- | --- |
| Presence | Every biome has ≥ 1 cell, all N seeds | Assignment deadlock; over-tight danger bands |
| Quota | Per-biome land share within ±10% of target | Granularity too coarse; bands not covering \[0,1\] |
| Radial order | Spearman correlation between a biome's `danger_preferred` and its cells' mean normalised area ≥ 0.8 | Danger field defined on radius instead of area; `w` too high |
| Land fraction | Within ±5% of target, all seeds | Continent mask drifting with parameter edits |
| Edge taper | No land above sea level past `r = 0.95`; terrain below void threshold at `r = 1.0` | Taper too weak for an unusual seed |
| Foot connectivity | Flood fill from spawn over walkable land reaches ≥ X% of land cells | Biomes stranded behind cliffs |
| Boat connectivity | Flood fill over land + navigable water reaches 100% of biomes | Unreachable islands |
| Slope budget | ≥ 60% of land under 30° | Amplitude or octave settings making a biome unwalkable |
| Spawn quality | Spawn point is flat, coastal, in the calmest biome, deterministic | Spawn logic regressions |
| River sanity | Every traced river is monotonically descending and terminates in ocean | Carving order bugs; endorheic basins |
| LOD agreement | Height at every LOD matches LOD 0 within that LOD's voxel size | Octaves not dropped at Nyquist |
| Determinism | Hash of the sample grid is stable across runs and platforms | Non-deterministic iteration order; fast-math |
| Throughput | Nanoseconds per field sample, tracked over time | Silent cost creep in the field function |

### Golden seeds

Pin a handful of seeds and store the hash of their sample grid as a checked-in golden file. Any change to the generator breaks them — deliberately.

This doubles as the generator-version mechanism from earlier: the gate is "golden hashes changed, so either revert or bump `generator_version`". It makes the version bump impossible to forget, which is the only way that discipline survives contact with a year of tuning.

### The contact sheet

Automated checks cannot tell you whether a world is *interesting*. Have CI emit a single PNG contact sheet of 64 seeds rendered as biome maps, attached as a build artifact.

Thirty seconds of looking at that image catches things no assertion will: every seed having the same silhouette, one biome always hugging the coast, rivers all running the same direction, the radial structure becoming visible. It is the cheapest quality tool in the project.

## Prototype plan

Two tracks, independent until a single small merge point. Track B is built against a **trivial analytic world** — a sine heightfield plus a sphere for overhangs — so that pipeline bugs are never confused with worldgen bugs, and so neither track waits on the other.

No dates here; velocity is unknown. The ordering and the exit criteria are the content.

### Track A — worldgen, browser / WebGPU

| # | Milestone | Exit criterion |
| --- | --- | --- |
| A1 | WebGPU scaffold, field harness, seed scrubber | Layer-0 height and the danger field render as a 2048² map; seeds scrub interactively |
| A2 | Region cells and quota assignment | Biome map renders; the area histogram sits on the quota targets |
| A3 | Headless validation harness | 200 seeds green on every check; contact sheet emitted |
| A4 | Height composition and rivers | Shaded relief reads as a world, not as noise |
| A5 | 3D density, caves, cross-section view | A vertical slice shows caves and overhangs where intended |
| A6 | Archetype hot-reload and tuning UI | Someone who is not the author can retune a biome |

### Track B — voxel pipeline, C++

| # | Milestone | Exit criterion |
| --- | --- | --- |
| B1 | App, swapchain, trivial analytic field | Something on screen |
| B2 | GPU surface nets, one chunk | Meshed sine terrain rendering |
| B3 | Chunk streaming, LOD, skirts | Flight over the trivial world holds target frame rate |
| B4 | Async readback, `HeightFieldShape`, character | A capsule walks on trivial terrain |
| B5 | `MeshShape` path, chunk classification | A capsule walks over an analytic overhang |
| B6 | SVDAG edit overlay, digging | Dig a hole; it persists and physics follows within a frame or two |
| B7 | Virtualised buffer, paging, save/load | Edits survive a restart |

### Merge

| # | Milestone | Exit criterion |
| --- | --- | --- |
| M1 | Port the field function WGSL → HLSL; swap out the trivial field | Walking the real generated world in the engine |
| M2 | Golden seeds and version pinning wired into the C++ build | Changing a generator parameter fails the build until the version is bumped |

### Keeping the merge small

M1 is the only coupling between the tracks, and its cost is decided months earlier by how the field function is written. Keep it cheap:

- Write the field function in a **restricted WGSL subset** that maps directly to HLSL. No WGSL-only idioms, no pointer gymnastics, plain functions over `vec`/`float` and a small struct set.
- Keep it in its own directory with no dependency on either host.
- All tuning lives in the archetype data file, not in the shader.
- Verify the port by hash: the same seed must produce the same sample-grid hash through both paths.

Done that way the port is a mechanical translation plus a hash comparison. Done badly it is a rewrite, and the browser prototype stops paying for itself.

## Repository structure: one project, not two

**One repository.** The deciding argument is that `worldgen/` is consumed verbatim by both tracks. A repository boundary through it is a *version* boundary, and a version boundary through the one artefact that must never drift is exactly the wrong place to put one. You would spend the project answering "which commit of worldgen is the engine pinned to, and does the validator agree?"

```
/
├─ worldgen/                 # the shared source of truth
│  ├─ shaders/
│  │  ├─ noise.wgsl          # hash noise, fbm, ridged
│  │  ├─ field.wgsl          # layers 0,1,4,5,6
│  │  ├─ cells.wgsl          # layer 2 lookup
│  │  └─ surface_nets.wgsl   # meshing kernel
│  ├─ assign/                # layer 3, portable C++, no host deps
│  ├─ data/archetypes.json   # the biome table
│  └─ golden/                # pinned seed hashes
├─ tools/
│  ├─ mapview/               # Track A: TS + WebGPU prototype
│  └─ validate/              # headless harness, CI entry point
├─ engine/                   # Track B: C++
│  ├─ svdag/                 # edit overlay, virtualised buffer, paging
│  ├─ meshing/               # dispatch, readback, classification
│  ├─ streaming/             # chunk residency, LOD
│  ├─ physics/               # Jolt integration, cook pool, shape swap
│  └─ render/
├─ third_party/              # Jolt and friends, as submodules
├─ docs/
├─ CLAUDE.md
└─ README.md
```

### Why one wins

- The shader source, the archetype data and the golden hashes are shared. One copy, one commit, atomically changed.
- One CI pipeline gates both. A shader edit can run the 200-seed validator *and* a C++ build in the same check.
- The WGSL → HLSL port lands as a single reviewable diff with both sides visible.
- Repo count is cheap to increase later and expensive to decrease. Starting merged keeps the option open.

### The honest costs

- Two toolchains in one tree — CMake and npm. Handled with path-filtered CI workflows so a renderer change doesn't run the web build.
- Everyone clones everything. Immaterial at this size; sparse checkout if it ever isn't.
- If `tools/mapview` ever ships publicly while the engine stays closed, you would have to split. Treat `worldgen/` as an internal library with a deliberate API surface and that split stays a `git filter-repo` away rather than a rewrite.

### Conventions worth setting on day one

- Path-filtered CI: `worldgen/**` runs the validator; `engine/**` runs the C++ build and tests; a change to either runs both.
- `worldgen/` depends on nothing in `engine/` or `tools/`. Enforce it with a lint rule, not with discipline.
- Golden hashes are checked in and reviewed like code. A diff to them must be deliberate.
- Submodules pinned by commit, updated in their own PRs.

## Risks and open questions

### Does the SVDAG earn its complexity?

This is the one place the stated architecture deserves a challenge. Once the DAG holds only edits, the thing it is storing is sparse, small, and mostly append-only — and a DAG's whole advantage is deduplicating *repeated structure* across a large volume.

A hash map of 16³ edited bricks would hold a heavily-played world in a few tens of megabytes, is perhaps a tenth the code, has no pointer-chasing descent in the sampling path, and serialises trivially. The DAG wins if edits become extremely numerous or highly repetitive — large regular excavations, player-built structures voxelised into the same field.

Recommendation: **build the brick hash map at B6, instrument it, and promote to a DAG only when measurements demand it.** The sampling interface is identical either way, so this is a contained swap rather than a rewrite. Deciding now, before there is a single edit to measure, is the expensive version of this choice.

### Ranked risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Field function too slow per sample | High — the whole no-storage bet depends on it | Budget ns/sample from week one; track it in CI. Fallback is a bounded LRU disk cache of generated chunks |
| Radial structure visible as rings | High — breaks the feel, hard to spot from one seed | Contact sheet every build; `w` and warp amplitude tuned against it |
| Readback latency during digging | Medium — feels bad, very visible | Disable collision in the edited AABB until the new shape lands |
| LOD octave fading insufficient | Medium — terrain morphs on approach | LOD-agreement assertion in the harness; analytic filtering if fading is not enough |
| Heightfield → mesh transition on first edit | Medium — a pop or a one-frame hole | Classify and cook the mesh before retiring the heightfield; swap in one step |
| Warp amplitude mismatched to cell size | Low — boundaries read as geometric | Keep warp amplitude near cell radius; visible immediately in the map view |
| WGSL/HLSL numerical divergence | Low, but silent | Integer hash noise; sample-grid hash compared across both paths in CI |
| 20 km world reads as empty | Low now, high later | Build at 10 km; keep R a parameter; revisit with real content density |

### Open questions

- **R = 10 km or 20 km?** Affects nothing structural, everything about feel. Deferred until there is content.
- **Voxel size — 0.5 m or 0.25 m?** Costs scale cubically in generation and quadratically in surface count. 0.5 m is the recommended default; 0.25 m should be justified by a specific gameplay need.
- **Vertical extent?** Chunk indexing and the DAG root both want a bounded Y. ±512 m is a reasonable starting guess but should be set deliberately.
- **Multiplayer?** If yes, determinism moves from convenience to correctness, and "GPU is the sole authority" stops working — a headless server has to generate terrain too. This is the single question whose answer most changes the architecture, and it is worth answering early even though gameplay is undecided.
- **Is open water traversable content or just a boundary?** Determines whether ocean depth, currents and navigable channels need generation rules at all.
- **Do caves need to be biome-aware?** Currently they are a per-biome parameter. If caves become a destination rather than a feature, they need their own placement layer.

## Appendix A — starter prompt

See [`docs/starter-prompt.md`](starter-prompt.md). Paste it into a fresh session in an empty repository; it bootstraps the tree and takes Track A to milestone A1.

## Appendix B — CLAUDE.md

See [`CLAUDE.md`](../CLAUDE.md) at the repository root. It holds the invariants and gotchas only; this document is the reference it points at.

## Appendix C — parameters and budgets

Starting points, not measurements. Every budget row is a number to instrument and then argue with; the point is to have one from day one so cost creep is visible.

### Spatial

| Parameter | Value | Note |
| --- | --- | --- |
| Voxel size at LOD 0 | 0.5 m | 0.25 m costs 8× in generation, 4× in surface count |
| Chunk | 32³ voxels = 16 m cube | Also the `HeightFieldShape` sample grid |
| Vertical extent | ±512 m | 64 chunks tall; bounds the chunk index and the DAG root |
| LOD levels | 8 | 0.5 m through 64 m, doubling |
| LOD 0 radius | 128 m | Each level doubles; level 7 reaches \~16 km |
| Field samples per chunk | 35,937 (33³) | One-voxel apron for normals and seams |

### Generation

| Parameter | Value | Note |
| --- | --- | --- |
| Field budget | ≤ 20 ns/sample | 0.7 ms per chunk serial-equivalent; the hard constraint of the no-storage design |
| Region cells, R = 10 km | \~245 | `15 × biome_count / land_fraction` |
| Region cells, R = 20 km | \~960 | Same cell size, more of them |
| Cell lookup grid | 64 × 64 buckets | A handful of candidates per sample |
| Boundary warp amplitude | \~1.3 km | Roughly one cell radius; the parameter that makes cells read as coastlines |
| Danger noise weight `w` | 0.15–0.25 | The most important feel parameter in the generator |
| Continent fBm | 4–6 octaves, 2–8 km base | Simplex family, never classic Perlin |
| Land fraction target | 55% | ±5% tolerance in the harness |
| Quota tolerance | ±10% relative | Granularity-limited; tighten by adding cells |

### Runtime

| Parameter | Value | Note |
| --- | --- | --- |
| Mesh dispatch budget | ≤ 0.25 ms GPU per chunk |  |
| Chunks remeshed per frame | ≤ 8 | Hard cap; queue the rest |
| Readback latency | 2–3 frames | 35–50 ms at 60 Hz |
| Staging ring | 3 buffers | Triple-buffered |
| Jolt cook budget | ≤ 1 ms per chunk, on a worker | Never on the main thread |
| `SetShape` swaps per frame | ≤ 4 | Each one touches the broadphase |
| Full-resolution physics radius | 64 m | Beyond it, the low-resolution mesh |
| Vertex format | 16-bit chunk-local position | 0.24 mm over a 16 m chunk; half the bandwidth of float32 |
| Resident chunk mesh memory | \~200 MB | \~4,000 chunks at \~48 KB; measure and revisit |

### Validation

| Parameter | Value |
| --- | --- |
| Seeds per CI run | 200 |
| Sample grid per seed | 1024² over the disc |
| Contact sheet | 64 seeds, one PNG, every build |
| Golden seeds | 8, hashes checked in |
