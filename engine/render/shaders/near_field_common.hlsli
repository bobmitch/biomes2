// The near field's layout (near_field.h has the C++ side): a camera-centred
// volume of the terrain as it is NOW -- the field and the player's edits --
// for light that the heightfield GI cannot see, under the top surface.
//
// Occupancy: 128 x 64 x 128 voxels of 1 m (mips to 8 m), toroidal: world
// voxel w lives at storage w mod N, so the volume scrolls with the camera by
// refilling 8 m blocks rather than moving anything. Voxel w is the field AT
// the integer point w.
// Probes: 64 x 32 x 64 cells of 2 m, the same toroidal rule; probe P sits at
// world 2P + 1. Each is five float4: sky visibility as L1 SH (the
// heightfield's convention, hfgi_common.hlsli), bounce radiance R, G, B as
// L1 SH, and (valid, the offset its rays left from, xyz).
#ifndef ENGINE_NEAR_FIELD_COMMON_HLSLI
#define ENGINE_NEAR_FIELD_COMMON_HLSLI

static const int3 kNfDims = int3(128, 64, 128);
static const int3 kNfProbeDims = int3(64, 32, 64);
static const int  kNfProbeFloat4s = 5;
static const int  kNfMips = 5;

int3 nf_wrap(int3 w, int3 n) {
    return ((w % n) + n) % n;
}

uint nf_probe_index(int3 p) {
    const int3 s = nf_wrap(p, kNfProbeDims);
    return uint((s.z * kNfProbeDims.y + s.y) * kNfProbeDims.x + s.x);
}

#endif
