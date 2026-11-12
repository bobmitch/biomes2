// ---------------------------------------------------------------------------
// Chunk residency and LOD.
//
// Nested levels, each a fixed box of chunk indices centred on the camera's
// chunk at that level. A level-L chunk is dropped when its world box lies
// entirely inside the level-(L-1) box, so exactly one level covers any point.
//
// That containment test is done in WORLD-BOX space rather than by comparing
// distances to radii, and that is the whole reason this scheme is written the
// way it is. Classifying chunks by "is my centre inside radius R" looks
// simpler and is wrong at the corners: a coarse chunk straddling the boundary
// is kept while the fine chunks inside it are kept too, and the two levels
// draw the same ground twice. Box containment cannot do that.
//
// What it does NOT solve is the seam. Adjacent chunks at different levels have
// vertices on different grids, so the surfaces do not meet exactly and a thin
// crack of sky shows through. Skirts cover it -- see the skirt pass.
// ---------------------------------------------------------------------------
#ifndef ENGINE_STREAMER_H
#define ENGINE_STREAMER_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/field/trivial_field.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine {

// Five levels of 16, 32, 64, 128 and 256 m chunks. The design specifies eight,
// reaching 16 km; five reaches 1.8 km, which is as far as the trivial field is
// worth looking and keeps the resident set inside one slot pool. Adding levels
// is changing a constant, not the scheme.
static const int kLodCount = 5;

// Half-extent in chunks at every level: 7 x 7 columns per level.
static const int kRingHalf = 3;

static const float kVoxel0 = 0.5f;

inline float lod_voxel(int lod) { return kVoxel0 * float(1 << lod); }
// The OWNED span, which is what tiles. The ghost layer overlaps the
// neighbour and must not be counted here, or every chunk would be placed
// one voxel too far apart and the cracks would come back wider.
inline float lod_chunk_size(int lod) { return lod_voxel(lod) * float(kChunkOwnedCells); }

struct ChunkKey {
    int x, y, z, lod;
    bool operator==(const ChunkKey& o) const {
        return x == o.x && y == o.y && z == o.z && lod == o.lod;
    }
};

struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const {
        uint64_t h = 1469598103934665603ull;
        const int v[4] = {k.x, k.y, k.z, k.lod};
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<uint64_t>(static_cast<uint32_t>(v[i]));
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

class Streamer {
public:
    void init() {
        free_slots_.clear();
        free_slots_.reserve(kSlots);
        // Handed out from the back, so the first chunks take slot 0 upward and
        // a capture of slot 0 is the nearest chunk rather than an arbitrary one.
        for (int i = int(kSlots) - 1; i >= 0; --i) free_slots_.push_back(UINT(i));
        resident_.clear();
        queue_.clear();
        retire_.clear();
    }

    // Recompute what should be resident. Fills the queue with what is missing
    // and the retire list with what is no longer wanted.
    void update(float cx, float cy, float cz) {
        desired_.clear();

        float band_lo = 0.0f, band_hi = 0.0f;
        trivial_band(band_lo, band_hi);

        // Each level's world box, so the next level out can test containment.
        float fine_x0 = 0, fine_x1 = 0, fine_z0 = 0, fine_z1 = 0;
        bool have_fine = false;

        for (int lod = 0; lod < kLodCount; ++lod) {
            const float S = lod_chunk_size(lod);
            const int ci = int(std::floor(cx / S));
            const int ck = int(std::floor(cz / S));
            const int j0 = int(std::floor(band_lo / S));
            const int j1 = int(std::floor(band_hi / S));

            const float bx0 = float(ci - kRingHalf) * S;
            const float bx1 = float(ci + kRingHalf + 1) * S;
            const float bz0 = float(ck - kRingHalf) * S;
            const float bz1 = float(ck + kRingHalf + 1) * S;



            for (int k = ck - kRingHalf; k <= ck + kRingHalf; ++k) {
                for (int i = ci - kRingHalf; i <= ci + kRingHalf; ++i) {
                    for (int j = j0; j <= j1; ++j) {
                        const float x0 = float(i) * S, x1 = x0 + S;
                        const float y0 = float(j) * S, y1 = y0 + S;
                        const float z0 = float(k) * S, z1 = z0 + S;

                        // No surface can pass through this chunk at all.
                        if (y1 < band_lo || y0 > band_hi) continue;

                        // Entirely covered by the finer level: it is drawn
                        // there, at higher detail, and drawing it here too
                        // would double the ground.
                        // Containment is tested in XZ ONLY, and that is not a
                        // simplification -- testing y as well is wrong.
                        //
                        // Every level spans the same surface band, so a level
                        // that covers a column in XZ covers all of it. But the
                        // levels' y boxes are quantised to their own chunk
                        // size, so a 256 m chunk reaches from -256 to 0 where
                        // the 128 m level only reaches -128. Including y in
                        // the test therefore never excludes the coarse chunks
                        // that hang below the band, and they stay resident
                        // directly underneath the fine ones.
                        //
                        // Rendering hides that: the depth test draws whichever
                        // surface is nearer. Physics does not -- a character
                        // stands on whichever is HIGHER, so it spent 44% of a
                        // measured walk on LOD 3 and 4 heightfields while
                        // standing in the middle of the LOD 0 ring, with the
                        // feet up to 1.16 m off the real surface.
                        if (have_fine &&
                            x0 >= fine_x0 && x1 <= fine_x1 &&
                            z0 >= fine_z0 && z1 <= fine_z1) {
                            continue;
                        }

                        ChunkKey key;
                        key.x = i; key.y = j; key.z = k; key.lod = lod;
                        desired_.push_back(key);
                    }
                }
            }

            fine_x0 = bx0; fine_x1 = bx1;

            fine_z0 = bz0; fine_z1 = bz1;
            have_fine = true;
        }

        // Retire anything no longer wanted.
        wanted_.clear();
        for (size_t i = 0; i < desired_.size(); ++i) wanted_[desired_[i]] = 1;
        retire_.clear();
        retired_keys_.clear();
        for (std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::iterator it = resident_.begin();
             it != resident_.end();) {
            if (wanted_.find(it->first) == wanted_.end()) {
                retire_.push_back(it->second);
                retired_keys_.push_back(it->first);
                free_slots_.push_back(it->second);
                it = resident_.erase(it);
            } else {
                ++it;
            }
        }

        // Queue what is missing, nearest first: the chunk under the camera
        // matters more than one at the edge of the ring, and with a per-frame
        // budget the order is what the player actually experiences.
        queue_.clear();
        for (size_t i = 0; i < desired_.size(); ++i) {
            const ChunkKey& k = desired_[i];
            if (resident_.find(k) != resident_.end()) continue;
            const float S = lod_chunk_size(k.lod);
            const float dx = (float(k.x) + 0.5f) * S - cx;
            const float dy = (float(k.y) + 0.5f) * S - cy;
            const float dz = (float(k.z) + 0.5f) * S - cz;
            Pending p;
            p.key = k;
            p.dist2 = dx * dx + dy * dy + dz * dz;
            queue_.push_back(p);
        }
        std::sort(queue_.begin(), queue_.end(), by_distance);
    }

    // Retire first, then mesh up to `budget` chunks. Returns how many were
    // meshed. The budget is the design's cap on remeshing per frame: without
    // it, walking into an unloaded region stalls for as long as it takes to
    // mesh everything at once.
    UINT record_work(ID3D12GraphicsCommandList* cmd, ChunkMesher& mesher, UINT budget) {
        for (size_t i = 0; i < retire_.size(); ++i) mesher.record_retire(cmd, retire_[i]);
        retire_.clear();

        last_meshed_.clear();
        UINT done = 0;
        for (size_t i = 0; i < queue_.size() && done < budget; ++i) {
            if (free_slots_.empty()) break;
            const ChunkKey& k = queue_[i].key;
            const UINT slot = free_slots_.back();
            free_slots_.pop_back();
            const float S = lod_chunk_size(k.lod);
            const float origin[3] = {float(k.x) * S, float(k.y) * S, float(k.z) * S};
            mesher.record_mesh(cmd, slot, origin, lod_voxel(k.lod));
            resident_[k] = slot;
            MeshedChunk mc;
            mc.key = k;
            mc.slot = slot;
            last_meshed_.push_back(mc);
            ++done;
        }
        return done;
    }

    // What was meshed in the last record_work call, for the readback ring.
    struct MeshedChunk {
        ChunkKey key;
        UINT     slot;
    };
    const std::vector<MeshedChunk>& last_meshed() const { return last_meshed_; }

    // Chunks dropped by the last update. Their collision patches must go with
    // them, or a character keeps standing on terrain nothing draws any more.
    const std::vector<ChunkKey>& retired_keys() const { return retired_keys_; }

    // Which slot a chunk currently owns, if it is still resident. The mesh
    // readback needs this: by the time a copy lands the chunk may have been
    // retired and its slot handed to something else entirely.
    bool slot_of(const ChunkKey& k, UINT& out) const {
        std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::const_iterator it =
            resident_.find(k);
        if (it == resident_.end()) return false;
        out = it->second;
        return true;
    }

    size_t resident_count() const { return resident_.size(); }
    size_t pending_count() const {
        // The queue is rebuilt each update, so what is left after a frame's
        // budget is simply the part not yet taken.
        size_t n = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (resident_.find(queue_[i].key) == resident_.end()) ++n;
        }
        return n;
    }
    size_t free_slot_count() const { return free_slots_.size(); }

private:
    struct Pending {
        ChunkKey key;
        float    dist2;
    };
    static bool by_distance(const Pending& a, const Pending& b) { return a.dist2 < b.dist2; }

    std::vector<ChunkKey> desired_;
    std::unordered_map<ChunkKey, UINT, ChunkKeyHash> resident_;
    std::unordered_map<ChunkKey, int, ChunkKeyHash>  wanted_;
    std::vector<Pending> queue_;
    std::vector<UINT>    retire_;
    std::vector<UINT>    free_slots_;
    std::vector<MeshedChunk> last_meshed_;
    std::vector<ChunkKey>    retired_keys_;
};

}  // namespace engine

#endif  // ENGINE_STREAMER_H
