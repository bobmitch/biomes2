#include "engine/mobs/minds.h"

#include "engine/mobs/locomotion.h"
#include "engine/nav/nav_world.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {
namespace mobs {

namespace {

const char* kMeta = "biomes.creature";
const char* kLogPath = "build/creature_log.txt";

uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

}  // namespace

// The creature handle's methods. Each finds its creature by id, so a handle
// kept past its creature's freezing raises an error rather than touching
// freed memory.
struct MindsApi {
    static Minds* self(lua_State* L) { return static_cast<Minds*>(lua_touserdata(L, lua_upvalueindex(1))); }
    static uint32_t id_of(lua_State* L) { return *static_cast<uint32_t*>(luaL_checkudata(L, 1, kMeta)); }
    static Mob& mob(lua_State* L) {
        const uint32_t id = id_of(L);
        Mob* m = self(L)->creatures_->find_live(id);
        if (!m) luaL_error(L, "creature %d is not live", int(id));
        return *m;
    }
    static int push3(lua_State* L, const float v[3]) {
        lua_pushnumber(L, v[0]);
        lua_pushnumber(L, v[1]);
        lua_pushnumber(L, v[2]);
        return 3;
    }
    static int id(lua_State* L) { lua_pushinteger(L, lua_Integer(mob(L).id)); return 1; }
    static int kind(lua_State* L) {
        const Mob& m = mob(L);
        lua_pushstring(L, self(L)->table_->kinds[size_t(m.kind)].id.c_str());
        return 1;
    }
    static int pos(lua_State* L) { return push3(L, mob(L).pos); }
    static int home(lua_State* L) { return push3(L, mob(L).home); }
    static int yaw(lua_State* L) { lua_pushnumber(L, mob(L).yaw); return 1; }
    static int state(lua_State* L) { lua_pushinteger(L, mob(L).state); return 1; }
    static int set_state(lua_State* L) {
        Mob& m = mob(L);
        const int s = int(luaL_checkinteger(L, 2));
        if (s != m.state) {
            m.state = s;
            m.state_t = 0.0f;
        }
        return 0;
    }
    static int state_time(lua_State* L) { lua_pushnumber(L, mob(L).state_t); return 1; }
    static int mark(lua_State* L) { return push3(L, mob(L).mark); }
    static int set_mark(lua_State* L) {
        Mob& m = mob(L);
        for (int k = 0; k < 3; ++k) m.mark[k] = float(luaL_checknumber(L, 2 + k));
        return 0;
    }
    static int move_to(lua_State* L) {
        Mob& m = mob(L);
        for (int k = 0; k < 3; ++k) m.goal[k] = float(luaL_checknumber(L, 2 + k));
        const char* g = luaL_optstring(L, 5, "walk");
        m.gait = std::strcmp(g, "run") == 0 ? 2 : 1;
        return 0;
    }
    static int stop(lua_State* L) { mob(L).gait = 0; return 0; }
    static int moving(lua_State* L) { lua_pushboolean(L, mob(L).gait != 0); return 1; }
    static int face(lua_State* L) {
        Mob& m = mob(L);
        self(L)->loco_->face(m, float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)));
        return 0;
    }
    static int player(lua_State* L) {
        mob(L);
        const Minds::Player& p = self(L)->player_;
        if (!p.present) { lua_pushnil(L); return 1; }
        return push3(L, p.feet);
    }
    static int player_distance(lua_State* L) {
        const Mob& m = mob(L);
        const Minds::Player& p = self(L)->player_;
        if (!p.present) { lua_pushnil(L); return 1; }
        const float dx = p.feet[0] - m.pos[0], dy = p.feet[1] - m.pos[1], dz = p.feet[2] - m.pos[2];
        lua_pushnumber(L, std::sqrt(dx * dx + dy * dy + dz * dz));
        return 1;
    }
    static float random01(Minds* M, const Mob& m) {
        Minds::Mind& d = M->minds_[m.id];
        return float(mix(uint64_t(m.id) * 0x9E3779B1u ^ (uint64_t(++d.rolls) << 32)) >> 40) / float(1ull << 24);
    }
    static int random(lua_State* L) {
        const Mob& m = mob(L);
        lua_pushnumber(L, random01(self(L), m));
        return 1;
    }
    static int random_point(lua_State* L) {
        const Mob& m = mob(L);
        Minds* M = self(L);
        const float r = float(luaL_optnumber(L, 2, 15.0));
        if (!M->nav_) { lua_pushnil(L); return 1; }
        for (int t = 0; t < 6; ++t) {
            const float a = random01(M, m) * 6.2831853f, d = r * std::sqrt(random01(M, m));
            const float p[3] = {m.home[0] + std::sin(a) * d, m.home[1], m.home[2] + std::cos(a) * d};
            const float ext[3] = {2.0f, 6.0f, 2.0f};
            float q[3];
            if (M->nav_->nearest(p, ext, M->loco_->include_for(m.kind) & nav::kFlagWalk, q)) return push3(L, q);
        }
        lua_pushnil(L);
        return 1;
    }
    static int log(lua_State* L) {
        const Mob& m = mob(L);
        char head[64];
        std::snprintf(head, sizeof head, "#%u: ", m.id);
        self(L)->log(head + std::string(luaL_checkstring(L, 2)));
        return 0;
    }
};

Minds::~Minds() { shutdown(); }

void Minds::shutdown() {
    if (L_) lua_close(L_);
    L_ = nullptr;
    minds_.clear();
    scripted_.clear();
}

void Minds::log(const std::string& s) {
    if (FILE* f = std::fopen(kLogPath, "a")) {
        std::fprintf(f, "%s\n", s.c_str());
        std::fclose(f);
    }
}

bool Minds::init(const CreatureTable* t, Creatures* c, Locomotion* loco, nav::NavWorld* nav, std::string& error) {
    shutdown();
    table_ = t;
    creatures_ = c;
    loco_ = loco;
    nav_ = nav;
    std::remove(kLogPath);
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    lua_pushstring(L_, "game/scripts/?.lua;game/scripts/creatures/?.lua");
    lua_setglobal(L_, "CREATURE_PATH");
    luaL_dostring(L_, "package.path = CREATURE_PATH .. ';' .. package.path");

    // The handle's metatable: methods with this Minds as their upvalue.
    static const luaL_Reg kMethods[] = {
        {"id", MindsApi::id}, {"kind", MindsApi::kind}, {"pos", MindsApi::pos}, {"home", MindsApi::home},
        {"yaw", MindsApi::yaw}, {"state", MindsApi::state}, {"set_state", MindsApi::set_state},
        {"state_time", MindsApi::state_time}, {"mark", MindsApi::mark}, {"set_mark", MindsApi::set_mark},
        {"move_to", MindsApi::move_to}, {"stop", MindsApi::stop}, {"moving", MindsApi::moving},
        {"face", MindsApi::face}, {"player", MindsApi::player}, {"player_distance", MindsApi::player_distance},
        {"random", MindsApi::random}, {"random_point", MindsApi::random_point}, {"log", MindsApi::log},
        {nullptr, nullptr}};
    luaL_newmetatable(L_, kMeta);
    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    luaL_setfuncs(L_, kMethods, 1);
    lua_setfield(L_, -2, "__index");
    lua_pop(L_, 1);

    // Every kind's script: a table of hooks, kept in the registry by kind.
    lua_newtable(L_);
    scripted_.assign(t->kinds.size(), false);
    bool ok = true;
    for (size_t k = 0; k < t->kinds.size(); ++k) {
        const std::string& sc = t->kinds[k].script;
        if (sc.empty()) continue;
        const std::string path = "game/scripts/" + sc;
        lua_pushcfunction(L_, traceback);
        if (luaL_loadfile(L_, path.c_str()) != LUA_OK || lua_pcall(L_, 0, 1, -2) != LUA_OK) {
            const std::string e = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?";
            log(path + ": " + e);
            if (ok) error = path + ": " + e;
            ok = false;
            lua_pop(L_, 2);
            continue;
        }
        if (!lua_istable(L_, -1)) {
            log(path + ": does not return a table of hooks");
            if (ok) error = path + ": does not return a table";
            ok = false;
            lua_pop(L_, 2);
            continue;
        }
        lua_rawseti(L_, -3, lua_Integer(k));   // kinds[k] = the table
        lua_pop(L_, 1);                        // the traceback
        scripted_[k] = true;
    }
    lua_setfield(L_, LUA_REGISTRYINDEX, "biomes.kinds");
    return ok;
}

const Minds::Mind* Minds::mind(uint32_t id) const {
    const auto it = minds_.find(id);
    return it == minds_.end() ? nullptr : &it->second;
}

// kinds[kind][hook](handle, ev): `nev` named numbers as an event table, or
// with nev < 0 one plain number (on_think's dt).
void Minds::call(int kind, const char* hook, Mob& m, int nev, const float* ev, const char* const* names) {
    if (!L_ || !scripted(kind)) return;
    const int top = lua_gettop(L_);
    lua_pushcfunction(L_, traceback);
    lua_getfield(L_, LUA_REGISTRYINDEX, "biomes.kinds");
    lua_rawgeti(L_, -1, lua_Integer(kind));
    lua_getfield(L_, -1, hook);
    if (!lua_isfunction(L_, -1)) { lua_settop(L_, top); return; }
    uint32_t* u = static_cast<uint32_t*>(lua_newuserdatauv(L_, sizeof(uint32_t), 0));
    *u = m.id;
    luaL_setmetatable(L_, kMeta);
    int nargs = 1;
    if (nev < 0) {
        lua_pushnumber(L_, ev[0]);
        ++nargs;
    } else if (nev > 0) {
        lua_createtable(L_, 0, nev);
        for (int i = 0; i < nev; ++i) {
            lua_pushnumber(L_, ev[i]);
            lua_setfield(L_, -2, names[i]);
        }
        ++nargs;
    }
    if (lua_pcall(L_, nargs, 0, top + 1) != LUA_OK) {
        ++errors_;
        if (errors_ <= 50) {
            char head[96];
            std::snprintf(head, sizeof head, "%s.%s #%u: ", table_->kinds[size_t(kind)].id.c_str(), hook, m.id);
            log(head + std::string(lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
        }
        m.gait = 0;
    }
    lua_settop(L_, top);
}

void Minds::update(float dt, const Player& p, const ClearFn& clear) {
    player_ = p;
    if (!creatures_ || !table_) return;
    std::vector<Mob>& live = creatures_->live();
    // Forget what is no longer live.
    std::unordered_map<uint32_t, bool> alive;
    for (const Mob& m : live) alive[m.id] = true;
    for (auto it = minds_.begin(); it != minds_.end();) it = alive.count(it->first) ? std::next(it) : minds_.erase(it);
    if (dt <= 0.0f) return;

    static const char* const kSee[] = {"x", "y", "z", "dist"};
    static const char* const kHear[] = {"x", "y", "z", "dist", "db"};
    for (size_t i = 0; i < live.size(); ++i) {
        Mob& m = live[i];
        Mind& d = minds_[m.id];
        if (!d.awake) {
            d.awake = true;
            d.until = float(m.id % 7u) * 0.03f;   // out of step with each other
            call(m.kind, "on_wake", m, 0, nullptr, nullptr);
        }
        d.since += dt;
        d.until -= dt;
        if (d.until > 0.0f) continue;
        const Locomotion::Body* b = loco_ ? loco_->body(m.id) : nullptr;
        d.until = (b && b->close) ? 0.2f : 1.0f;
        const float tick = d.since;
        d.since = 0.0f;
        ++d.ticks;

        const CreatureKind& K = table_->kinds[size_t(m.kind)];
        bool seeing = false, hearing = false;
        float dist = 0.0f, db = perception::kSilent;
        if (p.present) {
            const float eye[3] = {m.pos[0], m.pos[1] + 1.6f, m.pos[2]};
            const float chest[3] = {p.feet[0], p.feet[1] + 1.2f, p.feet[2]};
            const float to[3] = {chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2]};
            dist = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            const float fwd[3] = {std::sin(m.yaw), 0.0f, std::cos(m.yaw)};
            const float light = perception::light_factor(p.lux, K.dark_gain, light_);
            const float range = perception::sight_range(K.view_range_m, p.visibility, light);
            // The cone is the HORIZONTAL field of view, and the kind says how
            // far up and down it looks (view_vertical_deg; 90 or more, any
            // way). As a 3D cone, a player on a slope two metres off, or lower
            // down 30 m away, fell out of 45 degrees.
            const float flat[3] = {to[0], 0.0f, to[2]};
            const float h = std::sqrt(to[0] * to[0] + to[2] * to[2]);
            const bool vertical_ok = K.view_vertical_deg >= 90.0f ||
                                     std::fabs(to[1]) <= h * std::tan(K.view_vertical_deg * 0.017453293f);
            if (dist <= range && perception::in_cone(fwd, flat, K.view_cone_deg) && vertical_ok) {
                // A clear line from the eye, or from a head's width either
                // side of it, to the chest or the head: one point for an eye
                // let a trunk beside the creature blind it to a whole sector.
                const float head[3] = {p.feet[0], p.feet[1] + 1.7f, p.feet[2]};
                const float side[3] = {std::cos(m.yaw) * 0.35f, 0.0f, -std::sin(m.yaw) * 0.35f};
                for (int e = 0; e < 3 && !seeing; ++e) {
                    const float s = e == 0 ? 0.0f : (e == 1 ? 1.0f : -1.0f);
                    const float from[3] = {eye[0] + side[0] * s, eye[1], eye[2] + side[2] * s};
                    seeing = !clear || clear(from, chest) || clear(from, head);
                }
            }
            if (!seeing && perception::hears(p.level_db, dist, K.hear_range_m, K.hear_threshold_db)) {
                hearing = true;
                db = perception::level_at(p.level_db, dist);
            }
        }
        if (seeing) {
            for (int k = 0; k < 3; ++k) d.seen[k] = p.feet[k];
            const float ev[4] = {p.feet[0], p.feet[1], p.feet[2], dist};
            call(m.kind, "on_see", m, 4, ev, kSee);
        } else if (d.seeing) {
            const float ev[4] = {d.seen[0], d.seen[1], d.seen[2], 0.0f};
            call(m.kind, "on_lose", m, 3, ev, kSee);
        }
        if (hearing) {
            const float ev[5] = {p.feet[0], p.feet[1], p.feet[2], dist, db};
            call(m.kind, "on_hear", m, 5, ev, kHear);
        }
        d.seeing = seeing;
        d.hearing = hearing;
        call(m.kind, "on_think", m, -1, &tick, nullptr);
    }
}

}  // namespace mobs
}  // namespace engine
