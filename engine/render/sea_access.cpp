#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/sea_access.h"
#include "engine/field/world_gpu.h"
#include "engine/svdag/brick_store.h"
#include "worldgen/world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>
#include <functional>
#include <queue>

namespace engine {

namespace {
inline int floor_div(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

// Past this many bricks (~10 KB each) the cache starts over.
const size_t kMaxBricks = 12000;
const size_t kBatch = 128;
const float kC = SeaAccess::kCellM;

inline int64_t cell_key(int x, int y, int z) {
    return (int64_t(x + (1 << 20)) << 42) | (int64_t(y + (1 << 20)) << 21) | int64_t(z + (1 << 20));
}

double now_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

const int kD6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
}  // namespace

// --- the main thread ---------------------------------------------------------

void SeaAccess::init(const WorldGpu* world, const BrickStore* edits, float sea_level) {
    shutdown();
    world_ = world;
    edits_ = edits;
    sea_ = sea_level;
    bricks_.clear();
    columns_.clear();
    floor_.clear();
    queue_.clear();
    results_.clear();
    busy_ = false;
    stop_ = false;
    worker_gen_ = gen_.load();
    const worldgen::ArchetypeTable& t = world->table();
    caves_ = worldgen::caves_enabled(t);
    cave_depth_ = 0.0f;
    for (size_t i = 0; i < t.size(); ++i)
        if (t.biomes[i].cave_density > 0.0f) cave_depth_ = (std::max)(cave_depth_, t.biomes[i].cave_depth_max);
    if (caves_) cave_depth_ += 4.0f;
    placed_ = !world->world().caves.empty();
    thread_ = std::thread([this]() { run(); });
}

void SeaAccess::shutdown() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void SeaAccess::invalidate_box(const float lo[3], const float hi[3]) {
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

void SeaAccess::invalidate_all() {
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

void SeaAccess::request_block(int bx, int by, int bz) {
    if (!world_) return;
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

void SeaAccess::take_results(std::vector<Result>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    out.insert(out.end(), results_.begin(), results_.end());
    results_.clear();
}

void SeaAccess::wait_idle() {
    if (!thread_.joinable()) return;
    std::unique_lock<std::mutex> lk(mutex_);
    idle_cv_.wait(lk, [this]() { return queue_.empty() && !busy_; });
}

size_t SeaAccess::queued() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

// --- the worker --------------------------------------------------------------

void SeaAccess::run() {
    for (;;) {
        Cmd inv = {};
        bool have_inv = false;
        std::vector<Cmd> blocks;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this]() { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (queue_.front().kind != Cmd::kBlock) {
                inv = queue_.front();
                queue_.pop_front();
                have_inv = true;
            } else {
                while (!queue_.empty() && queue_.front().kind == Cmd::kBlock && blocks.size() < kBatch) {
                    if (queue_.front().gen >= gen_.load()) blocks.push_back(queue_.front());
                    queue_.pop_front();
                }
            }
            busy_ = true;
        }
        const double t0 = now_ms();
        if (have_inv && inv.kind == Cmd::kInvalidate) {
            forget(inv.lo, inv.hi);
            worker_gen_ = inv.gen;
        } else if (have_inv) {
            // A load: everything is shown full at once.
            bricks_.clear();
            floor_.clear();
            was_wet_.clear();
            have_edit_ = false;
            worker_gen_ = inv.gen;
        } else if (!blocks.empty()) {
            answer(blocks);
        }
        if (bricks_.size() > kMaxBricks) {
            bricks_.clear();
            floor_.clear();
        }
        us_ += (unsigned long long)((now_ms() - t0) * 1000.0);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            busy_ = false;
            if (queue_.empty()) idle_cv_.notify_all();
        }
    }
}

void SeaAccess::answer(std::vector<Cmd>& blocks) {
    std::vector<Result> out(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        const int* b = blocks[i].b;
        Brick* br = brick(b[0], b[1], b[2]);
        for (int idx = 0; idx < kCells; ++idx) {
            if ((br->known[idx >> 6] >> (idx & 63)) & 1u) continue;
            const int x = b[0] * kBrick + (idx & 15), y = b[1] * kBrick + ((idx >> 4) & 15), z = b[2] * kBrick + (idx >> 8);
            const uint8_t st = br->st[idx];
            uint8_t label = kDry;
            bool search = false;
            if (st == kRock) {
                label = kSolid;
            } else if (!below_sea(y)) {
                label = kDry;
            } else if (st == kSeaFixed) {
                label = 1;
            } else if (y >= open_floor(x, z) && column_h(x, z) < sea_) {
                label = 1;   // open water over the generated seabed: the sea itself
            } else {
                search = true;
            }
            if (search) {
                component(x, y, z);   // labels this cell and its whole component
                br = brick(b[0], b[1], b[2]);   // the search may have evaluated (and moved) bricks
            } else {
                br->label[idx] = label;
                br->arrive[idx] = 0;   // the open sea, rock or dry air: no fill to watch
                br->known[idx >> 6] |= uint64_t(1) << (idx & 63);
            }
        }
        Result& r = out[i];
        for (int k = 0; k < 3; ++k) r.b[k] = b[k];
        r.gen = worker_gen_;
        std::memcpy(r.cell, br->label, sizeof r.cell);
        std::memcpy(r.arrive, br->arrive, sizeof r.arrive);
    }
    if (!out.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        results_.insert(results_.end(), out.begin(), out.end());
    }
}

// The component of air holding (x, y, z): followed through every cell that
// is air and not an opening (sea or sky), its openings noted; then which of
// its cells the water fills, and how far each is from the open sea.
void SeaAccess::component(int sx, int sy, int sz) {
    std::unordered_map<int64_t, uint32_t> at;
    std::vector<int> cx, cy, cz;
    std::vector<uint8_t> vent, touch_sea;
    std::deque<uint32_t> q;
    bool sea = false, capped = false;
    // An air cell's kind: 0 to follow, 1 an opening to the sea, 2 an opening
    // to the sky (above sea level), and whether it is open straight up.
    auto kind = [&](int x, int y, int z, uint8_t st, bool& open) -> int {
        if (st == kSeaFixed) { open = true; return 1; }
        if (st == kSkyFixed) { open = true; return 2; }
        open = y >= open_floor(x, z);
        if (open && !below_sea(y)) return 2;
        if (open && column_h(x, z) < sea_) return 1;
        return 0;
    };
    auto add = [&](int x, int y, int z, bool open) {
        at.emplace(cell_key(x, y, z), uint32_t(cx.size()));
        cx.push_back(x); cy.push_back(y); cz.push_back(z);
        // Open straight up (a channel dug below sea level in a land column):
        // its air rises out by itself.
        vent.push_back(open ? 1 : 0);
        touch_sea.push_back(0);
        q.push_back(uint32_t(cx.size() - 1));
    };
    {
        int idx;
        Brick* b = brick_of(sx, sy, sz, idx);
        bool open = false;
        kind(sx, sy, sz, b->st[idx], open);
        add(sx, sy, sz, open);
    }
    while (!q.empty()) {
        const uint32_t i = q.front();
        q.pop_front();
        const int x = cx[i], y = cy[i], z = cz[i];
        for (int n = 0; n < 6; ++n) {
            const int nx = x + kD6[n][0], ny = y + kD6[n][1], nz = z + kD6[n][2];
            int ni;
            Brick* nb = brick_of(nx, ny, nz, ni);
            const uint8_t st = nb->st[ni];
            if (st == kRock) continue;
            bool open = false;
            const int k = kind(nx, ny, nz, st, open);
            if (k != 0) {
                if (k == 1) { sea = true; touch_sea[i] = 1; }
                // An opening beside or above: this cell's air can rise to it.
                if (y <= ny) vent[i] = 1;
                continue;
            }
            if (at.count(cell_key(nx, ny, nz))) continue;
            if (cx.size() >= kMaxComponent) { capped = true; continue; }
            add(nx, ny, nz, open);
        }
    }
    const size_t n = cx.size();
    cells_searched_ += n;
    ++components_;
    if (capped) ++capped_;
    // Venting: from every cell whose air can get out, down or sideways --
    // the way its neighbours' air could rise to it.
    for (uint32_t i = 0; i < n; ++i)
        if (vent[i]) q.push_back(i);
    while (!q.empty()) {
        const uint32_t i = q.front();
        q.pop_front();
        for (int d = 0; d < 6; ++d) {
            if (kD6[d][1] > 0) continue;   // never up
            auto it = at.find(cell_key(cx[i] + kD6[d][0], cy[i] + kD6[d][1], cz[i] + kD6[d][2]));
            if (it == at.end() || vent[it->second]) continue;
            vent[it->second] = 1;
            q.push_back(it->second);
        }
    }
    // Wet, and the path to the open sea through the water.
    std::vector<uint16_t> dist(n, 0xFFFF);
    const bool fills = sea && !capped;
    for (uint32_t i = 0; i < n && fills; ++i) {
        if (touch_sea[i] && vent[i] && below_sea(cy[i])) { dist[i] = 1; q.push_back(i); }
    }
    while (!q.empty()) {
        const uint32_t i = q.front();
        q.pop_front();
        for (int d = 0; d < 6; ++d) {
            auto it = at.find(cell_key(cx[i] + kD6[d][0], cy[i] + kD6[d][1], cz[i] + kD6[d][2]));
            if (it == at.end()) continue;
            const uint32_t j = it->second;
            if (dist[j] != 0xFFFF || !vent[j] || !below_sea(cy[j])) continue;
            dist[j] = uint16_t(dist[i] + 1);
            q.push_back(j);
        }
    }
    // The fill: when the water reaches each wet cell, from where it came in
    // -- the cells beside the open sea, and any wet before the edit -- by a
    // Dijkstra search over the wet cells with gravity in the step costs, so
    // it pours down a shaft, runs along a floor and rises last. Only for the
    // component the latest edit touched.
    std::vector<float> at_t(n, 0.0f);
    bool animate = false;
    if (fills && have_edit_) {
        for (uint32_t i = 0; i < n && !animate; ++i) {
            const float wx = float(cx[i]) * kC, wy = float(cy[i]) * kC, wz = float(cz[i]) * kC;
            animate = wx >= edit_lo_[0] - 1.0f && wx <= edit_hi_[0] + 1.0f && wy >= edit_lo_[1] - 1.0f &&
                      wy <= edit_hi_[1] + 1.0f && wz >= edit_lo_[2] - 1.0f && wz <= edit_hi_[2] + 1.0f;
        }
    }
    if (animate) {
        std::fill(at_t.begin(), at_t.end(), 1e30f);
        typedef std::pair<float, uint32_t> QE;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        for (uint32_t i = 0; i < n; ++i) {
            if (dist[i] == 0xFFFF) continue;
            if (touch_sea[i] || was_wet_.count(cell_key(cx[i], cy[i], cz[i]))) {
                at_t[i] = 0.0f;
                pq.push(QE(0.0f, i));
            }
        }
        while (!pq.empty()) {
            const QE e = pq.top();
            pq.pop();
            const uint32_t i = e.second;
            if (e.first > at_t[i]) continue;
            for (int d = 0; d < 6; ++d) {
                auto it = at.find(cell_key(cx[i] + kD6[d][0], cy[i] + kD6[d][1], cz[i] + kD6[d][2]));
                if (it == at.end() || dist[it->second] == 0xFFFF) continue;
                const float step = kD6[d][1] < 0 ? kFillDown : (kD6[d][1] > 0 ? kFillUp : kFillSide);
                const float t = e.first + step;
                if (t < at_t[it->second]) {
                    at_t[it->second] = t;
                    pq.push(QE(t, it->second));
                }
            }
        }
        ++animated_;
    }
    for (uint32_t i = 0; i < n; ++i) {
        int idx;
        Brick* b = brick_of(cx[i], cy[i], cz[i], idx);
        const float tq = at_t[i] < 1e29f ? at_t[i] / kArriveStep + 0.5f : 0.0f;
        b->arrive[idx] = uint8_t(tq > 255.0f ? 255.0f : tq);
        was_wet_.erase(cell_key(cx[i], cy[i], cz[i]));
        uint8_t label = kDry;
        // Vented water joined to the sea through vented water: dist set. A
        // vented cell the water cannot reach without passing through
        // trapped air stays dry.
        if (fills && dist[i] != 0xFFFF) label = uint8_t((std::min)(int(dist[i]), 253) + 1);
        b->label[idx] = label;
        b->known[idx >> 6] |= uint64_t(1) << (idx & 63);
    }
}

// An edit can join or split a component of any size: every label goes. The
// bricks it touched are evaluated again, and the open floors over it.
void SeaAccess::forget(const float lo[3], const float hi[3]) {
    // What is wet now stays wet through the relabel: the fill starts from it.
    for (auto& kv : bricks_) {
        const int64_t k = kv.first;
        const int bx = int((k >> 42) & 0x1FFFFF) - (1 << 20), by = int((k >> 21) & 0x1FFFFF) - (1 << 20),
                  bz = int(k & 0x1FFFFF) - (1 << 20);
        for (int idx = 0; idx < kCells; ++idx) {
            if (!((kv.second.known[idx >> 6] >> (idx & 63)) & 1u)) continue;
            const uint8_t l = kv.second.label[idx];
            if (l == kDry || l == kSolid) continue;
            was_wet_.insert(cell_key(bx * kBrick + (idx & 15), by * kBrick + ((idx >> 4) & 15), bz * kBrick + (idx >> 8)));
        }
    }
    have_edit_ = true;
    for (int k = 0; k < 3; ++k) { edit_lo_[k] = lo[k]; edit_hi_[k] = hi[k]; }
    for (auto& kv : bricks_) std::memset(kv.second.known, 0, sizeof kv.second.known);
    const int c0[3] = {int(std::floor(lo[0] / kC)) - 2, int(std::floor(lo[1] / kC)) - 2, int(std::floor(lo[2] / kC)) - 2};
    const int c1[3] = {int(std::ceil(hi[0] / kC)) + 2, int(std::ceil(hi[1] / kC)) + 2, int(std::ceil(hi[2] / kC)) + 2};
    for (int z = floor_div(c0[2], kBrick); z <= floor_div(c1[2], kBrick); ++z)
        for (int y = floor_div(c0[1], kBrick); y <= floor_div(c1[1], kBrick); ++y)
            for (int x = floor_div(c0[0], kBrick); x <= floor_div(c1[0], kBrick); ++x) bricks_.erase(key(x, y, z));
    for (int z = c0[2]; z <= c1[2]; ++z)
        for (int x = c0[0]; x <= c1[0]; ++x) floor_.erase(cell_key(x, 0, z));
}

// Under the shared edit lock. A cell IS an edit lattice point (0.5 m), where
// the LOD 0 mesher reads the field.
float SeaAccess::lattice(int lx, int ly, int lz) const {
    float d = 0.0f;
    if (edits_ && edits_->edited_at(lx, ly, lz, d)) return d;
    return worldgen::terrain_density(world_->world(), world_->params(), world_->table(), 0.5f * float(lx),
                                     0.5f * float(ly), 0.5f * float(lz), 0.5f);
}

bool SeaAccess::has_edits_locked(int bx, int by, int bz) const {
    if (!edits_ || edits_->brick_count() == 0) return false;
    std::vector<BrickKey> touched;
    const float m = float(kBrick) * kC;
    const float lo[3] = {float(bx) * m - 1.0f, float(by) * m - 1.0f, float(bz) * m - 1.0f};
    const float hi[3] = {lo[0] + m + 2.0f, lo[1] + m + 2.0f, lo[2] + m + 2.0f};
    edits_->bricks_in_box(lo, hi, touched);
    return !touched.empty();
}

const SeaAccess::Column& SeaAccess::column(int bx, int bz) {
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
            const float x = float(bx * kBrick + i) * kC, z = float(bz * kBrick + k2) * kC;
            const float r = std::sqrt(x * x + z * z) / wp.radius;
            const float h = worldgen::terrain_height(w, wp, t, x, z, r, 0.5f);
            c.h[k2 * kBrick + i] = h;
            c.hmin = (std::min)(c.hmin, h);
            c.hmax = (std::max)(c.hmax, h);
        }
    // As sky_access.cpp: the overhang term's largest reach here, + 1 m.
    const float m = float(kBrick) * kC;
    float strength = 0.0f;
    const float px[5] = {0.0f, 8.0f, 0.0f, 8.0f, 4.0f}, pz[5] = {0.0f, 0.0f, 8.0f, 8.0f, 4.0f};
    for (int s = 0; s < 5; ++s) {
        int b[8];
        float wt[8];
        const int n = worldgen::biome_mix(w, wp, float(bx) * m + px[s], float(bz) * m + pz[s], b, wt);
        for (int j = 0; j < n; ++j)
            if (wt[j] > 0.0f && b[j] >= 0) strength = (std::max)(strength, t.biomes[size_t(b[j])].overhang_strength);
    }
    c.reach = strength * wp.overhang_scale + 1.0f;
    if (placed_)
        c.cave_lo = worldgen::cave_bottom_in_box(w, float(bx) * m - 1.0f, float(bz) * m - 1.0f,
                                                 float(bx) * m + m + 1.0f, float(bz) * m + m + 1.0f, 1.0f);
    return columns_.emplace(k, c).first->second;
}

float SeaAccess::column_h(int x, int z) {
    const int bx = floor_div(x, kBrick), bz = floor_div(z, kBrick);
    return column(bx, bz).h[(z - bz * kBrick) * kBrick + (x - bx * kBrick)];
}

void SeaAccess::evaluate_batch(const std::vector<int>& bxyz) {
    struct Job { uint8_t* out; int x, y, z; };
    std::vector<Job> jobs;
    std::shared_lock<std::shared_mutex> lk(edits_mutex_);
    for (size_t n = 0; n + 2 < bxyz.size(); n += 3) {
        const int bx = bxyz[n], by = bxyz[n + 1], bz = bxyz[n + 2];
        const int64_t k = key(bx, by, bz);
        if (bricks_.count(k)) continue;
        Brick& b = bricks_[k];
        std::memset(b.label, 0, sizeof b.label);
        std::memset(b.arrive, 0, sizeof b.arrive);
        std::memset(b.known, 0, sizeof b.known);
        const Column& c = column(bx, bz);
        const bool has_edits = has_edits_locked(bx, by, bz);
        const float deep = caves_ ? cave_depth_ : 0.0f;
        for (int k2 = 0; k2 < kBrick; ++k2)
            for (int i = 0; i < kBrick; ++i) {
                const int x = bx * kBrick + i, z = bz * kBrick + k2;
                const float h = c.h[k2 * kBrick + i];
                float open_above = h + c.reach;
                if (has_edits) {
                    float top = 0.0f;
                    if (edits_->column_top(x, z, top)) open_above = (std::max)(open_above, top + 1.0f);
                }
                for (int j = 0; j < kBrick; ++j) {
                    const int y = by * kBrick + j;
                    const float wy = float(y) * kC;
                    const int idx = (k2 * kBrick + j) * kBrick + i;
                    if (wy - 0.25f > open_above) {
                        b.st[idx] = below_sea(y) ? kSeaFixed : kSkyFixed;
                        continue;
                    }
                    if (!has_edits && wy + 0.25f < h - c.reach - deep - 1.0f && wy + 0.25f < c.cave_lo) {
                        b.st[idx] = kRock;
                        continue;
                    }
                    b.st[idx] = kAir;   // decided below
                    jobs.push_back({&b.st[idx], x, y, z});
                }
            }
    }
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [this](const Job& j) {
        *j.out = lattice(j.x, j.y, j.z) > 0.0f ? uint8_t(kRock) : uint8_t(kAir);
    });
    points_evaluated_ += jobs.size();
}

SeaAccess::Brick* SeaAccess::brick(int bx, int by, int bz) {
    auto it = bricks_.find(key(bx, by, bz));
    if (it != bricks_.end()) return &it->second;
    std::vector<int> want = {bx, by, bz};
    for (int n = 0; n < 6; ++n) {
        const int x = bx + kD6[n][0], y = by + kD6[n][1], z = bz + kD6[n][2];
        if (!bricks_.count(key(x, y, z))) { want.push_back(x); want.push_back(y); want.push_back(z); }
    }
    evaluate_batch(want);
    return &bricks_.find(key(bx, by, bz))->second;
}

SeaAccess::Brick* SeaAccess::brick_of(int x, int y, int z, int& idx) {
    const int bx = floor_div(x, kBrick), by = floor_div(y, kBrick), bz = floor_div(z, kBrick);
    idx = ((z - bz * kBrick) * kBrick + (y - by * kBrick)) * kBrick + (x - bx * kBrick);
    return brick(bx, by, bz);
}

// The lowest cell of column (x, z) with nothing but air straight above it
// to where the column is open by construction.
int SeaAccess::open_floor(int x, int z) {
    const int64_t k = cell_key(x, 0, z);
    auto it = floor_.find(k);
    if (it != floor_.end()) return it->second;
    float open_above = column_h(x, z) + column(floor_div(x, kBrick), floor_div(z, kBrick)).reach;
    {
        std::shared_lock<std::shared_mutex> lk(edits_mutex_);
        float top = 0.0f;
        if (edits_ && edits_->brick_count() > 0 && edits_->column_top(x, z, top))
            open_above = (std::max)(open_above, top + 1.0f);
    }
    int y = int(std::floor((open_above + 0.25f) / kC)) + 1;
    for (int guard = 0; guard < 4096; ++guard) {
        int idx;
        const Brick* b = brick_of(x, y - 1, z, idx);
        if (b->st[idx] == kRock) break;
        --y;
    }
    floor_[k] = y;
    return y;
}

}  // namespace engine
