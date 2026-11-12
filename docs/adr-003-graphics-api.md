# ADR-003 — Direct3D 12 is the engine's graphics API

**Status:** accepted
**Date:** 2026-11-12
**Supersedes:** nothing. Extends [ADR-001](adr-001-field-function-authority.md) and [ADR-002](adr-002-drop-wgsl-native-viewer.md).

## Context

The design document fixes the shader language — the field function is a
restricted C-like dialect that compiles unmodified as C++ and as HLSL — but it
never names a graphics API. Track B starts at B1 and every milestone after it
inherits whatever B1 picks, so the choice has to be made now rather than
discovered at B4.

Three candidates were considered: D3D11, D3D12 and Vulkan.

## Decision

**Direct3D 12.**

## Why

The runtime budget table in the design document is already a D3D12 design,
whether or not it was written as one:

| Budget | What it implies |
| --- | --- |
| Mesh dispatch ≤ 0.25 ms GPU per chunk | Measurable per-dispatch GPU timing |
| Chunks remeshed per frame ≤ 8, queue the rest | An explicit compute queue the app schedules |
| Readback latency 2–3 frames | Fences the app waits on by value, not by flush |
| Staging ring, 3 buffers | Application-owned buffer lifetime across frames |

D3D11 can express all of this, but only by fighting its own abstractions. Its
immediate context hides the sync the streaming design wants to control, and
`Map` with `DO_NOT_WAIT` plus query-based readback is a worse version of
fences. The place it runs out is B3–B4, which is exactly the point at which a
port is most expensive — the code is written, nothing is measured yet, and the
symptoms (stalls, jitter) are easy to misread as streaming bugs.

Vulkan is a real alternative and loses on a narrower point: the project is
Windows-only today, the viewer is already raw Win32, and HLSL is the chosen
shader language either way. Vulkan would mean HLSL → SPIR-V through DXC and a
much larger bring-up for a portability benefit nothing is currently asking
for. If the project ever wants Linux or Proton as a first-class target, that
is a deliberate second backend, and the compute kernels were always going to
be hand-written per backend (ADR-001 excludes kernel code from the dialect).

The cost is honest: B1 is a few hundred lines of scaffolding rather than a few
dozen. That scaffolding is not throwaway — descriptor heaps, fences and
command list recycling are what B2 through B7 are built on.

## What this does not decide

- **Shader compilation.** B1 uses the in-box `D3DCompile` at Shader Model 5.1,
  which D3D12 accepts. `dxcapi.h` is present in the SDK and SM 6.x is
  available whenever a kernel needs wave intrinsics. That is a per-shader
  choice, not an architectural one.
- **The field function's authority.** Unchanged by this. ADR-001 stands: the
  C++ build with contraction disabled is the determinism authority, and
  CPU-to-GPU agreement is a tolerance comparison and never a hash. Nothing
  about D3D12 makes a GPU able to anchor a golden hash.

## Consequences

- `engine/` is Windows-only, like `tools/view/`. CI stays headless and gates
  `worldgen/` on both Linux and Windows, so determinism coverage does not
  depend on anything in this ADR.
- The trivial analytic field that Track B is built against is written in the
  ADR-001 dialect and included by the HLSL shader directly. That is deliberate
  rehearsal: M1 swaps the trivial field for the real one, and the include
  mechanism it depends on is exercised from B1 rather than first tried at the
  merge point.
