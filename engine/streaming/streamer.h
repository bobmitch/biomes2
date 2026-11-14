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
// no skirt pass. Then there was a LIP -- the border cells' vertices dropped
// two voxels -- and the note here said that if it caused trouble a second
// time, a real skirt pass was the fix, not a better predicate. It did: the
// lip dropped the vertex row two same-LOD neighbours share from BOTH sides,
// and every chunk border was a V groove (see cs_vertices). So the skirt is now
// real: extra curtain geometry hung from the mesh border into the solid, with
// every surface vertex left true. What remains visible is the STEP between
// two levels at a ring boundary, which no crack cover can remove.
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

// The coarsest LOD that gets collision. The character is always inside the
// LOD 0 ring -- streaming follows the camera, and the camera rides the feet --
// so a body at a 64 m voxel 7 km away is never stood on. LOD 2 is a 2 m voxel
// and a +/-224 m ring: room for a fall from altitude to stream in under the
// feet before they arrive. One constant, because two places act on it: what
// physics is handed, and which chunks the renderer reads a collision MESH
// back for -- the one that matters for overhangs, see consume_heights.
static const int kCollisionMaxLod = 2;

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

// A skirt vertex: the hidden end of the curtain cs_quads hangs from a chunk's
// border edge to cover the crack between LOD levels. It is two voxels into the
// solid on purpose, so anything that measures a vertex against the field, and
// the collision mesh, must leave it out.
//
// Exact, by construction: the kernel stores a skirt vertex's normal at HALF
// length (shading normalises, so it looks identical). The predicate this
// replaced, is_lip_vertex, had to guess from a position whether a vertex sat
// in a border cell, could not do it exactly, and documented which way it
// chose to be wrong. The lip it served was itself the bug -- see cs_vertices.
inline bool is_skirt_vertex(const float nrm[3]) {
    return nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2] < 0.5f;
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

    // LOD transitions. With hold on, a chunk that stops being wanted keeps its
    // slot and keeps drawing until everything replacing its footprint has
    // meshed, then cross-fades out over `fade_frames` while the replacements
    // fade in. Off is the old behaviour -- retire at once, and the ground under
    // a LOD change is missing until the queue reaches it -- kept for A/B.
    void set_lod_hold(bool on) { hold_ = on; }
    void set_fade_frames(int n) { fade_frames_ = n < 0 ? 0 : n; }
    // Surface-first queue order and holds that wait on the core surface only.
    // Off is distance-only order, holds waiting on everything -- for A/B.
    void set_prioritise(bool on) { prioritise_ = on; }
    // Count, each update, surface chunks that are queued with nothing drawn
    // over their footprint. Costs a height query per queued chunk.
    void set_measure_holes(bool on) { measure_holes_ = on; }
    // Holes found by the last update, per LOD.
    const size_t* holes_by_lod() const { return holes_; }
    // Detail DEFICIT, per LOD: surface chunks wanted but not yet on screen,
    // whether or not something coarser is covering for them. Holes are the
    // part with nothing covering. Measured with holes.
    const size_t* deficit_by_lod() const { return deficit_; }
    // Frames from a surface chunk first being wanted to it starting to show,
    // per LOD, for every one that made it. The number the eye calls "the
    // detail arrives late".
    const std::vector<int>& latency_frames(int lod) const { return latency_[lod]; }
    size_t held_count() const { return held_.size(); }
    size_t fading_in_count() const { return fading_in_.size(); }
    unsigned long long forced_retires() const { return forced_retires_; }

    // Per-slot (mode, v) for the draw: mode 0 drawn, 1 fading in (drawn where
    // the dither threshold is below v), 2 fading out (drawn where it is at or
    // above v). The two are exact complements at equal v, so a chunk and its
    // replacement on one clock never both draw a pixel.
    void fill_visibility(float* out) const {
        for (UINT s = 0; s < kSlots; ++s) { out[s * 2] = 0.0f; out[s * 2 + 1] = 1.0f; }
        const float n = float(fade_frames_ > 0 ? fade_frames_ : 1);
        for (size_t i = 0; i < fading_in_.size(); ++i) {
            const FadeIn& f = fading_in_[i];
            float v = f.start < 0 ? 0.0f : float(frame_ - f.start) / n;
            if (v > 1.0f) v = 1.0f;
            out[f.slot * 2] = 1.0f; out[f.slot * 2 + 1] = v;
        }
        for (size_t i = 0; i < held_.size(); ++i) {
            const Held& h = held_[i];
            float v = h.fade_start < 0 ? 0.0f : float(frame_ - h.fade_start) / n;
            if (v > 1.0f) v = 1.0f;
            out[h.slot * 2] = 2.0f; out[h.slot * 2 + 1] = v;
        }
        // Retired but not yet zeroed on the GPU -- a capture that stops right
        // after an update leaves these, and they would draw the old ground.
        for (size_t i = 0; i < retire_.size(); ++i) {
            out[retire_[i] * 2] = 2.0f; out[retire_[i] * 2 + 1] = 1.0f;
        }
    }

    // A capture is one still frame of a world that has finished loading:
    // every fade is over. Held chunks become fully invisible (and retire on
    // the next update), and everything fading in is simply drawn.
    void settle_fades() {
        fading_in_.clear();
        for (size_t i = 0; i < held_.size(); ++i) {
            held_[i].fade_start = frame_ - (fade_frames_ > 0 ? fade_frames_ : 1);
        }
    }

    void init() {
        held_.clear();
        fading_in_.clear();
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
        ++frame_;
        cam_[0] = cx; cam_[1] = cy; cam_[2] = cz;
        desired_.clear();
        desired_core_.clear();

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
                    float core_lo = band_lo, core_hi = band_hi;
                    if (bounds_) {
                        bounds_->column_band(float(i) * S, float(k) * S,
                                             float(i + 1) * S, float(k + 1) * S,
                                             col_lo, col_hi, &core_lo, &core_hi);
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
                        // Where the ground itself can be, without the overhang
                        // and cave padding: meshed first, and the only chunks
                        // a held LOD swap waits for.
                        desired_core_.push_back(y1 >= core_lo && y0 <= core_hi ? 1 : 0);
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
                // Physics lets go now, as it always did: a held chunk is a
                // picture of terrain that is no longer the resident truth.
                retired_keys_.push_back(it->first);
                if (it->second != kNoSlot) retire_or_hold(it->first, it->second);
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
            p.core = desired_core_[i] != 0;
            // Completing a LOD swap that is being held is detail the player
            // gets the moment it lands, so it goes ahead of a chunk at the
            // same distance that is not holding anything up.
            if (p.core && prioritise_) {
                for (size_t h = 0; h < held_.size(); ++h) {
                    if (held_[h].fade_start < 0 && xz_overlap(held_[h].key, k)) {
                        p.dist2 *= 0.5f;
                        break;
                    }
                }
            }
            if (!prioritise_) p.core = true;   // old order: distance alone
            queue_.push_back(p);
        }
        // The visible surface first, nearest first; then the margins, which
        // are mostly air and solid rock and meshed to nothing 84% of the time.
        std::sort(queue_.begin(), queue_.end(), by_priority);

        if (measure_holes_) {
            // When each queued chunk first became wanted, and forget the ones
            // that stopped being wanted before they ever showed.
            for (size_t q = 0; q < queue_.size(); ++q) {
                if (queued_since_.find(queue_[q].key) == queued_since_.end())
                    queued_since_[queue_[q].key] = frame_;
            }
            for (std::unordered_map<ChunkKey, int, ChunkKeyHash>::iterator it =
                     queued_since_.begin(); it != queued_since_.end();) {
                if (wanted_.find(it->first) == wanted_.end()) it = queued_since_.erase(it);
                else ++it;
            }
            count_holes();
        }
        advance_fades();
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
            // Held like any other replacement: the ground stays drawn, without
            // the edit, until the remesh WITH it lands, rather than leaving a
            // hole the size of the chunk for the frames in between.
            if (it->second != kNoSlot) retire_or_hold(it->first, it->second);
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
        for (size_t i = 0; i < fading_in_.size(); ++i) {
            if (fading_in_[i].slot == it->second) {
                fading_in_[i] = fading_in_.back();
                fading_in_.pop_back();
                break;
            }
        }
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
            // Holding old chunks must never starve their own replacements:
            // with the pool dry, give back the farthest hold early. That is
            // the old behaviour -- a hole -- but only for the chunk the pool
            // could not afford, and counted so it shows if it ever matters.
            if (free_slots_.empty() && !held_.empty()) force_retire_farthest();
            if (free_slots_.empty()) break;
            const ChunkKey& k = queue_[i].key;
            const UINT slot = free_slots_.back();
            free_slots_.pop_back();
            if (hold_ && fade_frames_ > 0) {
                FadeIn f;
                f.key = k; f.slot = slot; f.start = -1;
                fading_in_.push_back(f);
            } else {
                note_visible(k);   // drawn the frame it is meshed
            }

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
    struct Held   { ChunkKey key; UINT slot; int fade_start; };   // -1: waiting
    struct FadeIn { ChunkKey key; UINT slot; int start; };        // -1: waiting

    static bool xz_overlap(const ChunkKey& a, const ChunkKey& b) {
        const float sa = lod_chunk_size(a.lod), sb = lod_chunk_size(b.lod);
        return float(a.x) * sa < float(b.x + 1) * sb && float(b.x) * sb < float(a.x + 1) * sa &&
               float(a.z) * sa < float(b.z + 1) * sb && float(b.z) * sb < float(a.z + 1) * sa;
    }

    void free_now(UINT slot) {
        retire_.push_back(slot);
        free_slots_.push_back(slot);
    }

    void retire_or_hold(const ChunkKey& key, UINT slot) {
        // A chunk still waiting to fade in was never on screen: nothing to hold.
        for (size_t i = 0; i < fading_in_.size(); ++i) {
            if (fading_in_[i].slot != slot) continue;
            const bool never_shown = fading_in_[i].start < 0;
            fading_in_[i] = fading_in_.back();
            fading_in_.pop_back();
            if (never_shown) { free_now(slot); return; }
            break;
        }
        if (!hold_) { free_now(slot); return; }
        Held h;
        h.key = key; h.slot = slot; h.fade_start = -1;
        held_.push_back(h);
    }

    void force_retire_farthest() {
        size_t best = 0;
        float bd = -1.0f;
        for (size_t i = 0; i < held_.size(); ++i) {
            const float S = lod_chunk_size(held_[i].key.lod);
            const float dx = (float(held_[i].key.x) + 0.5f) * S - cam_[0];
            const float dz = (float(held_[i].key.z) + 0.5f) * S - cam_[2];
            const float d = dx * dx + dz * dz;
            if (d > bd) { bd = d; best = i; }
        }
        free_now(held_[best].slot);
        held_[best] = held_.back();
        held_.pop_back();
        ++forced_retires_;
    }

    // Start, advance and finish the cross-fades. Called at the end of update,
    // once the queue says what is still missing.
    void advance_fades() {
        if (!hold_) return;
        // A hold starts fading out once nothing queued overlaps its footprint:
        // everything replacing it has meshed. Ignoring y is deliberate -- the
        // surface can be in any chunk of the column, and a hold over a column
        // whose replacement is still coming would otherwise let go early.
        for (size_t i = 0; i < held_.size(); ++i) {
            Held& h = held_[i];
            if (h.fade_start >= 0) continue;
            bool waiting = false;
            // Only the CORE surface blocks: margin chunks are mostly air and
            // rock, and waiting on them held new detail back for no picture.
            for (size_t q = 0; q < queue_.size() && !waiting; ++q) {
                if (queue_[q].core && xz_overlap(queue_[q].key, h.key)) waiting = true;
            }
            if (!waiting) h.fade_start = frame_;
        }
        // A replacement starts fading in when no hold over it is still
        // waiting -- the same frame those holds start fading out, so the two
        // run on one clock and their dither patterns are exact complements.
        for (size_t i = 0; i < fading_in_.size(); ++i) {
            FadeIn& f = fading_in_[i];
            if (f.start >= 0) continue;
            bool blocked = false;
            for (size_t h = 0; h < held_.size() && !blocked; ++h) {
                if (held_[h].fade_start < 0 && xz_overlap(held_[h].key, f.key)) blocked = true;
            }
            if (!blocked) { f.start = frame_; note_visible(f.key); }
        }
        // Finish what has run its course.
        for (size_t i = 0; i < held_.size();) {
            if (held_[i].fade_start >= 0 && frame_ - held_[i].fade_start >= fade_frames_) {
                free_now(held_[i].slot);
                held_[i] = held_.back();
                held_.pop_back();
            } else {
                ++i;
            }
        }
        for (size_t i = 0; i < fading_in_.size();) {
            if (fading_in_[i].start >= 0 && frame_ - fading_in_[i].start >= fade_frames_) {
                fading_in_[i] = fading_in_.back();
                fading_in_.pop_back();
            } else {
                ++i;
            }
        }
    }

    // A hole: a queued chunk that holds the surface, with nothing drawn over
    // its footprint -- no hold, and no resident chunk at another level. The
    // outermost level's new ground is the horizon growing rather than a hole
    // in it, so it is counted under its own LOD and read separately.
    // Does the ground pass through this chunk at its centre column? A probe,
    // not a guarantee -- the measurement's notion of "a chunk you would see".
    bool is_surface_chunk(const ChunkKey& k) const {
        if (!bounds_) return false;
        const float S = lod_chunk_size(k.lod);
        const float h = bounds_->height((float(k.x) + 0.5f) * S, (float(k.z) + 0.5f) * S);
        return h >= float(k.y) * S && h < float(k.y + 1) * S;
    }

    void note_visible(const ChunkKey& k) {
        if (!measure_holes_) return;
        std::unordered_map<ChunkKey, int, ChunkKeyHash>::iterator it = queued_since_.find(k);
        if (it == queued_since_.end()) return;
        if (is_surface_chunk(k)) latency_[k.lod].push_back(frame_ - it->second);
        queued_since_.erase(it);
    }

    void count_holes() {
        for (int l = 0; l < kLodCount; ++l) { holes_[l] = 0; deficit_[l] = 0; }
        if (!bounds_) return;
        for (size_t i = 0; i < fading_in_.size(); ++i) {
            if (fading_in_[i].start < 0 && is_surface_chunk(fading_in_[i].key))
                ++deficit_[fading_in_[i].key.lod];
        }
        for (size_t q = 0; q < queue_.size(); ++q) {
            const ChunkKey& k = queue_[q].key;
            if (!is_surface_chunk(k)) continue;
            ++deficit_[k.lod];
            bool covered = false;
            for (size_t i = 0; i < held_.size() && !covered; ++i) {
                if (xz_overlap(held_[i].key, k)) covered = true;
            }
            for (std::unordered_map<ChunkKey, UINT, ChunkKeyHash>::const_iterator it =
                     resident_.begin(); it != resident_.end() && !covered; ++it) {
                if (it->second == kNoSlot || it->first.lod == k.lod) continue;
                if (xz_overlap(it->first, k)) covered = true;
            }
            if (!covered) ++holes_[k.lod];
        }
    }

    std::vector<Held>   held_;
    std::vector<FadeIn> fading_in_;
    int   frame_ = 0;
    // 16 frames, about 0.27 s at 60 Hz: long enough to read as a fade rather
    // than a pop, and a third sooner to full detail than the first 24.
    int   fade_frames_ = 16;
    bool  hold_ = true;
    bool  measure_holes_ = false;
    size_t holes_[kLodCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t deficit_[kLodCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<int> latency_[kLodCount];
    std::unordered_map<ChunkKey, int, ChunkKeyHash> queued_since_;
    unsigned long long forced_retires_ = 0;
    float cam_[3] = {0.0f, 0.0f, 0.0f};

    struct Pending {
        ChunkKey key;
        float    dist2;
        bool     core;   // may hold the 2.5D surface
    };
    static bool by_priority(const Pending& a, const Pending& b) {
        if (a.core != b.core) return a.core;
        return a.dist2 < b.dist2;
    }

    std::vector<ChunkKey> desired_;
    std::vector<unsigned char> desired_core_;
    bool prioritise_ = true;
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
