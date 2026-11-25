#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/sky_access.h"
#include "engine/field/world_gpu.h"
#include "engine/svdag/brick_store.h"
#include "worldgen/world.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <utility>

namespace engine {

namespace {
inline int floor_div(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

// Past this many bricks (~120 MB) the cache starts over.
const size_t kMaxBricks = 100000;
// Blocks answered in one batch: one search serves them all, but a long batch
// delays the camera and the next edit.
const size_t kBatch = 256;

inline int64_t cell_key(int x, int y, int z) {
    return (int64_t(x + (1 << 20)) << 42) | (int64_t(y + (1 << 20)) << 21) | int64_t(z + (1 << 20));
}
inline void cell_unkey(int64_t k, int& x, int& y, int& z) {
    x = int((k >> 42) & 0x1FFFFF) - (1 << 20);
    y = int((k >> 21) & 0x1FFFFF) - (1 << 20);
    z = int(k & 0x1FFFFF) - (1 << 20);
}

double now_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}
}  // namespace

// --- the main thread ---------------------------------------------------------

void SkyAccess::init(const WorldGpu* world, const BrickStore* edits, float reach_m) {
    shutdown();
    world_ = world;
    edits_ = edits;
    reach_ = (std::max)(8, (std::min)(254, int(reach_m + 0.5f)));
    bricks_.clear();
    columns_.clear();
    sky_floor_.clear();
    queue_.clear();
    results_.clear();
    cam_pending_ = false;
    busy_ = false;
    stop_ = false;
    worker_gen_ = gen_.load();
    const worldgen::ArchetypeTable& t = world->table();
    caves_ = worldgen::caves_enabled(t);
    cave_depth_ = 0.0f;
    for (size_t i = 0; i < t.size(); ++i)
        if (t.biomes[i].cave_density > 0.0f) cave_depth_ = (std::max)(cave_depth_, t.biomes[i].cave_depth_max);
    // The entrances' shafts end in cave air, inside the band; a margin for
    // the mask's own edge.
    if (caves_) cave_depth_ += 4.0f;
    thread_ = std::thread([this]() { run(); });
}

void SkyAccess::shutdown() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void SkyAccess::invalidate_box(const float lo[3], const float hi[3]) {
    if (!world_) return;
    Cmd c = {};
    c.kind = Cmd::kInvalidate;
    for (int k = 0; k < 3; ++k) { c.lo[k] = lo[k]; c.hi[k] = hi[k]; }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        c.gen = ++gen_;
        queue_.push_back(c);
    }
    cv_.notify_one();
}

void SkyAccess::invalidate_all() {
    if (!world_) return;
    Cmd c = {};
    c.kind = Cmd::kInvalidateAll;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        c.gen = ++gen_;
        queue_.push_back(c);
    }
    cv_.notify_one();
}

bool SkyAccess::may_seal_cheap(int bx, int by, int bz) const {
    if (!world_) return false;
    if (caves_) return true;
    if (!edits_ || edits_->brick_count() == 0) return false;
    std::vector<BrickKey> touched;
    const float lo[3] = {float(bx * kBrick) - 0.5f, float(by * kBrick) - 0.5f, float(bz * kBrick) - 0.5f};
    const float hi[3] = {lo[0] + float(kBrick), lo[1] + float(kBrick), lo[2] + float(kBrick)};
    edits_->bricks_in_box(lo, hi, touched);
    return !touched.empty();
}

void SkyAccess::request_block(int bx, int by, int bz) {
    Cmd c = {};
    c.kind = Cmd::kBlock;
    c.b[0] = bx; c.b[1] = by; c.b[2] = bz;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        c.gen = gen_;
        queue_.push_back(c);
    }
    cv_.notify_one();
}

void SkyAccess::request_camera(float x, float y, float z) {
    if (!world_) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cam_[0] = x; cam_[1] = y; cam_[2] = z;
        cam_pending_ = true;
    }
    cv_.notify_one();
}

void SkyAccess::take_results(std::vector<Result>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    out.insert(out.end(), results_.begin(), results_.end());
    results_.clear();
}

void SkyAccess::wait_idle() {
    if (!thread_.joinable()) return;
    std::unique_lock<std::mutex> lk(mutex_);
    idle_cv_.wait(lk, [this]() { return queue_.empty() && !cam_pending_ && !busy_; });
}

size_t SkyAccess::queued() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

// --- the worker --------------------------------------------------------------

void SkyAccess::run() {
    for (;;) {
        Cmd inv = {};
        bool have_inv = false, cam = false;
        std::vector<Cmd> blocks;
        float cp[3] = {0.0f, 0.0f, 0.0f};
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this]() { return stop_ || !queue_.empty() || cam_pending_; });
            if (stop_) return;
            // An edit first if it is next; else the block requests up to the
            // next edit (at most kBatch), one search for them all; the camera
            // once nothing is queued, so its answer is under the latest edit.
            if (!queue_.empty() && queue_.front().kind != Cmd::kBlock) {
                inv = queue_.front();
                queue_.pop_front();
                have_inv = true;
            } else {
                while (!queue_.empty() && queue_.front().kind == Cmd::kBlock && blocks.size() < kBatch) {
                    // Asked before an edit the main thread has since made: it
                    // drops the answer and has asked again.
                    if (queue_.front().gen >= gen_.load()) blocks.push_back(queue_.front());
                    queue_.pop_front();
                }
                if (queue_.empty() && cam_pending_) {
                    for (int k = 0; k < 3; ++k) cp[k] = cam_[k];
                    cam_pending_ = false;
                    cam = true;
                }
            }
            busy_ = true;
        }
        const double t0 = now_ms();
        if (have_inv && inv.kind == Cmd::kInvalidate) {
            forget(inv.lo, inv.hi);
            worker_gen_ = inv.gen;
        } else if (have_inv) {
            bricks_.clear();
            sky_floor_.clear();
            worker_gen_ = inv.gen;
        } else if (!blocks.empty() || cam) {
            answer(blocks, cam, cp);
        }
        if (bricks_.size() > kMaxBricks) {
            bricks_.clear();
            sky_floor_.clear();
        }
        n_bricks_ = bricks_.size();
        us_ += (unsigned long long)((now_ms() - t0) * 1000.0);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            busy_ = false;
            if (queue_.empty() && !cam_pending_) idle_cv_.notify_all();
        }
    }
}

// The blocks' and the camera's cells with no distance yet are searched
// together; then each block's 512 distances go back.
void SkyAccess::answer(std::vector<Cmd>& blocks, bool camera, const float cam[3]) {
    std::vector<int64_t> sources;
    std::vector<uint8_t> live(blocks.size(), 0);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const int* b = blocks[i].b;
        if (!may_seal(b[0], b[1], b[2])) continue;
        live[i] = 1;
        Brick* br = brick(b[0], b[1], b[2]);
        if (!br->has_air) continue;
        for (int idx = 0; idx < kCells; ++idx) {
            if (br->st[idx] != kAir || ((br->known[idx >> 6] >> (idx & 63)) & 1u)) continue;
            const int x = b[0] * kBrick + (idx & 7), y = b[1] * kBrick + ((idx >> 3) & 7), z = b[2] * kBrick + (idx >> 6);
            sources.push_back(cell_key(x, y, z));
        }
    }
    int cx = 0, cy = 0, cz = 0;
    bool cam_cell = false;
    if (camera) {
        cx = int(std::floor(cam[0] + 0.5f)); cy = int(std::floor(cam[1] + 0.5f)); cz = int(std::floor(cam[2] + 0.5f));
        if (may_seal(floor_div(cx, kBrick), floor_div(cy, kBrick), floor_div(cz, kBrick))) {
            int idx;
            Brick* br = brick_of(cx, cy, cz, idx);
            // A camera brushing a wall (or a still's camera set inside it)
            // rounds to rock: the nearest air within 2 m speaks for it.
            if (br->st[idx] == kRock) {
                int best = 1 << 30, bx = cx, by = cy, bz = cz;
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 >= best) continue;
                            int ni;
                            const Brick* nb = brick_of(cx + dx, cy + dy, cz + dz, ni);
                            if (nb->st[ni] == kAir) { best = d2; bx = cx + dx; by = cy + dy; bz = cz + dz; }
                        }
                cx = bx; cy = by; cz = bz;
                br = brick_of(cx, cy, cz, idx);
            }
            cam_cell = br->st[idx] == kAir;
            dbg_cam_st_ = int(br->st[idx]);
            dbg_cam_floor_ = sky_floor(cx, cz);
            if (cam_cell && !((br->known[idx >> 6] >> (idx & 63)) & 1u)) sources.push_back(cell_key(cx, cy, cz));
        }
    }
    if (!sources.empty()) distances(sources);

    std::vector<Result> out(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        Result& r = out[i];
        for (int k = 0; k < 3; ++k) r.b[k] = blocks[i].b[k];
        r.gen = worker_gen_;
        std::memset(r.dist, 0, sizeof r.dist);
        if (!live[i]) continue;
        const Brick* br = brick(r.b[0], r.b[1], r.b[2]);
        for (int idx = 0; idx < kCells; ++idx)
            if (br->st[idx] == kAir) r.dist[idx] = br->dist[idx];
    }
    if (camera) {
        uint8_t d = 0;
        if (cam_cell) {
            int idx;
            const Brick* br = brick_of(cx, cy, cz, idx);
            d = br->dist[idx];
        }
        cam_dist_ = d;
        cam_gen_ = worker_gen_;
    }
    if (!out.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        results_.insert(results_.end(), out.begin(), out.end());
    }
}

// Exact distances for `sources`: forward from all of them through air out to
// the reach, noting every sky cell met (and not going past one); then back
// from those over the cells reached. Any shortest path from a source to the
// sky within the reach lies inside what the forward search saw.
void SkyAccess::distances(const std::vector<int64_t>& sources) {
    struct Scratch { uint16_t fwd[kCells], rev[kCells]; };
    std::unordered_map<int64_t, Scratch> scratch;
    const uint16_t kUnseen = 0xFFFF;
    auto scr = [&](int x, int y, int z, int& idx, Brick*& b) -> Scratch& {
        b = brick_of(x, y, z, idx);
        const int64_t bk = key(floor_div(x, kBrick), floor_div(y, kBrick), floor_div(z, kBrick));
        auto it = scratch.find(bk);
        if (it == scratch.end()) {
            it = scratch.emplace(bk, Scratch()).first;
            std::memset(it->second.fwd, 0xFF, sizeof it->second.fwd);
            std::memset(it->second.rev, 0xFF, sizeof it->second.rev);
        }
        return it->second;
    };
    const int d6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::deque<int64_t> q;
    std::vector<int64_t> openings;
    for (int64_t k : sources) {
        int x, y, z, idx;
        Brick* b;
        cell_unkey(k, x, y, z);
        Scratch& s = scr(x, y, z, idx, b);
        if (s.fwd[idx] != kUnseen) continue;
        s.fwd[idx] = 0;
        q.push_back(k);
    }
    unsigned long long seen = q.size();
    while (!q.empty()) {
        const int64_t k = q.front();
        q.pop_front();
        int x, y, z, idx;
        Brick* b;
        cell_unkey(k, x, y, z);
        const uint16_t f = scr(x, y, z, idx, b).fwd[idx];
        if (is_sky(x, y, z)) { openings.push_back(k); continue; }
        if (f >= reach_) continue;
        for (int n = 0; n < 6; ++n) {
            const int nx = x + d6[n][0], ny = y + d6[n][1], nz = z + d6[n][2];
            int ni;
            Brick* nb;
            Scratch& ns = scr(nx, ny, nz, ni, nb);
            if (nb->st[ni] == kRock || ns.fwd[ni] != kUnseen) continue;
            ns.fwd[ni] = uint16_t(f + 1);
            q.push_back(cell_key(nx, ny, nz));
            ++seen;
        }
    }
    for (int64_t k : openings) {
        int x, y, z, idx;
        Brick* b;
        cell_unkey(k, x, y, z);
        scr(x, y, z, idx, b).rev[idx] = 0;
        q.push_back(k);
    }
    while (!q.empty()) {
        const int64_t k = q.front();
        q.pop_front();
        int x, y, z, idx;
        Brick* b;
        cell_unkey(k, x, y, z);
        const uint16_t r = scr(x, y, z, idx, b).rev[idx];
        if (r >= reach_) continue;
        for (int n = 0; n < 6; ++n) {
            const int nx = x + d6[n][0], ny = y + d6[n][1], nz = z + d6[n][2];
            const int64_t nbk = key(floor_div(nx, kBrick), floor_div(ny, kBrick), floor_div(nz, kBrick));
            auto it = scratch.find(nbk);
            if (it == scratch.end()) continue;   // never reached forward
            const int ni = ((nz - floor_div(nz, kBrick) * kBrick) * kBrick + (ny - floor_div(ny, kBrick) * kBrick)) * kBrick +
                           (nx - floor_div(nx, kBrick) * kBrick);
            Scratch& ns = it->second;
            if (ns.fwd[ni] == kUnseen || ns.rev[ni] != kUnseen) continue;
            ns.rev[ni] = uint16_t(r + 1);
            q.push_back(cell_key(nx, ny, nz));
        }
    }
    for (int64_t k : sources) {
        int x, y, z, idx;
        Brick* b;
        cell_unkey(k, x, y, z);
        const uint16_t r = scr(x, y, z, idx, b).rev[idx];
        b->dist[idx] = r == kUnseen ? kNoSky : uint8_t((std::min)(int(r), 254));
        b->known[idx >> 6] |= uint64_t(1) << (idx & 63);
        if (r == kUnseen) ++no_sky_;
    }
    ++searches_;
    cells_searched_ += seen;
}

// Forget what an edit can have changed: the distances of every cell within
// the reach of it (a path through the box can be no longer), the bricks it
// touched (evaluated again with the new edits) and the sky columns over it.
void SkyAccess::forget(const float lo[3], const float hi[3]) {
    static const bool forget_all = std::getenv("BIOMES_SKY_FORGET_ALL") != nullptr;
    const int c0[3] = {int(std::floor(lo[0])) - 1, int(std::floor(lo[1])) - 1, int(std::floor(lo[2])) - 1};
    const int c1[3] = {int(std::ceil(hi[0])) + 1, int(std::ceil(hi[1])) + 1, int(std::ceil(hi[2])) + 1};
    const int r = (reach_ + kBrick) / kBrick + 1;
    const int b0[3] = {floor_div(c0[0], kBrick), floor_div(c0[1], kBrick), floor_div(c0[2], kBrick)};
    const int b1[3] = {floor_div(c1[0], kBrick), floor_div(c1[1], kBrick), floor_div(c1[2], kBrick)};
    unsigned long long n = 0;
    for (auto& kv : bricks_) {
        const int64_t k = kv.first;
        const int bx = int((k >> 42) & 0x1FFFFF) - (1 << 20), by = int((k >> 21) & 0x1FFFFF) - (1 << 20),
                  bz = int(k & 0x1FFFFF) - (1 << 20);
        if (!forget_all && (bx < b0[0] - r || bx > b1[0] + r || by < b0[1] - r || by > b1[1] + r || bz < b0[2] - r ||
                            bz > b1[2] + r))
            continue;
        for (int w = 0; w < kCells / 64; ++w) {
            n += uint64_t(__popcnt64(kv.second.known[w]));
            kv.second.known[w] = 0;
        }
    }
    cells_reset_ += n;
    for (int z = b0[2]; z <= b1[2]; ++z)
        for (int y = b0[1]; y <= b1[1]; ++y)
            for (int x = b0[0]; x <= b1[0]; ++x) bricks_.erase(key(x, y, z));
    for (int z = c0[2]; z <= c1[2]; ++z)
        for (int x = c0[0]; x <= c1[0]; ++x) sky_floor_.erase(cell_key(x, 0, z));
}

// Under the shared edit lock (evaluate_batch).
float SkyAccess::density(int x, int y, int z) const {
    float d = 0.0f;
    if (edits_ && edits_->edited_at(2 * x, 2 * y, 2 * z, d)) return d;
    return worldgen::terrain_density(world_->world(), world_->params(), world_->table(), float(x), float(y), float(z),
                                     1.0f);
}

bool SkyAccess::has_edits_locked(int bx, int by, int bz) const {
    if (!edits_ || edits_->brick_count() == 0) return false;
    std::vector<BrickKey> touched;
    const float lo[3] = {float(bx * kBrick) - 0.5f, float(by * kBrick) - 0.5f, float(bz * kBrick) - 0.5f};
    const float hi[3] = {lo[0] + float(kBrick), lo[1] + float(kBrick), lo[2] + float(kBrick)};
    edits_->bricks_in_box(lo, hi, touched);
    return !touched.empty();
}

const SkyAccess::Column& SkyAccess::column(int bx, int bz) {
    const int64_t k = key(bx, 0, bz);
    auto it = columns_.find(k);
    if (it != columns_.end()) return it->second;
    Column c;
    const worldgen::SeedWorld& w = world_->world();
    const worldgen::WorldParams& wp = world_->params();
    const worldgen::ArchetypeTable& t = world_->table();
    c.hmin = 1e30f;
    c.hmax = -1e30f;
    for (int k2 = 0; k2 < kBrick; ++k2)
        for (int i = 0; i < kBrick; ++i) {
            const float x = float(bx * kBrick + i), z = float(bz * kBrick + k2);
            const float r = std::sqrt(x * x + z * z) / wp.radius;
            const float h = worldgen::terrain_height(w, wp, t, x, z, r, 1.0f);
            c.h[k2 * kBrick + i] = h;
            c.hmin = (std::min)(c.hmin, h);
            c.hmax = (std::max)(c.hmax, h);
        }
    // The overhang term reaches strength x overhang_scale either side of the
    // height; the blended strength is never more than the strongest biome
    // with weight here. Its blend is ~500 m wide, so the corners and the
    // middle of 8 m cannot miss one.
    float strength = 0.0f;
    const float px[5] = {0.0f, 8.0f, 0.0f, 8.0f, 4.0f}, pz[5] = {0.0f, 0.0f, 8.0f, 8.0f, 4.0f};
    for (int s = 0; s < 5; ++s) {
        int b[8];
        float wt[8];
        const int n = worldgen::biome_mix(w, wp, float(bx * kBrick) + px[s], float(bz * kBrick) + pz[s], b, wt);
        for (int j = 0; j < n; ++j)
            if (wt[j] > 0.0f && b[j] >= 0) strength = (std::max)(strength, t.biomes[size_t(b[j])].overhang_strength);
    }
    c.reach = strength * wp.overhang_scale + 1.0f;
    return columns_.emplace(k, c).first->second;
}

void SkyAccess::evaluate_batch(const std::vector<int>& bxyz) {
    struct Job { Brick* b; int idx, x, y, z; };
    std::vector<Job> jobs;
    std::vector<Brick*> made;
    // The edits are read here and only here; a dig waits for this batch.
    std::shared_lock<std::shared_mutex> lk(edits_mutex_);
    for (size_t n = 0; n + 2 < bxyz.size(); n += 3) {
        const int bx = bxyz[n], by = bxyz[n + 1], bz = bxyz[n + 2];
        const int64_t k = key(bx, by, bz);
        if (bricks_.count(k)) continue;
        Brick& b = bricks_[k];
        std::memset(b.dist, 0, sizeof b.dist);
        std::memset(b.known, 0, sizeof b.known);
        made.push_back(&b);
        const Column& c = column(bx, bz);
        const bool has_edits = has_edits_locked(bx, by, bz);
        const float deep = caves_ ? cave_depth_ : 0.0f;
        for (int k2 = 0; k2 < kBrick; ++k2)
            for (int i = 0; i < kBrick; ++i) {
                const int x = bx * kBrick + i, z = bz * kBrick + k2;
                const float h = c.h[k2 * kBrick + i];
                // Open by construction above the overhang's reach -- and
                // above anything the player built in this column.
                float open_above = h + c.reach;
                if (has_edits) {
                    float top = 0.0f;
                    if (edits_->column_top(2 * x, 2 * z, top)) open_above = (std::max)(open_above, top + 1.0f);
                }
                for (int j = 0; j < kBrick; ++j) {
                    const int y = by * kBrick + j;
                    const int idx = (k2 * kBrick + j) * kBrick + i;   // x + 8 y + 64 z
                    if (float(y) > open_above) { b.st[idx] = kSkyFixed; continue; }
                    if (!has_edits && float(y) < h - c.reach - deep - 1.0f) { b.st[idx] = kRock; continue; }
                    b.st[idx] = kAir;   // decided below
                    jobs.push_back({&b, idx, x, y, z});
                }
            }
    }
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [this](const Job& j) {
        j.b->st[j.idx] = density(j.x, j.y, j.z) > 0.0f ? uint8_t(kRock) : uint8_t(kAir);
    });
    cells_evaluated_ += jobs.size();
    for (Brick* b : made) {
        b->has_air = false;
        for (int i = 0; i < kCells && !b->has_air; ++i) b->has_air = b->st[i] == kAir;
    }
}

SkyAccess::Brick* SkyAccess::brick(int bx, int by, int bz) {
    auto it = bricks_.find(key(bx, by, bz));
    if (it != bricks_.end()) return &it->second;
    // This one and its face neighbours: a search that needs a brick nearly
    // always goes on into the next, and the batch evaluates in parallel.
    std::vector<int> want = {bx, by, bz};
    const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int n = 0; n < 6; ++n) {
        const int x = bx + d[n][0], y = by + d[n][1], z = bz + d[n][2];
        if (!bricks_.count(key(x, y, z))) { want.push_back(x); want.push_back(y); want.push_back(z); }
    }
    evaluate_batch(want);
    return &bricks_.find(key(bx, by, bz))->second;
}

SkyAccess::Brick* SkyAccess::brick_of(int x, int y, int z, int& idx) {
    const int bx = floor_div(x, kBrick), by = floor_div(y, kBrick), bz = floor_div(z, kBrick);
    idx = ((z - bz * kBrick) * kBrick + (y - by * kBrick)) * kBrick + (x - bx * kBrick);
    return brick(bx, by, bz);
}

// The lowest cell of 1 m column (x, z) with sky straight above it: down from
// where the column is open by construction to the first rock.
int SkyAccess::sky_floor(int x, int z) {
    const int64_t k = cell_key(x, 0, z);
    auto it = sky_floor_.find(k);
    if (it != sky_floor_.end()) return it->second;
    const Column& c = column(floor_div(x, kBrick), floor_div(z, kBrick));
    float open_above = c.h[(z - floor_div(z, kBrick) * kBrick) * kBrick + (x - floor_div(x, kBrick) * kBrick)] + c.reach;
    {
        std::shared_lock<std::shared_mutex> lk(edits_mutex_);
        float top = 0.0f;
        if (edits_ && edits_->brick_count() > 0 && edits_->column_top(2 * x, 2 * z, top))
            open_above = (std::max)(open_above, top + 1.0f);
    }
    int y = int(std::floor(open_above)) + 1;
    for (int guard = 0; guard < 2048; ++guard) {
        int idx;
        const Brick* b = brick_of(x, y - 1, z, idx);
        if (b->st[idx] == kRock) break;
        --y;
    }
    sky_floor_[k] = y;
    return y;
}

bool SkyAccess::is_sky(int x, int y, int z) {
    int idx;
    const Brick* b = brick_of(x, y, z, idx);
    if (b->st[idx] == kSkyFixed) return true;
    if (b->st[idx] != kAir) return false;
    return y >= sky_floor(x, z);
}

bool SkyAccess::may_seal(int bx, int by, int bz) {
    if (!world_) return false;
    bool has_edits;
    {
        std::shared_lock<std::shared_mutex> lk(edits_mutex_);
        has_edits = has_edits_locked(bx, by, bz);
    }
    if (!caves_ && !has_edits) return false;
    if (!has_edits) {
        // Wholly in the open, or wholly below where any cave reaches.
        const Column& c = column(bx, bz);
        if (float(by * kBrick) > c.hmax + c.reach) return false;
        if (float(by * kBrick + kBrick - 1) < c.hmin - c.reach - cave_depth_ - 1.0f) return false;
    }
    return true;
}

}  // namespace engine
