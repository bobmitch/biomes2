// ---------------------------------------------------------------------------
// The generator's version: one number, declared once, in the thing it versions.
//
// A save stores edits as ABSOLUTE density against the base terrain (invariant
// 5), so a save means nothing against a world the generator no longer produces.
// This number is what says which world that was.
//
// Bumping it is the ONLY way to pin a different world. The build runs the
// golden seeds and fails when their hashes move; `validate --bless` refuses to
// re-pin moved hashes while this number is unchanged. So the sequence is
// forced: change the generator, the build goes red, and it stays red until you
// either revert or come here and say, deliberately, that the world is allowed
// to be different.
//
// It lives in a header of its own rather than in golden.json so that the two
// are independent: the version is what the generator CLAIMS to be, golden.json
// records what was actually measured and at which version. A gate needs both
// halves, and a file that holds only one of them cannot tell a deliberate bump
// from an accidental drift.
// ---------------------------------------------------------------------------
#ifndef WORLDGEN_GENERATOR_VERSION_H
#define WORLDGEN_GENERATOR_VERSION_H

namespace worldgen {

// History, so that a bump is never a mystery a year later. Taken from the
// commits that moved it, not from memory:
//   1  A3: validate becomes a real gate, and the world is pinned at all
//   2  A4 part one: layer 4 height composition, in metres
//   3  A4 part two: rivers as real drainage
//   4  pow removed from the field function -- not bit-reproducible
//   5  cos and sin removed from the cell scatter -- same reason, and the one
//      that moved three golden seeds in eight. Versions up to here had been
//      passing CI by luck; see invariant 9 in CLAUDE.md.
//   6  a 35% mean land fraction: the world becomes an archipelago
//   7  A5: 3D density, caves and overhangs
//   8  A7: cave entrances
//   9  continuous region-cell blend: weights now fall to zero as a cell ties
//      with the fifth nearest. The 4-cell blend had been cutting a live
//      weight wherever the 4th and 5th traded places -- straight-line steps
//      in the heightfield up to 63 m, and dark serrated overhang walls.
static const int kGeneratorVersion = 9;

}  // namespace worldgen

#endif  // WORLDGEN_GENERATOR_VERSION_H
