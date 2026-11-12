// ---------------------------------------------------------------------------
// The streamed terrain as a collision surface.
//
// One height patch per resident chunk, arriving through the readback ring two
// frames after its chunk was meshed, and dropped when the chunk is retired. A
// query walks from the finest LOD outwards and takes the first level that has
// a patch covering the point, so a character standing near the camera collides
// against LOD 0 and one far away against whatever is loaded there.
//
// NOT a physics engine. The design document calls for Jolt and a real
// HeightFieldShape, and B5's overhangs need a MeshShape that a heightfield
// cannot express at all -- a column has one height, and the trivial field has
// a sphere floating over the ground. What this is, is the half of B4 that is
// this project's own: the path from a GPU field evaluation to something a
// character can stand on, with the latency and the residual measured. Swapping
// the query for Jolt is a contained change behind `sample`.
// ---------------------------------------------------------------------------
#ifndef ENGINE_HEIGHT_FIELD_H
#define ENGINE_HEIGHT_FIELD_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/streaming/streamer.h"

#include <unordered_map>
#include <vector>

namespace engine {

// Matches the kernel's sentinel for a column with no surface in it.
static const float kNoSurface = 1.0e29f;

class HeightField {
public:
    void insert(const ChunkKey& key, const float origin[3], float voxel, const float* h) {
        Patch& p = patches_[key];
        p.origin[0] = origin[0];
        p.origin[1] = origin[1];
        p.origin[2] = origin[2];
        p.voxel = voxel;
        bool any = false;
        for (UINT i = 0; i < kHeightSamples; ++i) {
            p.h[i] = h[i];
            if (h[i] < kNoSurface) any = true;
        }
        // A chunk of empty air carries no surface, and keeping it would make
        // every query walk a patch that can never answer.
        if (!any) patches_.erase(key);
    }

    void remove(const ChunkKey& key) { patches_.erase(key); }

    size_t patch_count() const { return patches_.size(); }

    // Ground height under (x, z). Returns false when nothing covering that
    // point has streamed in yet -- which a caller must handle rather than
    // treat as "no ground", or a character falls through terrain that simply
    // has not arrived.
    bool sample(float x, float z, float& out_y, int& out_lod) const {
        float band_lo = 0.0f, band_hi = 0.0f;
        trivial_band(band_lo, band_hi);

        for (int lod = 0; lod < kLodCount; ++lod) {
            const float S = lod_chunk_size(lod);
            ChunkKey key;
            key.lod = lod;
            key.x = int(std::floor(x / S));
            key.z = int(std::floor(z / S));
            const int j0 = int(std::floor(band_lo / S));
            const int j1 = int(std::floor(band_hi / S));

            // Highest first: the kernel resolves a column to its highest
            // surface, so the query has to agree or a character would be
            // placed under an overhang it should be standing on.
            for (int j = j1; j >= j0; --j) {
                key.y = j;
                std::unordered_map<ChunkKey, Patch, ChunkKeyHash>::const_iterator it =
                    patches_.find(key);
                if (it == patches_.end()) continue;
                float y = 0.0f;
                if (bilinear(it->second, x, z, y)) {
                    out_y = y;
                    out_lod = lod;
                    return true;
                }
            }
        }
        return false;
    }

private:
    struct Patch {
        float origin[3];
        float voxel;
        float h[kHeightSamples];
    };

    static bool bilinear(const Patch& p, float x, float z, float& out) {
        const float u = (x - p.origin[0]) / p.voxel;
        const float w = (z - p.origin[2]) / p.voxel;
        if (u < 0.0f || w < 0.0f) return false;
        const int i = int(u), k = int(w);
        if (i + 1 >= int(kHeightGrid) || k + 1 >= int(kHeightGrid)) return false;
        const float fu = u - float(i), fw = w - float(k);

        const float h00 = p.h[size_t(k) * kHeightGrid + i];
        const float h10 = p.h[size_t(k) * kHeightGrid + i + 1];
        const float h01 = p.h[size_t(k + 1) * kHeightGrid + i];
        const float h11 = p.h[size_t(k + 1) * kHeightGrid + i + 1];
        // A hole anywhere in the cell makes the whole cell unanswerable.
        // Interpolating against the sentinel would put the ground 10^29 m up.
        if (h00 >= kNoSurface || h10 >= kNoSurface ||
            h01 >= kNoSurface || h11 >= kNoSurface) {
            return false;
        }
        const float a = h00 + (h10 - h00) * fu;
        const float b = h01 + (h11 - h01) * fu;
        out = a + (b - a) * fw;
        return true;
    }

    std::unordered_map<ChunkKey, Patch, ChunkKeyHash> patches_;
};

}  // namespace engine

#endif  // ENGINE_HEIGHT_FIELD_H
