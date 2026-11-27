#include "engine/nav/nav_world.h"

#include "worldgen/world.h"

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <cmath>

namespace engine {
namespace nav {

NavWorld::~NavWorld() { shutdown(); }

bool NavWorld::init(const worldgen::SeedWorld* w, const worldgen::WorldParams* wp, const worldgen::ArchetypeTable* t,
                    float zone_size, int threads, std::string& error) {
    shutdown();
    w_ = w;
    wp_ = wp;
    t_ = t;
    size_ = zone_size;
    // Tile coordinates kept positive: zone x + origin_.
    origin_ = int(std::ceil(wp->radius / zone_size)) + 2;
    mesh_ = dtAllocNavMesh();
    query_ = dtAllocNavMeshQuery();
    if (!mesh_ || !query_) { error = "dtAlloc"; return false; }
    dtNavMeshParams p = {};
    p.orig[0] = -float(origin_) * size_;
    p.orig[1] = 0.0f;
    p.orig[2] = -float(origin_) * size_;
    p.tileWidth = size_;
    p.tileHeight = size_;
    // 256 tiles leaves 14 bits of poly index: 16384 polys a tile.
    p.maxTiles = 256;
    p.maxPolys = 1 << 14;
    if (dtStatusFailed(mesh_->init(&p))) { error = "dtNavMesh::init"; return false; }
    if (dtStatusFailed(query_->init(mesh_, 8192))) { error = "dtNavMeshQuery::init"; return false; }
    stop_ = false;
    for (int i = 0; i < (threads < 1 ? 1 : threads); ++i) threads_.emplace_back([this] { worker(); });
    return true;
}

void NavWorld::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (std::thread& th : threads_) th.join();
    threads_.clear();
    for (Result& r : results_) dtFree(r.out.data);
    results_.clear();
    tiles_.clear();
    wanted_.clear();
    if (query_) { dtFreeNavMeshQuery(query_); query_ = nullptr; }
    if (mesh_) { dtFreeNavMesh(mesh_); mesh_ = nullptr; }
    stats_ = Stats();
}

void NavWorld::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++busy_;
        }
        Result r;
        r.key = job.key;
        r.gen = job.gen;
        r.ok = build_tile(*w_, *wp_, *t_, job.in, r.out, r.error);
        {
            std::lock_guard<std::mutex> lk(mu_);
            --busy_;
            if (stop_) { dtFree(r.out.data); return; }
            results_.push_back(std::move(r));
        }
    }
}

void NavWorld::drop(const Key& k) {
    auto it = tiles_.find(k);
    if (it == tiles_.end()) return;
    if (it->second.ref) mesh_->removeTile(dtTileRef(it->second.ref), nullptr, nullptr);
    tiles_.erase(it);
}

void NavWorld::update(float x, float z, float radius) {
    if (!mesh_) return;
    // Finished tiles in.
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        done.swap(results_);
    }
    for (Result& r : done) {
        auto it = tiles_.find(r.key);
        if (it == tiles_.end()) { dtFree(r.out.data); continue; }   // dropped meanwhile
        State& s = it->second;
        s.pending = false;
        if (!r.ok) {
            ++stats_.failed;
            stats_.last_error = r.error;
            dtFree(r.out.data);
            s.have = s.want;   // do not retry every frame
            s.present = true;
            continue;
        }
        ++stats_.built;
        stats_.ms_total += r.out.ms;
        stats_.ms_max = (std::max)(stats_.ms_max, r.out.ms);
        stats_.samples += r.out.samples;
        if (r.gen < s.want) { dtFree(r.out.data); continue; }   // stale: a newer one is wanted
        if (s.ref) {
            mesh_->removeTile(dtTileRef(s.ref), nullptr, nullptr);
            s.ref = 0;
        }
        if (r.out.data) {
            dtTileRef ref = 0;
            if (dtStatusFailed(mesh_->addTile(r.out.data, r.out.size, DT_TILE_FREE_DATA, 0, &ref))) {
                dtFree(r.out.data);
                ++stats_.failed;
                stats_.last_error = "addTile";
            } else {
                s.ref = uint64_t(ref);
            }
        }
        s.have = r.gen;
        s.present = true;
    }

    // What is wanted, nearest first.
    const int reach = int(std::ceil(radius / size_)) + 1;
    const int cx = int(std::floor(x / size_)), cz = int(std::floor(z / size_));
    std::vector<std::pair<float, Key> > want;
    for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
            Key k;
            k.x = cx + dx;
            k.z = cz + dz;
            // Distance from (x, z) to the zone's square.
            const float x0 = float(k.x) * size_, z0 = float(k.z) * size_;
            const float qx = (std::max)(x0, (std::min)(x, x0 + size_)), qz = (std::max)(z0, (std::min)(z, z0 + size_));
            const float d = std::sqrt((qx - x) * (qx - x) + (qz - z) * (qz - z));
            if (d <= radius) want.push_back(std::make_pair(d, k));
        }
    std::sort(want.begin(), want.end(), [](const std::pair<float, Key>& a, const std::pair<float, Key>& b) {
        return a.first != b.first ? a.first < b.first : (a.second.z != b.second.z ? a.second.z < b.second.z
                                                                                    : a.second.x < b.second.x);
    });
    wanted_.clear();
    const bool can = !ready_fn_ || ready_fn_();
    for (const auto& wk : want) {
        wanted_.push_back(wk.second);
        State& s = tiles_[wk.second];
        if (s.pending || s.have == s.want || !can) continue;
        Job job;
        job.key = wk.second;
        job.gen = s.want;
        job.in.tx = wk.second.x;
        job.in.tz = wk.second.z;
        job.in.size = size_;
        job.in.tile_x = wk.second.x + origin_;
        job.in.tile_y = wk.second.z + origin_;
        job.in.agent = agent_;
        job.in.sea_level = t_->sea_level;
        if (gather_) gather_(job.in);
        s.pending = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }
    // Far tiles out (not while their job is out: the result finds no state
    // and is freed).
    std::vector<Key> far;
    for (const auto& t : tiles_) {
        const float x0 = float(t.first.x) * size_, z0 = float(t.first.z) * size_;
        const float qx = (std::max)(x0, (std::min)(x, x0 + size_)), qz = (std::max)(z0, (std::min)(z, z0 + size_));
        if (std::sqrt((qx - x) * (qx - x) + (qz - z) * (qz - z)) > radius + 96.0f) far.push_back(t.first);
    }
    for (const Key& k : far) drop(k);

    stats_.tiles = 0;
    stats_.empty = 0;
    stats_.pending = 0;
    stats_.polys = 0;
    for (const auto& t : tiles_) {
        if (t.second.pending) ++stats_.pending;
        if (t.second.ref) {
            ++stats_.tiles;
            const dtMeshTile* mt = mesh_->getTileByRef(dtTileRef(t.second.ref));
            if (mt && mt->header) stats_.polys += mt->header->polyCount;
        } else if (t.second.present) {
            ++stats_.empty;
        }
    }
}

void NavWorld::invalidate_box(const float lo[3], const float hi[3]) {
    // The tile's border reaches a few cells past its zone: widen by 4 m.
    const int x0 = int(std::floor((lo[0] - 4.0f) / size_)), x1 = int(std::floor((hi[0] + 4.0f) / size_));
    const int z0 = int(std::floor((lo[2] - 4.0f) / size_)), z1 = int(std::floor((hi[2] + 4.0f) / size_));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            Key k;
            k.x = x;
            k.z = z;
            auto it = tiles_.find(k);
            if (it != tiles_.end()) ++it->second.want;
        }
}

void NavWorld::invalidate_all() {
    for (auto& t : tiles_) ++t.second.want;
}

bool NavWorld::settled() const {
    if (wanted_.empty()) return false;
    for (const Key& k : wanted_) {
        const auto it = tiles_.find(k);
        if (it == tiles_.end() || it->second.pending || it->second.have != it->second.want) return false;
    }
    return true;
}

bool NavWorld::has_tile(float x, float z) const {
    Key k;
    k.x = int(std::floor(x / size_));
    k.z = int(std::floor(z / size_));
    const auto it = tiles_.find(k);
    return it != tiles_.end() && it->second.present;
}

namespace {

void make_filter(dtQueryFilter& f, unsigned short include, float swim_cost) {
    f.setIncludeFlags(include);
    f.setExcludeFlags(0);
    f.setAreaCost(kAreaGround, 1.0f);
    f.setAreaCost(kAreaWade, 1.5f);
    f.setAreaCost(kAreaSwim, swim_cost);
}

}  // namespace

bool NavWorld::nearest(const float p[3], const float ext[3], unsigned short include, float out[3],
                       unsigned char* area) const {
    if (!query_) return false;
    dtQueryFilter f;
    make_filter(f, include, 3.0f);
    dtPolyRef ref = 0;
    if (dtStatusFailed(query_->findNearestPoly(p, ext, &f, &ref, out)) || !ref) return false;
    if (area) {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusSucceed(mesh_->getTileAndPolyByRef(ref, &tile, &poly))) *area = poly->getArea();
    }
    return true;
}

bool NavWorld::move_along(const float from[3], const float to[3], unsigned short include, float out[3],
                          unsigned char* area) const {
    if (!query_) return false;
    dtQueryFilter f;
    make_filter(f, include, 3.0f);
    const float ext[3] = {1.0f, 3.0f, 1.0f};
    dtPolyRef ref = 0;
    float start[3];
    if (dtStatusFailed(query_->findNearestPoly(from, ext, &f, &ref, start)) || !ref) return false;
    dtPolyRef visited[16];
    int nv = 0;
    float res[3];
    if (dtStatusFailed(query_->moveAlongSurface(ref, start, to, &f, res, visited, &nv, 16)) || nv <= 0) return false;
    const dtPolyRef last = visited[nv - 1];
    float h = res[1];
    if (dtStatusSucceed(query_->getPolyHeight(last, res, &h))) res[1] = h;
    out[0] = res[0];
    out[1] = res[1];
    out[2] = res[2];
    if (area) {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusSucceed(mesh_->getTileAndPolyByRef(last, &tile, &poly))) *area = poly->getArea();
    }
    return true;
}

bool NavWorld::path(const float a[3], const float b[3], unsigned short include, std::vector<float>& corners,
                    float* length, bool* complete, float swim_cost) const {
    corners.clear();
    if (length) *length = 0.0f;
    if (complete) *complete = false;
    if (!query_) return false;
    dtQueryFilter f;
    make_filter(f, include, swim_cost);
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef ra = 0, rb = 0;
    float pa[3], pb[3];
    if (dtStatusFailed(query_->findNearestPoly(a, ext, &f, &ra, pa)) || !ra) return false;
    if (dtStatusFailed(query_->findNearestPoly(b, ext, &f, &rb, pb)) || !rb) rb = 0;
    static const int kMaxPath = 1024;
    std::vector<dtPolyRef> polys(kMaxPath);
    int np = 0;
    if (rb) {
        query_->findPath(ra, rb, pa, pb, &f, polys.data(), &np, kMaxPath);
    } else {
        // b is off the mesh: head for the nearest point we can find to it.
        query_->findPath(ra, ra, pa, pa, &f, polys.data(), &np, kMaxPath);
    }
    if (np <= 0) return false;
    float end[3] = {pb[0], pb[1], pb[2]};
    if (!rb || polys[size_t(np - 1)] != rb) {
        // A partial path: end at the closest point of its last poly.
        query_->closestPointOnPoly(polys[size_t(np - 1)], b, end, nullptr);
    }
    std::vector<float> pts(size_t(kMaxPath) * 3);
    int nc = 0;
    query_->findStraightPath(pa, end, polys.data(), np, pts.data(), nullptr, nullptr, &nc, kMaxPath);
    corners.assign(pts.begin(), pts.begin() + size_t(nc) * 3);
    if (length)
        for (int i = 1; i < nc; ++i) {
            const float* p = &pts[size_t(i - 1) * 3];
            const float* q = &pts[size_t(i) * 3];
            *length += std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                                 (q[2] - p[2]) * (q[2] - p[2]));
        }
    if (complete) {
        const float dx = end[0] - b[0], dz = end[2] - b[2];
        *complete = rb && polys[size_t(np - 1)] == rb && dx * dx + dz * dz < 1.0f;
    }
    return nc > 0;
}

}  // namespace nav
}  // namespace engine
