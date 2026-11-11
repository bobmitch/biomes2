# ADR-002 — Drop WGSL; the viewer is native

Status: **accepted** · 2026-11-11 · supersedes decision 2 of
[ADR-001](adr-001-field-function-authority.md)

## Decision

1. **`tools/mapview` and `tools/wgslgen` are deleted.** The browser prototype
   and the WGSL generator both go, along with npm, Vite and the generated
   `.wgsl` files.
2. **The field function becomes ordinary C++** that still compiles as HLSL
   behind `worldgen/shaders/field_types.h`. The dialect restrictions — one
   statement per line, no structs, functional casts, a fixed builtin set —
   are retired with the generator that required them. **Structs come back.**
3. **A native viewer, `tools/view`,** links `worldgen/` directly.
4. ADR-001's decisions 1, 3 and 4 **stand unchanged**: `tools/validate` is
   native C++, the C++ build with contraction disabled is the determinism
   authority, and kernel-shaped code stays hand-written per backend.

## Why ADR-001 decided otherwise, and why that has expired

ADR-001 separated what the WGSL specification forces from what was a bet. The
forced part is untouched by this document. The bet was:

> That the field function can be expressed in a dialect restricted enough to
> generate WGSL with a small tool, while staying readable enough to be the
> canonical source a human edits.

That bet was taken **given a browser prototype**. Remove the prototype and
there is nothing left to generate WGSL *for*:

- `tools/mapview` was the only consumer of WGSL in the repository. `engine/`
  does not exist yet and will use HLSL.
- So the dependency chain ran: mapview → WGSL → `wgslgen` → the dialect →
  every restriction in invariant 2. Cutting the head removes the body.

Three things then tip it:

**The dialect was already straining.** ADR-001 predicted layer 4 would force
structs; it did not quite, but only because `layer4_profile` was allowed to
take eleven loose scalars, which the file itself flags as "at the edge of what
reads". Layer 6 is worse — caves carry a per-biome overhang strength, density
and depth band. Deciding now refactors layer 4 once; deciding at A5 refactors
it twice.

**The native viewer is strictly more trustworthy than mapview was.** It runs
the shipping C++ directly. mapview ran a *translation* of it, on a backend
whose specification permits reassociation and FMA fusion — so the thing being
tuned against was never quite the thing that ships. That inverts ADR-001's own
argument: the generator existed to keep one source of truth, and the native
viewer achieves that more completely by removing the second backend entirely.

**The port cost was always front-loaded onto the throwaway half.** ADR-001
noted the prototype "is the part with an end date". That date is now.

## What this does NOT touch

Stated explicitly because the determinism work and the WGSL question are easy
to conflate, and they are unrelated:

- **Golden hashes and `generator_version`.** Unchanged, and this change is
  required to be hash-neutral.
- **Invariant 9, no libm upstream of the hash.** The determinism bug it came
  from was `cos`/`sin` in the cell scatter, caught by MSVC against GCC, with
  no WGSL anywhere near it.
- **The two-OS CI matrix.** Still the only thing that catches that class.
- **The 200-seed harness and every check in it.**

## Consequences

- **Hash-neutral by requirement.** Moving code between files and replacing
  eleven scalars with a struct must not alter a single value. If the golden
  hashes move, that is a bug to find, not a version to bump.
- **Generator code moves into `worldgen/`.** The Bridson scatter (layer 2),
  `hash64`/`feature_seed` (invariant 4) and the world build currently live in
  `tools/validate`. Both the harness and the viewer need them, and the move
  also separates code that feeds the golden hash from metrics that legitimately
  use `atan` — which is what would make a forbidden-symbol check for invariant
  9 possible later.
- **The viewer is Windows-only**, being raw Win32. Accepted: CI is headless,
  so determinism coverage is unaffected, and the viewer is a debug tool rather
  than a deliverable. It is built to fold into the engine later as a hidden
  debug mode.
- **CLAUDE.md invariant 2 describes a dialect that no longer exists** and has
  to go with this change.
- The debug modes stay: `--components` found the determinism bug after two
  wrong guesses, `--closeup` revealed the staircase rivers, and the sweeps are
  the record of how the tuning was arrived at.

## Alternatives rejected

| Option | Why not |
| --- | --- |
| Keep mapview and the generator | Pays the dialect's whole cost — no structs, one statement per line — to serve a prototype whose job is finished, and tunes against a translation rather than the shipping code |
| Keep `wgslgen` against a future need | There is no consumer and no planned one. `engine/` is HLSL, which the field function already compiles as unmodified. Reviving it is a `git revert` away |
| Keep the dialect restrictions without the generator | Restrictions with no mechanism behind them are a style guide that will quietly rot, and they cost real expressiveness at layer 6 |
