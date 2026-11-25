#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/sky_access.h"
#include "engine/field/world_gpu.h"
#include "engine/svdag/brick_store.h"
#include "worldgen/world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <execution>
#include <queue>
#include <utility>

namespace engine {

namespace {
inline int floor_div(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

// Past this many bricks (~100 MB of labels) the cache starts over.
const size_t kMaxBricks = 200000;

double now_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}
}  // namespace

// --- the main thread ---------------------------------------------------------

void SkyAccess::init(const WorldGpu* world, const BrickStore* edits) {
    shutdown();
    world_ = world;
    edits_ = edits;
    bricks_.clear();
    columns_.clear();
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
        cam_req_gen_ = gen_;
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
        Cmd c = {};
        bool have = false, cam = false;
        float cp[3] = {0.0f, 0.0f, 0.0f};
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this]() { return stop_ || !queue_.empty() || cam_pending_; });
            if (stop_) return;
            if (!queue_.empty()) {
                c = queue_.front();
                queue_.pop_front();
                have = true;
            } else {
                // The camera only once the queue is empty: every edit before
                // it has been applied, so its answer is under the latest.
                for (int k = 0; k < 3; ++k) cp[k] = cam_[k];
                cam_pending_ = false;
                cam = true;
            }
            busy_ = true;
        }
        const double t0 = now_ms();
        if (have && c.kind == Cmd::kInvalidate) {
            forget(c.lo, c.hi);
            worker_gen_ = c.gen;
        } else if (have && c.kind == Cmd::kInvalidateAll) {
            bricks_.clear();
            worker_gen_ = c.gen;
        } else if (have && c.gen < gen_.load()) {
            // Asked before an edit the main thread has since made: it drops
            // the answer, and has asked again. Digging into a big sealed cave
            // re-asked every held block per dig; answering the stale ones
            // left 63,518 requests queued after 40 digs.
        } else if (have) {
            Result r = {};
            for (int k = 0; k < 3; ++k) r.b[k] = c.b[k];
            r.gen = worker_gen_;
            block_mask(c.b[0], c.b[1], c.b[2], r.mask);
            std::lock_guard<std::mutex> lk(mutex_);
            results_.push_back(r);
        } else if (cam) {
            cam_sealed_ = sealed_at(cp[0], cp[1], cp[2]);
            cam_gen_ = worker_gen_;
        }
        if (bricks_.size() > kMaxBricks) bricks_.clear();
        n_bricks_ = bricks_.size();
        us_ += (unsigned long long)((now_ms() - t0) * 1000.0);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            busy_ = false;
            if (queue_.empty() && !cam_pending_) idle_cv_.notify_all();
        }
    }
}

// Forget what an edit can have changed. The labels of every component of air
// within a cell of the box -- a dig can join one to the sky, a fill can seal
// one -- found by a fill through the labels themselves (no field), each cell
// back to unknown; open-by-construction cells are never forgotten and stop
// it. Then the bricks the box touches, evaluated again with the new edits.
void SkyAccess::forget(const float lo[3], const float hi[3]) {
    // BIOMES_SKY_FORGET_ALL: every label, as before the local re-flood (A/B).
    static const bool forget_all = std::getenv("BIOMES_SKY_FORGET_ALL") != nullptr;
    if (forget_all) {
        for (auto& kv : bricks_)
            for (int i = 0; i < kCells; ++i) {
                uint8_t& s = kv.second.st[i];
                if (s == kOpen || s == kSealed || s == kSearching) { s = kUnknown; ++cells_reset_; }
            }
    }
    const int c0[3] = {int(std::floor(lo[0])) - 1, int(std::floor(lo[1])) - 1, int(std::floor(lo[2])) - 1};
    const int c1[3] = {int(std::ceil(hi[0])) + 1, int(std::ceil(hi[1])) + 1, int(std::ceil(hi[2])) + 1};
    auto at = [this](int x, int y, int z) -> uint8_t* {
        const int bx = floor_div(x, kBrick), by = floor_div(y, kBrick), bz = floor_div(z, kBrick);
        auto it = bricks_.find(key(bx, by, bz));
        if (it == bricks_.end()) return nullptr;
        return &it->second.st[((z - bz * kBrick) * kBrick + (y - by * kBrick)) * kBrick + (x - bx * kBrick)];
    };
    auto labelled = [](uint8_t s) { return s == kOpen || s == kSealed || s == kSearching; };
    struct P { int x, y, z; };
    std::vector<P> stack;
    for (int z = c0[2]; z <= c1[2]; ++z)
        for (int y = c0[1]; y <= c1[1]; ++y)
            for (int x = c0[0]; x <= c1[0]; ++x) {
                uint8_t* s = at(x, y, z);
                if (s && labelled(*s)) { *s = kUnknown; stack.push_back({x, y, z}); }
            }
    unsigned long long n = stack.size();
    const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!stack.empty()) {
        const P p = stack.back();
        stack.pop_back();
        for (int k = 0; k < 6; ++k) {
            uint8_t* s = at(p.x + d[k][0], p.y + d[k][1], p.z + d[k][2]);
            if (!s || !labelled(*s)) continue;
            *s = kUnknown;
            stack.push_back({p.x + d[k][0], p.y + d[k][1], p.z + d[k][2]});
            ++n;
        }
    }
    cells_reset_ += n;
    const int b0[3] = {floor_div(c0[0], kBrick), floor_div(c0[1], kBrick), floor_div(c0[2], kBrick)};
    const int b1[3] = {floor_div(c1[0], kBrick), floor_div(c1[1], kBrick), floor_div(c1[2], kBrick)};
    for (int z = b0[2]; z <= b1[2]; ++z)
        for (int y = b0[1]; y <= b1[1]; ++y)
            for (int x = b0[0]; x <= b1[0]; ++x) bricks_.erase(key(x, y, z));
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
                    if (float(y) > open_above) { b.st[idx] = kOpenFixed; continue; }
                    if (!has_edits && float(y) < h - c.reach - deep - 1.0f) { b.st[idx] = kRock; continue; }
                    b.st[idx] = kUnknown;   // decided below
                    jobs.push_back({&b, idx, x, y, z});
                }
            }
    }
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [this](const Job& j) {
        j.b->st[j.idx] = density(j.x, j.y, j.z) > 0.0f ? uint8_t(kRock) : uint8_t(kUnknown);
    });
    cells_evaluated_ += jobs.size();
    for (Brick* b : made) {
        b->has_air = false;
        for (int i = 0; i < kCells && !b->has_air; ++i) b->has_air = b->st[i] != kRock;
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

uint8_t* SkyAccess::cell(int x, int y, int z) {
    const int bx = floor_div(x, kBrick), by = floor_div(y, kBrick), bz = floor_div(z, kBrick);
    Brick* b = brick(bx, by, bz);
    const int i = x - bx * kBrick, j = y - by * kBrick, k = z - bz * kBrick;
    return &b->st[(k * kBrick + j) * kBrick + i];
}

void SkyAccess::search(int x0, int y0, int z0) {
    // Highest first: in open ground the search climbs straight out; in a
    // sealed cave it must see every cell anyway.
    struct Node { int y, x, z; };
    auto cmp = [](const Node& a, const Node& b) { return a.y < b.y; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> q(cmp);
    std::vector<uint8_t*> seen;
    uint8_t* s0 = cell(x0, y0, z0);
    if (*s0 != kUnknown) return;
    *s0 = kSearching;
    seen.push_back(s0);
    q.push({y0, x0, z0});
    uint8_t result = kSealed;
    const int d[6][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
    bool done = false;
    while (!q.empty() && !done) {
        const Node c = q.top();
        q.pop();
        for (int n = 0; n < 6; ++n) {
            const int x = c.x + d[n][0], y = c.y + d[n][1], z = c.z + d[n][2];
            uint8_t* s = cell(x, y, z);
            if (*s == kOpen || *s == kOpenFixed) { result = kOpen; done = true; break; }
            if (*s == kSealed) { done = true; break; }   // over budget before: called sealed
            if (*s != kUnknown) continue;
            *s = kSearching;
            seen.push_back(s);
            q.push({y, x, z});
        }
        if (seen.size() > kMaxSearch) break;   // no opening found in kMaxSearch cells: sealed
    }
    for (uint8_t* s : seen) *s = result;
    ++searches_;
    cells_searched_ += seen.size();
    if (result == kSealed) ++sealed_;
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

bool SkyAccess::sealed_at(float x, float y, float z) {
    const int ix = int(std::floor(x + 0.5f)), iy = int(std::floor(y + 0.5f)), iz = int(std::floor(z + 0.5f));
    if (!may_seal(floor_div(ix, kBrick), floor_div(iy, kBrick), floor_div(iz, kBrick))) return false;
    uint8_t* s = cell(ix, iy, iz);
    if (*s == kUnknown) search(ix, iy, iz);
    return *s == kSealed;
}

void SkyAccess::block_mask(int bx, int by, int bz, uint32_t mask[kMaskWords]) {
    for (int i = 0; i < kMaskWords; ++i) mask[i] = 0u;
    if (!may_seal(bx, by, bz)) return;
    Brick* b = brick(bx, by, bz);
    if (!b->has_air) return;
    for (int k = 0; k < kBrick; ++k)
        for (int j = 0; j < kBrick; ++j)
            for (int i = 0; i < kBrick; ++i) {
                const int idx = (k * kBrick + j) * kBrick + i;
                if (b->st[idx] == kUnknown) search(bx * kBrick + i, by * kBrick + j, bz * kBrick + k);
                if (b->st[idx] == kSealed) mask[idx >> 5] |= 1u << (idx & 31);
            }
}

}  // namespace engine
