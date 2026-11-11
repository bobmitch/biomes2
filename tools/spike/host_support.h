// ---------------------------------------------------------------------------
// Host-side scaffolding for the spike. NOT the field function.
//
// The noise that was here has been promoted to worldgen/shaders/noise.h, which
// is the canonical dialect source (ADR-001). What remains is CPU-only and has
// no GPU counterpart by design:
//
//   - hash64 / feature_seed derive the per-feature seeds. WGSL has no 64-bit
//     integers, so this runs once on the host and the result is handed to the
//     shader as a u32 uniform. It is not part of the field function.
//   - Rng drives the layer-2 blue-noise scatter, which the design doc puts on
//     the CPU at world init.
// ---------------------------------------------------------------------------
#pragma once
#include "../../worldgen/shaders/noise.h"

#include <cstdint>

namespace spike {

// Per-feature seeds: hash64(world_seed ^ FEATURE_ID). Retuning one layer must
// not reshuffle any other layer.
enum FeatureId : uint32_t {
    FEATURE_CONTINENT = 1u,
    FEATURE_DANGER    = 2u,
    FEATURE_CELLS     = 3u,
    FEATURE_WARP_X    = 4u,
    FEATURE_WARP_Z    = 5u
};

inline uint64_t hash64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint32_t feature_seed(uint64_t world_seed, uint32_t feature_id) {
    return static_cast<uint32_t>(hash64(world_seed ^ static_cast<uint64_t>(feature_id)) >> 32);
}

// Deterministic RNG for the cell scatter. Host-only: layer 2 is a world-init
// preprocess, not a per-sample function.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t s) : state(hash64(s)) {}
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t xs  = static_cast<uint32_t>(((state >> 18) ^ state) >> 27);
        const uint32_t rot = static_cast<uint32_t>(state >> 59);
        return (xs >> rot) | (xs << ((32u - rot) & 31u));
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

}  // namespace spike
