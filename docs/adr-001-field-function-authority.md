# ADR-001 — Field function authority and the validator's language

Status: **accepted** · 2026-11-10 · supersedes two decisions in
[`worldgen-design.md`](worldgen-design.md)

## Decision

1. **`tools/validate` is native C++.** `worldgen/assign/` links in directly.
   No Dawn, no headless WebGPU, no wasm on the CI path. emscripten stays a
   `tools/mapview` build dependency only.
2. **The field function's canonical source is a restricted C-like dialect**
   that compiles unmodified as C++ and as HLSL behind a small vector header
   ([`worldgen/shaders/field_types.h`](../worldgen/shaders/field_types.h)).
   WGSL is generated from it for `tools/mapview`.
3. **Golden hashes come from the C++ build** with `/fp:precise` and
   `-ffp-contract=off`. That build is the determinism authority. CPU-to-GPU
   agreement is a tolerance comparison, never a hash.
4. **This covers the field function only.** See the scope section.

This document separates what follows from the WGSL specification — which is
not negotiable and does not depend on anything we build — from the one place
we are taking a bet.

## Part 1 — What follows from the specification

### WGSL cannot be bit-reproducible

The design doc asks for a sample-grid hash "stable across runs and platforms",
pins golden seeds to checked-in hashes, and proposes verifying the port "by
hash: the same seed must produce the same sample-grid hash through both paths".

WGSL does not permit that. From [§15.7.5, *Reassociation and
Fusion*](https://www.w3.org/TR/WGSL/):

> An implementation may reassociate operations. […] An implementation may fuse
> operations if the transformed expression is at least as accurate as the
> original formulation.

with the explicit caveat that the result "may not be the same when computed in
floating point". The `fma` accuracy rule likewise permits an implementation to
emit an ordinary multiply followed by an ordinary add instead of a fused one.

Both are at the implementation's discretion, and there is no opt-out. **There
is no `-ffp-contract=off` for WGSL.** CLAUDE.md invariant 8 — never compile the
field function with fast-math, because FMA contraction breaks CPU/GPU parity —
is enforceable on a CPU compiler and simply not expressible on a GPU one.

A hash produced by running WGSL on a CI software rasteriser is therefore that
rasteriser's hash, not the player's GPU's, and pinning it as golden pins the
wrong thing. **The determinism authority has to be a CPU build with contraction
disabled.** A CPU implementation of the field function is mandatory, not
optional. Everything else here follows from that.

It also means any CPU↔GPU check is a tolerance comparison. Only CPU↔CPU can be
a hash. That is a property of the specification, not of our tooling, and no
choice of language or generator changes it.

### The three languages are not symmetric

```hlsl
float3 add(float3 a, float3 b) { return a + b; }        // HLSL
float3 add(float3 a, float3 b) { return a + b; }        // C++, given a float3 header
fn add(a: vec3<f32>, b: vec3<f32>) -> vec3<f32> { … }   // WGSL — different grammar
```

A restricted C-like dialect compiles unmodified as both HLSL and C++ behind a
small vector header. WGSL's grammar — `fn`, `: f32`, `let`/`var`, explicit
address spaces — rules that out. **WGSL is the one language of the three that
must be produced mechanically, whichever direction the arrow points.** The only
question is whether it is produced once by hand or repeatedly by a tool.

### Nothing translates WGSL to CPU code

Neither naga nor Tint has a C++ or CPU backend; their writers target SPIR-V,
MSL, HLSL, GLSL and DXIL. Obtaining CPU evaluation from canonical WGSL means
writing an emitter over naga's IR — building a compiler to serve the half of
the project that has an end date.

### Headless WebGPU in Node works, but buys the wrong thing

It is feasible: Dawn drives it, Mesa/lavapipe supplies a software adapter in a
container. But every Node binding is community-maintained rather than an
official Dawn release, at least one states it has not passed the WebGPU
conformance suite, and CI would then carry Dawn, a software rasteriser, a Node
toolchain and emscripten for the `assign` wasm — to compute a number the first
section says is the wrong number.

**Conclusion of Part 1: decisions 1 and 3 are forced.** Native C++ validator,
golden hashes from the C++ build, tolerance comparison across the GPU boundary.
None of this depends on the generator.

## Part 2 — The bet

Decision 2 — canonical dialect, WGSL generated — is **a bet, and is marked as
one.** What is being bet:

> That the field function can be expressed in a dialect restricted enough to
> generate WGSL with a small tool, while staying readable enough to be the
> canonical source a human edits.

The alternative is one hand-written WGSL port kept in sync by review. That
alternative is viable; it is not absurd; and if the dialect ever starts
distorting the source to keep the generator happy, taking it is the correct
move rather than a defeat.

**Budget, set before the pilot:** if the generator passes ~400 lines, or the
dialect needs constructs that make the C++/HLSL dual-compile awkward, fall back
to a single hand-port plus a CPU/GPU tolerance test in CI. Failing the budget
changes *how* WGSL is produced, not which language is canonical — decisions 1,
3 and 4 stand either way.

### Pilot result: the bet holds, for now

`tools/spike/noise_standin.h` — a working C++ simplex and fBm — was expressed
in the dialect as [`worldgen/shaders/noise.h`](../worldgen/shaders/noise.h) and
`tools/wgslgen` emits the WGSL.

| | |
| --- | --- |
| Generator size | **253 non-blank lines, 279 total** — inside the ~400 budget |
| Dialect distortions needed | one: arrays are declared bare and filled by assignment rather than brace-initialised |
| Spike, 200 seeds at 1024² | identical to every digit: 3.9% / 6.8% / 10.3%, Spearman 1.00, patch 4.4 cells, 1 seed failing |
| Adjacency test | still 0 violations, 96/96 |

The dual-compile was not awkward. The one constraint worth recording is that
`field_types.h` brings in `floor`, `sqrt` and `clamp` with `using` declarations
rather than defining them — the standard library already declares float
overloads in the global namespace and a second definition is a hard error.

**Honest limit of this pilot:** the generated WGSL has been checked
structurally — balanced braces and parens, every `fn` carries a return type, no
C-isms survive — but it has **not** been parsed by a real WGSL frontend or
executed. The C++ half is proven bit-identical; the WGSL half is plausible and
unverified. Wiring naga or Tint over `build/noise.wgsl` is the first thing
`tools/mapview` should do, and until then the generator's output is an
untested artifact.

## Scope — field function only

**This ADR covers the field function and nothing else.** `surface_nets.wgsl`
and anything else kernel-shaped — workgroup memory, atomics, storage buffers,
barriers, dispatch-dimension logic — stays **hand-written per backend** and is
explicitly out of scope.

The reason is the one that drives the whole document: kernel code never needs
CPU evaluation. It is not the determinism authority, it produces no golden
hash, and nothing on the validator's path has to run it. The dialect exists to
buy CPU evaluation, so extending it to code that does not need CPU evaluation
would be paying its cost for nothing — and the constructs involved (address
spaces, barriers, atomics) are exactly the ones that would blow the generator
budget.

So: a WGSL compute kernel and an HLSL compute kernel, written and reviewed as a
pair. The field function they both call is generated from the one dialect
source.

## Alternatives rejected

| Option | Why not |
| --- | --- |
| Node + headless WebGPU, WGSL canonical | Most moving parts in CI, and the hash it produces is a software rasteriser's rather than a reproducible authority (Part 1) |
| WGSL canonical, emit C++ from naga IR | No such backend exists; means writing a compiler to serve the throwaway half of the project |
| Hand-write the C++ and the WGSL, compare them in CI | **Not** because §15.7.5 blocks hash comparison — that applies equally to the generated case, so it is not a discriminator. The reason is that generation makes a single source *structural*: the WGSL is a build product and cannot drift, by construction. Two hand-written copies can drift, and because the only available cross-backend check is a tolerance comparison, drift smaller than the tolerance accumulates invisibly and indefinitely. Generation guarantees one source; it does not guarantee hash equality, and nothing can |

## Consequences

- **The spike is the validator.** `tools/spike` already does 200 seeds at
  1024² in about 11 seconds, native, threaded, with `assign` linked in.
  Connectivity, slope budget, LOD agreement and golden hashes are growth of
  what exists, not a new program in a new language.
- **`noise_standin.h` is gone.** Its noise was promoted to the canonical
  dialect source; what remained is CPU-only scaffolding (64-bit seed
  derivation, which WGSL cannot express, and the scatter RNG) and now lives in
  `tools/spike/host_support.h`. There is no second implementation of the field
  function and never was one for longer than this change.
- **Generated WGSL is a build product**, written to `build/`. It is not checked
  in and must never be hand-edited.
- **Golden hashes become meaningful.** They pin the C++ build's output, the one
  thing in the system that can be held stable, so the generator-version gate
  hangs off something reproducible.
- **The multiplayer open question gets cheaper.** The design doc calls it "the
  single question whose answer most changes the architecture", because a
  headless server would have to generate terrain without a GPU. It already can.
- **Two design doc statements are corrected** by this ADR: the decision table's
  field-function language row, and milestone M1's "verify the port by hash".

## Open

- Validate the generated WGSL with naga or Tint as part of the `mapview` build.
  Until that exists the generator is unverified on its output side.
- Does the dialect stay small as layers 4–6 arrive? Height composition and the
  cave field are the next test of the budget. Re-measure then; the fallback
  stays available and taking it is not a reversal of decisions 1, 3 or 4.
