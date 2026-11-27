#include "engine/mobs/creatures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {
namespace mobs {

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

float u01(uint64_t h) { return float(h >> 40) / float(1ull << 24); }

const uint32_t kMagic = 0x31424F4Du;   // "MOB1"
const uint32_t kVersion = 3;   // 2: gait and goal; 3: mark

bool in_hours(float hour, float from, float to) {
    if (from <= 0.0f && to >= 24.0f) return true;
    return from <= to ? (hour >= from && hour < to) : (hour >= from || hour < to);
}

}  // namespace

void Creatures::set_world(uint64_t field_id) {
    world_ = field_id;
    clear();
}

void Creatures::clear() {
    zones_.clear();
    active_.clear();
    live_.clear();
    next_id_ = 1;
    last_ = -1.0;
    stats_ = Stats();
}

ZoneKey Creatures::zone_of(float x, float z) const {
    const float s = table_ ? table_->zones.size_m : 64.0f;
    ZoneKey k;
    k.x = int(std::floor(x / s));
    k.z = int(std::floor(z / s));
    return k;
}

void Creatures::zone_centre(const ZoneKey& k, float& x, float& z) const {
    const float s = table_ ? table_->zones.size_m : 64.0f;
    x = (float(k.x) + 0.5f) * s;
    z = (float(k.z) + 0.5f) * s;
}

bool Creatures::active(const ZoneKey& k) const { return active_.count(k) != 0; }

uint64_t Creatures::roll(const ZoneKey& k, uint64_t a, uint64_t b) const {
    uint64_t h = mix64(world_ ^ 0xC2EA7u);
    h = mix64(h ^ (uint64_t(uint32_t(k.x)) << 32 | uint32_t(k.z)));
    h = mix64(h ^ a);
    return mix64(h ^ b);
}

Creatures::ZoneRec& Creatures::record(const ZoneKey& k) {
    ZoneRec& z = zones_[k];
    const size_t n = table_ ? table_->spawns.size() : 0;
    if (z.wait.size() != n) {
        // A zone first seen (or a table whose rules changed): each rule's
        // first attempt somewhere in its first interval, so zones that wake
        // together do not all try at once.
        z.wait.assign(n, 0.0);
        for (size_t r = 0; r < n; ++r)
            z.wait[r] = double(table_->spawns[r].interval_h) * 3600.0 * (0.25 + 0.75 * u01(roll(k, r, 0xBEEFu)));
    }
    return z;
}

size_t Creatures::frozen_count() const {
    size_t n = 0;
    for (const auto& z : zones_) n += z.second.frozen.size();
    return n;
}

int Creatures::live_of_kind(int kind) const {
    int n = 0;
    for (const Mob& m : live_) n += m.kind == kind ? 1 : 0;
    return n;
}

int Creatures::in_zone(const ZoneKey& k, int kind) const {
    int n = 0;
    for (const Mob& m : live_)
        if (m.kind == kind && zone_of(m.pos[0], m.pos[2]) == k) ++n;
    return n;
}

double Creatures::zone_wait(const ZoneKey& k, int rule) const {
    const auto it = zones_.find(k);
    if (it == zones_.end() || rule < 0 || size_t(rule) >= it->second.wait.size()) return -1.0;
    return it->second.wait[size_t(rule)];
}

uint32_t Creatures::spawn_at(int kind, const float pos[3], float yaw, double now) {
    if (!table_ || kind < 0 || size_t(kind) >= table_->kinds.size()) return 0;
    Mob m;
    m.id = next_id_++;
    m.kind = kind;
    std::memcpy(m.pos, pos, sizeof m.pos);
    std::memcpy(m.home, pos, sizeof m.home);
    std::memcpy(m.goal, pos, sizeof m.goal);
    m.yaw = yaw;
    m.health = table_->kinds[size_t(kind)].health;
    m.born = now;
    live_.push_back(m);
    return m.id;
}

Mob* Creatures::find_live(uint32_t id) {
    for (Mob& m : live_)
        if (m.id == id) return &m;
    return nullptr;
}

void Creatures::freeze(const ZoneKey& k) {
    ZoneRec& z = record(k);
    for (size_t i = 0; i < live_.size();) {
        if (zone_of(live_[i].pos[0], live_[i].pos[2]) == k) {
            z.frozen.push_back(live_[i]);
            live_.erase(live_.begin() + std::ptrdiff_t(i));
            ++stats_.frozen;
        } else {
            ++i;
        }
    }
}

void Creatures::thaw(const ZoneKey& k) {
    const auto it = zones_.find(k);
    if (it == zones_.end()) return;
    for (const Mob& m : it->second.frozen) live_.push_back(m);
    stats_.thawed += it->second.frozen.size();
    it->second.frozen.clear();
}

void Creatures::update(double now, const float player[3], const SpawnEnv& env) {
    if (!table_) return;
    const ZoneTuning& zt = table_->zones;
    double dt = last_ < 0.0 ? 0.0 : now - last_;
    if (dt < 0.0) dt = 0.0;
    last_ = now;

    // Which zones are awake: centres within active_m, or within release_m
    // for those already awake.
    const int reach = int(std::ceil(zt.release_m / zt.size_m)) + 1;
    const ZoneKey pz = zone_of(player[0], player[2]);
    std::vector<ZoneKey> want;
    for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
            ZoneKey k;
            k.x = pz.x + dx;
            k.z = pz.z + dz;
            float cx, cz;
            zone_centre(k, cx, cz);
            const float d = std::sqrt((cx - player[0]) * (cx - player[0]) + (cz - player[2]) * (cz - player[2]));
            if (d <= zt.active_m || (active(k) && d <= zt.release_m)) want.push_back(k);
        }
    std::unordered_map<ZoneKey, bool, ZoneKeyHash> next;
    for (const ZoneKey& k : want) next[k] = true;
    std::vector<ZoneKey> leaving;
    for (const auto& a : active_)
        if (!next.count(a.first)) leaving.push_back(a.first);
    for (const ZoneKey& k : leaving) freeze(k);
    for (const ZoneKey& k : want)
        if (!active(k)) thaw(k);
    active_.swap(next);
    // A creature standing in a zone that is not awake (it walked out of the
    // active area) freezes where it is, with that zone.
    for (size_t i = 0; i < live_.size();) {
        const ZoneKey k = zone_of(live_[i].pos[0], live_[i].pos[2]);
        if (!active(k)) {
            record(k).frozen.push_back(live_[i]);
            live_.erase(live_.begin() + std::ptrdiff_t(i));
            ++stats_.frozen;
        } else {
            ++i;
        }
    }

    // The spawn clocks of awake zones, in a fixed order (the caps make the
    // order matter).
    std::sort(want.begin(), want.end(), [](const ZoneKey& a, const ZoneKey& b) {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
    for (const ZoneKey& k : want) {
        ZoneRec& z = record(k);
        for (size_t r = 0; r < table_->spawns.size(); ++r) {
            z.wait[r] -= dt;
            if (z.wait[r] > 0.0) continue;
            const double interval = double(table_->spawns[r].interval_h) * 3600.0;
            // One attempt a call: a clock jump does not fire a burst.
            z.wait[r] += interval;
            if (z.wait[r] < 0.0) z.wait[r] = interval;
            try_spawn(k, z, int(r), now, player, env);
        }
    }
}

void Creatures::try_spawn(const ZoneKey& k, ZoneRec& z, int rule, double now, const float player[3],
                          const SpawnEnv& env) {
    const SpawnRule& R = table_->spawns[size_t(rule)];
    const CreatureKind& K = table_->kinds[size_t(R.kind)];
    ++z.attempts;
    ++stats_.attempts;
    const uint64_t h = roll(k, uint64_t(rule), z.attempts);
    const float hour = float(std::fmod(now / 3600.0, 24.0));
    if (!in_hours(hour, R.hour_from, R.hour_to)) { ++stats_.out_of_hours; return; }
    if (u01(h) >= R.chance) return;
    ++stats_.rolls_won;
    const int n = R.group_min + int((h >> 8) % uint64_t(R.group_max - R.group_min + 1));
    const float size = table_->zones.size_m;
    for (int g = 0; g < n; ++g) {
        if (in_zone(k, R.kind) >= R.max_per_zone || live_of_kind(R.kind) >= R.max_active) {
            ++stats_.capped;
            return;
        }
        bool placed = false;
        for (int t = 0; t < 12 && !placed; ++t) {
            const uint64_t hp = roll(k, uint64_t(rule) * 1000u + uint64_t(g) + 1u, uint64_t(z.attempts) * 64u + uint64_t(t));
            const float x = (float(k.x) + u01(hp)) * size;
            const float zz = (float(k.z) + u01(mix64(hp))) * size;
            float y = 0.0f;
            if (env.ground && !env.ground(x, zz, y)) continue;
            if (env.biome_weight && env.biome_weight(x, zz, R.biome) < R.min_weight) continue;
            const float px = x - player[0], pz = zz - player[2];
            if (px * px + pz * pz < R.min_player_m * R.min_player_m) continue;
            bool crowded = false;
            for (const Mob& m : live_) {
                if (m.kind != R.kind) continue;
                const float dx = m.pos[0] - x, dz = m.pos[2] - zz;
                if (dx * dx + dz * dz < R.min_same_m * R.min_same_m) { crowded = true; break; }
            }
            if (crowded) continue;
            const float feet[3] = {x, y, zz}, head[3] = {x, y + 1.7f, zz};
            if (env.visible && env.visible(feet, head)) continue;
            Mob m;
            m.id = next_id_++;
            m.kind = R.kind;
            std::memcpy(m.pos, feet, sizeof feet);
            std::memcpy(m.home, feet, sizeof feet);
            m.yaw = u01(mix64(hp ^ 0x5EEDu)) * 6.2831853f;
            m.health = K.health;
            m.born = now;
            live_.push_back(m);
            ++stats_.spawned;
            placed = true;
        }
        if (!placed) { ++stats_.no_point; return; }
    }
}

// --- save / load --------------------------------------------------------------

namespace {

template <class T> void put(FILE* f, const T& v) { std::fwrite(&v, sizeof v, 1, f); }
template <class T> bool get(FILE* f, T& v) { return std::fread(&v, sizeof v, 1, f) == 1; }

void put_mob(FILE* f, const Mob& m, uint16_t kind) {
    put(f, m.id);
    put(f, kind);
    for (int k = 0; k < 3; ++k) put(f, m.pos[k]);
    put(f, m.yaw);
    put(f, m.health);
    for (int k = 0; k < 3; ++k) put(f, m.home[k]);
    put(f, int32_t(m.state));
    put(f, m.state_t);
    put(f, m.born);
    put(f, int32_t(m.gait));
    for (int k = 0; k < 3; ++k) put(f, m.goal[k]);
    for (int k = 0; k < 3; ++k) put(f, m.mark[k]);
}

bool get_mob(FILE* f, Mob& m, uint16_t& kind, uint32_t version) {
    int32_t st = 0, gait = 0;
    bool ok = get(f, m.id) && get(f, kind);
    for (int k = 0; k < 3 && ok; ++k) ok = get(f, m.pos[k]);
    ok = ok && get(f, m.yaw) && get(f, m.health);
    for (int k = 0; k < 3 && ok; ++k) ok = get(f, m.home[k]);
    ok = ok && get(f, st) && get(f, m.state_t) && get(f, m.born);
    m.state = st;
    if (version >= 2) {
        ok = ok && get(f, gait);
        for (int k = 0; k < 3 && ok; ++k) ok = get(f, m.goal[k]);
        m.gait = gait;
        if (version >= 3)
            for (int k = 0; k < 3 && ok; ++k) ok = get(f, m.mark[k]);
    } else {
        std::memcpy(m.goal, m.pos, sizeof m.goal);
    }
    return ok;
}

}  // namespace

bool Creatures::save(const char* path, std::string& error) const {
    if (!table_) { error = "creatures: no table"; return false; }
    FILE* f = std::fopen(path, "wb");
    if (!f) { error = std::string("cannot write ") + path; return false; }
    put(f, kMagic);
    put(f, kVersion);
    put(f, world_);
    put(f, next_id_);
    put(f, uint32_t(table_->kinds.size()));
    for (const CreatureKind& k : table_->kinds) {
        put(f, uint16_t(k.id.size()));
        std::fwrite(k.id.data(), 1, k.id.size(), f);
    }
    // Every zone with a record, live creatures written into theirs as frozen.
    std::unordered_map<ZoneKey, std::vector<const Mob*>, ZoneKeyHash> live_by_zone;
    for (const Mob& m : live_) live_by_zone[zone_of(m.pos[0], m.pos[2])].push_back(&m);
    std::vector<ZoneKey> keys;
    for (const auto& z : zones_) keys.push_back(z.first);
    for (const auto& z : live_by_zone)
        if (!zones_.count(z.first)) keys.push_back(z.first);
    std::sort(keys.begin(), keys.end(), [](const ZoneKey& a, const ZoneKey& b) {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
    put(f, uint32_t(keys.size()));
    for (const ZoneKey& k : keys) {
        put(f, int32_t(k.x));
        put(f, int32_t(k.z));
        const auto it = zones_.find(k);
        const std::vector<double> none;
        const std::vector<double>& wait = it != zones_.end() ? it->second.wait : none;
        put(f, it != zones_.end() ? it->second.attempts : uint32_t(0));
        put(f, uint32_t(wait.size()));
        for (double w : wait) put(f, w);
        const auto lz = live_by_zone.find(k);
        const size_t nf = it != zones_.end() ? it->second.frozen.size() : 0;
        const size_t nl = lz != live_by_zone.end() ? lz->second.size() : 0;
        put(f, uint32_t(nf + nl));
        for (size_t i = 0; i < nf; ++i) put_mob(f, it->second.frozen[i], uint16_t(it->second.frozen[i].kind));
        for (size_t i = 0; i < nl; ++i) put_mob(f, *lz->second[i], uint16_t(lz->second[i]->kind));
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) error = std::string("write failed: ") + path;
    return ok;
}

bool Creatures::load(const char* path, std::string& error) {
    if (!table_) { error = "creatures: no table"; return false; }
    FILE* f = std::fopen(path, "rb");
    if (!f) { error = std::string("cannot read ") + path; return false; }
    auto fail = [&](const std::string& why) { std::fclose(f); error = std::string(path) + ": " + why; return false; };
    uint32_t magic = 0, version = 0, next = 0, nk = 0, nz = 0;
    uint64_t world = 0;
    if (!get(f, magic) || magic != kMagic || !get(f, version) || version < 1 || version > kVersion)
        return fail("not a creature save");
    if (!get(f, world) || world != world_) return fail("a save of another world");
    if (!get(f, next) || !get(f, nk) || nk > 4096) return fail("truncated");
    std::vector<int> kind_map(nk, -1);
    for (uint32_t i = 0; i < nk; ++i) {
        uint16_t len = 0;
        if (!get(f, len)) return fail("truncated");
        std::string id(len, '\0');
        if (len && std::fread(&id[0], 1, len, f) != len) return fail("truncated");
        kind_map[i] = table_->find(id);   // a kind the table no longer has: its creatures go
    }
    if (!get(f, nz)) return fail("truncated");
    std::unordered_map<ZoneKey, ZoneRec, ZoneKeyHash> zones;
    for (uint32_t i = 0; i < nz; ++i) {
        int32_t x = 0, z = 0;
        uint32_t attempts = 0, nw = 0, nm = 0;
        if (!get(f, x) || !get(f, z) || !get(f, attempts) || !get(f, nw) || nw > 4096) return fail("truncated");
        ZoneKey k;
        k.x = x;
        k.z = z;
        ZoneRec& rec = zones[k];
        rec.attempts = attempts;
        rec.wait.resize(nw);
        for (uint32_t w = 0; w < nw; ++w)
            if (!get(f, rec.wait[w])) return fail("truncated");
        if (!get(f, nm) || nm > 1u << 20) return fail("truncated");
        for (uint32_t m = 0; m < nm; ++m) {
            Mob mob;
            uint16_t kind = 0;
            if (!get_mob(f, mob, kind, version)) return fail("truncated");
            if (kind >= nk || kind_map[kind] < 0) continue;
            mob.kind = kind_map[kind];
            rec.frozen.push_back(mob);
        }
    }
    std::fclose(f);
    zones_.swap(zones);
    for (auto& z : zones_)
        if (z.second.wait.size() != table_->spawns.size()) z.second.wait.clear();   // re-staggered by record()
    active_.clear();
    live_.clear();
    next_id_ = next;
    last_ = -1.0;
    return true;
}

}  // namespace mobs
}  // namespace engine
