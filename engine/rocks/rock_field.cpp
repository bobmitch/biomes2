#include "engine/rocks/rock_field.h"
#include "engine/field/world_gpu.h"

#include <algorithm>
#include <cmath>

namespace engine {

void rock_rotation(const worldgen::RockInstance& r, float q[4]) {
    const float yaw = r.yaw01 * 6.2831853f;
    const float tilt = r.tilt01 * 0.14f;
    const float ta = (r.tilt01 * 5.0f - std::floor(r.tilt01 * 5.0f)) * 6.2831853f;
    const float cy = std::cos(0.5f * yaw), sy = std::sin(0.5f * yaw);
    const float cl = std::cos(0.5f * tilt), sl = std::sin(0.5f * tilt);
    const float lx = std::cos(ta) * sl, lz = std::sin(ta) * sl, lw = cl;
    q[0] = lx * cy - lz * sy;
    q[1] = lw * sy;
    q[2] = lx * sy + lz * cy;
    q[3] = lw * cy;
}

RockField::RockField() {}
RockField::~RockField() { shutdown(); }

float RockField::tile_m() const { return kRockTileCells * world_->table().rocks.boulder.cell; }

void RockField::init(const WorldGpu* world, int threads) {
    world_ = world;
    stop_ = false;
    if (!world_->table().rocks.present) return;   // no layer 9, no workers
    for (int i = 0; i < threads; ++i) threads_.push_back(std::thread(&RockField::worker, this));
}

void RockField::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (size_t i = 0; i < threads_.size(); ++i) threads_[i].join();
    threads_.clear();
}

void RockField::worker() {
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
        Tile t;
        const worldgen::RockTable& rt = world_->table().rocks;
        const int n = kRockTileCells;
        worldgen::rocks_in_cells(world_->world(), world_->params(), world_->table(), 0,
                                 k.x * n, k.z * n, k.x * n + n, k.z * n + n, t.rocks);
        // Outcrops whose cell origin (oi * cell) lies in [x0, x1): exactly one
        // tile owns each.
        const float tm = tile_m();
        const float x0 = k.x * tm, x1 = x0 + tm, z0 = k.z * tm, z1 = z0 + tm;
        const float oc = rt.outcrop.cell;
        const int oi0 = int(std::ceil(x0 / oc)), oi1 = int(std::ceil(x1 / oc));
        const int oj0 = int(std::ceil(z0 / oc)), oj1 = int(std::ceil(z1 / oc));
        if (oi1 > oi0 && oj1 > oj0)
            worldgen::rocks_in_cells(world_->world(), world_->params(), world_->table(), 1,
                                     oi0, oj0, oi1, oj1, t.rocks);
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_flight_.erase(k);
            done_.push_back(std::make_pair(k, std::move(t)));
        }
    }
}

bool RockField::update(float x, float z, float radius) {
    if (!world_ || !world_->table().rocks.present) return false;
    const float tm = tile_m();
    const int cx = int(std::floor(x / tm)), cz = int(std::floor(z / tm));
    const int reach = int(std::ceil(radius / tm)) + 1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < done_.size(); ++i) {
            tiles_[done_[i].first] = std::move(done_[i].second);
            dirty_ = true;
        }
        done_.clear();
    }
    if (cx != last_cx_ || cz != last_cz_ || radius != last_radius_) {
        last_cx_ = cx; last_cz_ = cz; last_radius_ = radius;
        std::vector<std::pair<float, TileKey> > want;
        for (int j = cz - reach; j <= cz + reach; ++j) {
            for (int i = cx - reach; i <= cx + reach; ++i) {
                const float tx = (i + 0.5f) * tm - x, tz = (j + 0.5f) * tm - z;
                const float d = std::sqrt(tx * tx + tz * tz) - 0.7072f * tm;
                if (d > radius) continue;
                TileKey k; k.x = i; k.z = j;
                if (tiles_.count(k)) continue;
                want.push_back(std::make_pair(d, k));
            }
        }
        std::sort(want.begin(), want.end(),
                  [](const std::pair<float, TileKey>& a, const std::pair<float, TileKey>& b) { return a.first < b.first; });
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.clear();
            for (size_t i = 0; i < want.size(); ++i)
                if (!in_flight_.count(want[i].second)) queue_.push_back(want[i].second);
        }
        cv_.notify_all();
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            const float tx = (it->first.x + 0.5f) * tm - x, tz = (it->first.z + 0.5f) * tm - z;
            if (std::sqrt(tx * tx + tz * tz) > radius + 1.5f * tm + 0.7072f * tm) {
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

void RockField::rebuild() {
    rocks_.clear();
    for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
        rocks_.insert(rocks_.end(), it->second.rocks.begin(), it->second.rocks.end());
    ++version_;
}

}  // namespace engine
