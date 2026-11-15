#include "engine/trees/tree_field.h"
#include "engine/field/world_gpu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace engine {

void tree_standing_rotation(const worldgen::TreeInstance& t, float q[4]) {
    const float yaw = t.yaw01 * 6.2831853f;
    // Lean axis horizontal, at an angle of its own; up to 3 degrees.
    const float lean = t.lean01 * 0.052f;
    const float la = (t.lean01 * 7.0f - std::floor(t.lean01 * 7.0f)) * 6.2831853f;
    // q = q_lean * q_yaw, with q_yaw = (0, sy, 0, cy) and
    // q_lean = (lx, 0, lz, lw) about a horizontal axis.
    const float cy = std::cos(0.5f * yaw), sy = std::sin(0.5f * yaw);
    const float cl = std::cos(0.5f * lean), sl = std::sin(0.5f * lean);
    const float lx = std::cos(la) * sl, lz = std::sin(la) * sl, lw = cl;
    q[0] = lx * cy - lz * sy;
    q[1] = lw * sy;
    q[2] = lx * sy + lz * cy;
    q[3] = lw * cy;
}

TreeField::TreeField() {}
TreeField::~TreeField() { shutdown(); }

void TreeField::init(const WorldGpu* world, int threads) {
    world_ = world;
    stop_ = false;
    for (int i = 0; i < threads; ++i) threads_.push_back(std::thread(&TreeField::worker, this));
}

void TreeField::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (size_t i = 0; i < threads_.size(); ++i) threads_[i].join();
    threads_.clear();
}

size_t TreeField::tiles_queued() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size() + in_flight_.size();
}

void TreeField::worker() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        TileKey k;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            k = queue_.front();
            queue_.pop_front();
            in_flight_.insert(k);
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        Tile t;
        const int n = kTreeTileCells;
        worldgen::trees_in_cells(world_->world(), world_->params(), world_->table(),
                                 k.x * n, k.z * n, k.x * n + n, k.z * n + n, t.trees);
        QueryPerformanceCounter(&t1);
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_flight_.erase(k);
            done_.push_back(std::make_pair(k, std::move(t)));
            gen_ms_total_ += 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
            ++tiles_generated_;
        }
    }
}

bool TreeField::update(float x, float z, float radius) {
    if (!world_) return false;
    const float tile_m = kTreeTileCells * world_->table().trees.cell;
    const int cx = int(std::floor(x / tile_m)), cz = int(std::floor(z / tile_m));
    const int reach = int(std::ceil(radius / tile_m)) + 1;

    // Finished tiles.
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < done_.size(); ++i) {
            tiles_[done_[i].first] = std::move(done_[i].second);
            dirty_ = true;
        }
        done_.clear();
    }

    // Requeue when the camera crosses a tile or the radius changes: nearest
    // missing tiles first, so what is about to be looked at arrives first.
    if (cx != last_cx_ || cz != last_cz_ || radius != last_radius_) {
        last_cx_ = cx; last_cz_ = cz; last_radius_ = radius;
        std::vector<std::pair<float, TileKey> > want;
        for (int j = cz - reach; j <= cz + reach; ++j) {
            for (int i = cx - reach; i <= cx + reach; ++i) {
                const float tx = (i + 0.5f) * tile_m - x, tz = (j + 0.5f) * tile_m - z;
                const float d = std::sqrt(tx * tx + tz * tz) - 0.7072f * tile_m;
                if (d > radius) continue;
                TileKey k; k.x = i; k.z = j;
                if (tiles_.count(k)) continue;
                want.push_back(std::make_pair(d, k));
            }
        }
        std::sort(want.begin(), want.end(),
                  [](const std::pair<float, TileKey>& a, const std::pair<float, TileKey>& b) {
                      return a.first < b.first;
                  });
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.clear();
            for (size_t i = 0; i < want.size(); ++i) {
                if (!in_flight_.count(want[i].second)) queue_.push_back(want[i].second);
            }
        }
        cv_.notify_all();

        // Drop tiles a tile and a half beyond the radius: far enough that
        // turning round does not regenerate them, near enough to bound memory.
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            const float tx = (it->first.x + 0.5f) * tile_m - x, tz = (it->first.z + 0.5f) * tile_m - z;
            if (std::sqrt(tx * tx + tz * tz) > radius + 1.5f * tile_m + 0.7072f * tile_m) {
                it = tiles_.erase(it);
                dirty_ = true;
            } else {
                ++it;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        settled_ = queue_.empty() && in_flight_.empty() && done_.empty();
    }

    if (dirty_) {
        rebuild();
        dirty_ = false;
        return true;
    }
    return false;
}

void TreeField::set_felled(uint32_t id, bool felled) {
    if (felled) felled_.insert(id); else felled_.erase(id);
    rebuild();
}

void TreeField::rebuild() {
    standing_.clear();
    for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
        const std::vector<worldgen::TreeInstance>& ts = it->second.trees;
        for (size_t i = 0; i < ts.size(); ++i) {
            if (!felled_.count(ts[i].id)) standing_.push_back(ts[i]);
        }
    }
    ++version_;
}

}  // namespace engine
