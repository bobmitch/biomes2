// ---------------------------------------------------------------------------
// The trivial analytic field Track B is built against.
//
// The design document is explicit about why this exists: the voxel pipeline is
// developed against a sine heightfield plus a sphere so that pipeline bugs are
// never confused with worldgen bugs, and so neither track waits on the other.
// M1 swaps it for the real generator and this file is deleted.
//
// Written in the ADR-001 dialect -- ordinary C++ that also compiles as HLSL --
// even though nothing on the C++ side evaluates it yet. That is deliberate
// rehearsal. ADR-001's central claim is that "HLSL is the same text", and
// until something compiled a dialect header as HLSL, that claim had never been
// tested. Better to find out at B1 than at the merge point.
//
// NOT subject to invariant 9. This field never reaches a golden hash, so it is
// free to use sin, and the real field function is not -- see the note on
// `using std::sin` below. If this file ever stops being throwaway, that is the
// first thing to revisit.
// ---------------------------------------------------------------------------
#ifndef ENGINE_TRIVIAL_FIELD_H
#define ENGINE_TRIVIAL_FIELD_H

#include "worldgen/shaders/field_types.h"

#ifdef __cplusplus
// Deliberately NOT added to field_types.h. That header's builtin list is the
// enforcement point for invariant 9, and widening it to suit scaffolding would
// quietly license sin in the hashed generator, where one ULP moves a river.
// Scoped to this file instead, where the dialect's rules do not apply.
using std::sin;
#endif

// Rolling ground. Two frequencies so the surface is not a single wave and a
// camera moving over it has something to resolve against.
WG_FN float trivial_height(float x, float z) {
    return 9.0f * sin(x * 0.075f) * sin(z * 0.061f)
         + 3.0f * sin(x * 0.21f + z * 0.17f);
}

// density > 0 is rock, < 0 is air, and the sign change is the surface -- the
// same convention layer 6 uses, so surface nets does not care which field it
// is fed.
WG_FN float trivial_density(float x, float y, float z) {
    const float ground = trivial_height(x, z) - y;

    // A sphere of rock floating clear of the ground, so there is a genuine
    // overhang to walk under at B5 before the real generator arrives. Union of
    // two solids is a max; spelled as a comparison because the dialect's
    // builtin list has no max and does not need one.
    const float sx = x - 14.0f;
    const float sy = y - 26.0f;
    const float sz = z + 10.0f;
    const float sphere = 13.0f - sqrt(sx * sx + sy * sy + sz * sz);

    return ground > sphere ? ground : sphere;
}

#endif  // ENGINE_TRIVIAL_FIELD_H
