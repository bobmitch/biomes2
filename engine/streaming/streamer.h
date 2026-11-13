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
// crack of sky shows through.
//
// This comment used to end "Skirts cover it -- see the skirt pass". There was
// no skirt pass. It was written at B3 and went unchallenged for the obvious
// reason: with five levels the seams were close to the camera and the cracks
// were sub-pixel, so nothing contradicted it. Eight levels put seven seams in
// frame and a ground shot went from 71 cracks to 569. The lip in
// cs_vertices is the skirt now, and it is a lip rather than a real skirt
// pass -- cheap, and measured at zero sky pixels below the horizon.
//
// The reason to revisit it is not cosmetic. The lip is BAKED into surface
// vertices, which makes g_vertices mean two things at once: the surface, and
// the surface plus a rendering fudge. That is precisely how it reached Jolt
// and put a character two voxels into the ground, and it is why
// is_lip_vertex() has to reconstruct a cell index from a position -- which it
// cannot do exactly, so the predicate has a deliberate failure direction.
//
// A real skirt pass removes all of that rather than patching it: separate
// quads hanging from the chunk border, appended AFTER the surface geometry.
// Every stored surface vertex stays true, no predicate is needed anywhere,
// and the physics copy excludes the skirt by index range instead of by
// guessing from coordinates. If the lip causes trouble a second time, that is
// the fix, not a better predicate.
// ---------------------------------------------------------------------------
#ifndef ENGINE_STREAMER_H
#define ENGINE_STREAMER_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/svdag/brick_store.h"
#include "engine/svdag/brick_pager.h"
#include "engine/field/world_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine {

// Eight levels: 16, 32, 64, 128, 256, 512, 1024 and 2048 m chunks, which the
// 7 x 7 ring below turns into a +/-7.2 km box.
//
// It was five until the view distance stopped being the trivial field's
// problem. Five reaches +/-900 m, and on a 20 km disc that is 0.8% of the
// world's area -- enough while Track B was looking at a sine heightfield, and
// not enough the moment M1 put the real generator underneath it. Layers 2 and
// 3 exist to produce islandness (7.2 landmasses a seed, the spawn mass holding
// about three quarters of the land) and the design says explicitly that
// whether a world reads as a continent or an archipelago is a feel call made
// by LOOKING. From the ground at +/-900 m there is no coastline in sight but
// your own.
//
// The FIELD at these voxel sizes was already validated: the harness asserts
// LOD agreement from 0.5 m to 64 m, and lod_voxel(7) is exactly 64 m, so
// evaluating the field that coarsely was never the risk. Be precise about
// what that does and does not cover -- it is a statement about the octave
// fade across voxel sizes, and says nothing about residency, seams or skirts
// at those levels. Both of the things that actually broke here were on the
// side it says nothing about: the index cap, and the missing skirt.
//
// They are also the cheapest chunks in the clipmap: --band-report measures
// about one chunk per column at the coarse levels, against the slots that
// empty-chunk reclamation freed.
static const int kLodCount = 8;

// Half-extent in chunks at every level: 7 x 7 columns per level.
static const int kRingHalf = 3;

static const float kVoxel0 = 0.5f;

inline float lod_voxel(int lod) { return kVoxel0 * float(1 << lod); }
// The OWNED span, which is what tiles. The ghost layer overlaps the
// neighbour and must not be counted here, or every chunk would be placed
// one voxel too far apart and the cracks would come back wider.
inline float lod_chunk_size(int lod) { return lod_voxel(lod) * float(kChunkOwnedCells); }

// A chunk that is resident but holds no slot. Most of the clipmap is like
// this: measured over the real world, a LOD 0 column wants nine chunks and the
// surface crosses one of them. The other eight mesh to nothing, and before
// this they each held a slot of the pool -- 981 chunks wanted against 768,
// with the near ground never reached because the air above and below it got
// there first.
static const UINT kNoSlot = 0xFFFFFFFFu;

// The skirt lip sits on the outermost ring of cells, deliberately two voxels
// below the isosurface. Every check that measures a vertex against the field
// has to skip those, or it reports the lip depth instead of the error it was
// built to find -- measured: slot 0's worst residual went from 0.60 m to
// 1.33 m the moment the lip went in, which would have masked any real
// disagreement up to a metre.
//
// A chunk meshes kChunkCells cells from its origin, and the lip is the first
// and last of them in X and Z.
//
// The STRICT inequalities are deliberate; do not relax them to <= and >=.
//
// The kernel lips by cell index, and a cell-0 vertex occupies lx in [0, voxel]
// INCLUSIVE, so a vertex landing exactly on lx == voxel is lipped there and
// missed here. That looks like an off-by-one, and <= appears to fix it. It
// does not: a cell-ONE vertex also sits at lx == voxel when its crossings
// average to the cell's near face, and the kernel does NOT lip that one, so
// <= would raise legitimate ground by two voxels.
//
// The position cannot tell the two apart -- that information was lost when
// the cell index stopped travelling with the vertex -- so neither form is
// correct and the choice is which way to be wrong. Strict leaves a one-vertex
// PIT in the collision mesh; relaxed leaves a one-vertex SPIKE, and a spike is
// what stops a character or launches one. Both need a crossing to land exactly
// on a cell face, which needs a density of exactly zero at a corner.
inline bool is_lip_vertex(const float pos[3], const float origin[3], float voxel) {
    const float span = voxel * float(kChunkCells);
    const float lx = pos[0] - origin[0];
    const float lz = pos[2] - origin[2];
    return lx < voxel || lx > span - voxel || lz < voxel || lz > span - voxel;
}

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
    // The band of heights any surface can occupy. The trivial field answered
    // this with two literals; the real generator derives it from the continent
    // scale and the biome amplitudes, so it is set from outside now.
    void set_band(float lo, float hi) { band_lo_ = lo; band_hi_ = hi; }
    // Per-column bounds, when there is a world to ask.
    void set_bounds(const WorldGpu* w) { bounds_ = w; }

    void init() {
        free_slots_.clear();
        free_slots_.reserve(kSlots);
        // Handed out from the back, so the first chunks take slot 0 upward and
        // a capture of slot 0 is the nearest chunk rather than an arbitrary one.
        for (int i = int(kSlots) - 1; i >= 0; --i) free_slots_.push_back(UINT(i));
        resident_.clear();
        queue_.clear();
        retire_.clear();
        empty_resident_ = 0;
    }

    // Recompute what should be resident. Fills the queue with what is missing
    // and the retire list with what is no longer wanted.
    void update(float cx, float cy, float cz) {
        desired_.clear();

        const float band_lo = band_lo_, band_hi = band_hi_;

        // Each level's world box, so the next level out can test containment.
        float fine_x0 = 0, fine_x1 = 0, fine_z0 = 0, fine_z1 = 0;
        bool have_fine = false;

        for (int lod = 0; lod < kLodCount; ++lod) {
            const float S = lod_chunk_size(lod);
            const int ci = int(std::floor(cx / S));
            const int ck = int(std::floor(cz / S));

            const float bx0 = float(ci - kRingHalf) * S;
            const float bx1 = float(ci + kRingHalf + 1) * S;
            const float bz0 = float(ck - kRingHalf) * S;
            const float bz1 = float(ck + kRingHalf + 1) * S;



            for (int k = ck - kRingHalf; k <= ck + kRingHalf; ++k) {
                for (int i = ci - kRingHalf; i <= ci + kRingHalf; ++i) {
                    // The vertical range is per COLUMN, not global. Over the
                    // real world a global band spans about 700 m, which is 44
                    // chunks of air per column at LOD 0; the pool fills with
                    // them and the ground is never reached. Asking layer 0
                    // where the surface is here costs one fBm and usually
                    // leaves two or three.
                    float col_lo = band_lo, col_hi = band_hi;
                    if (bounds_) {
                        bounds_->column_band(float(i) * S, float(k) * S,
                                             float(i + 1) * S, float(k + 1) * S,
                                             col_lo, col_hi);
                    }
                    const int cj0 = int(std::floor(col_lo / S));
                    const int cj1 = int(std::floor(col_hi / S));
                    for (int j = cj0; j <= cj1; ++j) {
                        const float x0 = float(i) * S, x1 = x0 + S;
                        const float y0 = float(j) * S, y1 = y0 + S;
                        const float z0 = float(k) * S, z1 = z0 + S;

                        // No surface can pass through this chunk at all.
                        if (y1 < col_lo || y0 > col_hi) continue;

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
                retired_keys_.push_back(it->first);
                if (it->second != kNoSlot) {
                    retire_.push_back(it->second);
                    free_slots_.push_back(it->second);
                }
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
    // Throw away every resident chunk whose box the edit touched, so the next
    // update queues them again and they are meshed against the overlay.
    //
    // The box is grown by ONE CELL, not one chunk. A chunk's mesh does depend
    // on its ghost layer, but that layer is a single cell wide -- growing by a
    // whole chunk invalidated a 3x3x3 neighbourhood at every LOD, which at
    // LOD 4 is 768 m of world thrown away for a 7 m hole, and the re-meshing
    // is what the character was waiting on before it could fall.
    size_t last_invalidated() const { return last_invalidated_; }

    void invalidate_box(const float lo[3], const float hi[3]) {
        last_invalidated_ = 0;
        for (std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::iterator it = resident_.begin();
             it != resident_.end();) {
            const float S = lod_chunk_size(it->first.lod);
            const float g = lod_voxel(it->first.lod);
            const float x0 = float(it->first.x) * S - g, x1 = x0 + S + 2.0f * g;
            const float y0 = float(it->first.y) * S - g, y1 = y0 + S + 2.0f * g;
            const float z0 = float(it->first.z) * S - g, z1 = z0 + S + 2.0f * g;
            const bool hit = !(hi[0] < x0 || lo[0] > x1 || hi[1] < y0 || lo[1] > y1 ||
                               hi[2] < z0 || lo[2] > z1);
            if (!hit) { ++it; continue; }
            retired_keys_.push_back(it->first);
            if (it->second != kNoSlot) {
                retire_.push_back(it->second);
                free_slots_.push_back(it->second);
            }
            it = resident_.erase(it);
            ++last_invalidated_;
        }
    }

    // The chunk meshed to nothing, which the counter readback says two
    // frames after the fact. Give the slot back but KEEP the key resident:
    // dropping the key would have update() queue the chunk again next frame,
    // and the pool would spend every frame re-meshing the same air.
    void release_empty(const ChunkKey& k) {
        std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::iterator it = resident_.find(k);
        if (it == resident_.end() || it->second == kNoSlot) return;
        free_slots_.push_back(it->second);
        it->second = kNoSlot;
        ++empty_resident_;
    }
    size_t empty_resident() const { return empty_resident_; }

    // Which chunk is in a slot. A linear scan, for debug checks only.
    bool key_of_slot(UINT slot, ChunkKey& out) const {
        for (std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::const_iterator it =
                 resident_.begin(); it != resident_.end(); ++it) {
            if (it->second == slot) { out = it->first; return true; }
        }
        return false;
    }

    // What the edit overlay needs to answer "which bricks does this chunk
    // read, and where are they in the pool".
    struct EditContext {
        const BrickStore* store = nullptr;
        BrickPager*       pager = nullptr;
        uint64_t          frame = 0;
        std::vector<BrickKey>* uploads = nullptr;
    };

    // Choose this frame's chunks and page in the bricks they need. Records no
    // GPU commands: the caller uploads what `uploads` names and only then
    // calls record_planned.
    UINT plan_work(ChunkMesher& mesher, UINT budget, EditContext& ctx) {
        planned_.clear();
        for (size_t i = 0; i < queue_.size() && planned_.size() < budget; ++i) {
            if (free_slots_.empty()) break;
            const ChunkKey& k = queue_[i].key;
            const UINT slot = free_slots_.back();
            free_slots_.pop_back();

            Planned p;
            p.key = k;
            p.slot = slot;
            p.brick_first = 0;
            p.brick_count = 0;

            if (ctx.store && ctx.pager && ctx.store->brick_count() > 0) {
                const float S = lod_chunk_size(k.lod);
                const float origin[3] = {float(k.x) * S, float(k.y) * S, float(k.z) * S};
                // The span covers the ghost layer as well as the owned cells,
                // or an edit in the last cell would be missed by the chunk
                // that meshes it.
                const float span = lod_voxel(k.lod) * float(kChunkCells);
                const float lo[3] = {origin[0], origin[1], origin[2]};
                const float hi[3] = {origin[0] + span, origin[1] + span, origin[2] + span};
                ctx.store->bricks_in_box(lo, hi, brick_keys_);
                if (!brick_keys_.empty()) {
                    ctx.pager->acquire(brick_keys_, ctx.frame, brick_slots_,
                                       *ctx.uploads);
                    brick_origins_.clear();
                    for (size_t b = 0; b < brick_keys_.size(); ++b) {
                        brick_origins_.push_back(brick_keys_[b].x);
                        brick_origins_.push_back(brick_keys_[b].y);
                        brick_origins_.push_back(brick_keys_[b].z);
                    }
                    p.brick_count = static_cast<UINT>(brick_slots_.size());
                    p.brick_first = mesher.push_brick_headers(
                        brick_origins_.data(), brick_slots_.data(), p.brick_count);
                }
            }
            planned_.push_back(p);
            resident_[k] = slot;
        }
        return static_cast<UINT>(planned_.size());
    }

    // Retire first, then mesh what plan_work chose.
    void record_planned(ID3D12GraphicsCommandList* cmd, ChunkMesher& mesher) {
        for (size_t i = 0; i < retire_.size(); ++i) mesher.record_retire(cmd, retire_[i]);
        retire_.clear();

        last_meshed_.clear();
        for (size_t i = 0; i < planned_.size(); ++i) {
            const Planned& p = planned_[i];
            const float S = lod_chunk_size(p.key.lod);
            const float origin[3] = {float(p.key.x) * S, float(p.key.y) * S,
                                     float(p.key.z) * S};
            mesher.record_mesh(cmd, p.slot, origin, lod_voxel(p.key.lod),
                               p.brick_first, p.brick_count);
            MeshedChunk mc;
            mc.key = p.key;
            mc.slot = p.slot;
            last_meshed_.push_back(mc);
        }
        planned_.clear();
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
    float band_lo_ = -13.0f, band_hi_ = 40.0f;
    const WorldGpu* bounds_ = nullptr;
    size_t last_invalidated_ = 0;
    size_t empty_resident_ = 0;
    struct Planned {
        ChunkKey key;
        UINT     slot;
        UINT     brick_first;
        UINT     brick_count;
    };
    std::vector<Planned> planned_;
    mutable std::vector<BrickKey> brick_keys_;
    mutable std::vector<uint32_t> brick_slots_;
    mutable std::vector<int>      brick_origins_;
};

}  // namespace engine

#endif  // ENGINE_STREAMER_H
