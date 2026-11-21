#include "engine/ui/script_api.h"

#include <RmlUi/Core/Log.h>
#include <RmlUi/Lua/IncludeLua.h>

#include <windows.h>

namespace engine {
namespace ui {

namespace {

const char* kHostKey = "biomes2.host";

Host* host_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    Host* h = static_cast<Host*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return h;
}

const char* kind_name(TunableKind k) {
    return k == TunableKind::Toggle ? "toggle" : k == TunableKind::Choice ? "choice" : "range";
}

int l_quit(lua_State* L) {
    Host* h = host_of(L);
    if (h && h->quit) h->quit();
    return 0;
}

int l_set_paused(lua_State* L) {
    Host* h = host_of(L);
    if (h && h->set_paused) h->set_paused(lua_toboolean(L, 1) != 0);
    return 0;
}

int l_paused(lua_State* L) {
    Host* h = host_of(L);
    lua_pushboolean(L, h && h->paused && h->paused());
    return 1;
}

int l_capture_mouse(lua_State* L) {
    Host* h = host_of(L);
    if (h && h->capture_mouse) h->capture_mouse(lua_toboolean(L, 1) != 0);
    return 0;
}

int l_tunables(lua_State* L) {
    Host* h = host_of(L);
    lua_newtable(L);
    if (!h) return 1;
    int i = 1;
    for (const Tunable& t : h->tunables) {
        lua_newtable(L);
        lua_pushstring(L, t.id.c_str());      lua_setfield(L, -2, "id");
        lua_pushstring(L, t.group.c_str());   lua_setfield(L, -2, "group");
        lua_pushstring(L, t.label.c_str());   lua_setfield(L, -2, "label");
        lua_pushstring(L, kind_name(t.kind)); lua_setfield(L, -2, "kind");
        lua_pushnumber(L, t.min);             lua_setfield(L, -2, "min");
        lua_pushnumber(L, t.max);             lua_setfield(L, -2, "max");
        lua_pushnumber(L, t.step);            lua_setfield(L, -2, "step");
        lua_pushstring(L, t.unit.c_str());    lua_setfield(L, -2, "unit");
        lua_pushnumber(L, t.get ? t.get() : 0.0f);
        lua_setfield(L, -2, "value");
        lua_newtable(L);
        for (size_t c = 0; c < t.choices.size(); ++c) {
            lua_pushstring(L, t.choices[c].c_str());
            lua_rawseti(L, -2, lua_Integer(c + 1));
        }
        lua_setfield(L, -2, "choices");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int l_get(lua_State* L) {
    Host* h = host_of(L);
    const char* id = luaL_checkstring(L, 1);
    const Tunable* t = h ? h->find(id) : nullptr;
    if (!t || !t->get) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, t->get());
    return 1;
}

int l_set(lua_State* L) {
    Host* h = host_of(L);
    const char* id = luaL_checkstring(L, 1);
    // Booleans are accepted for toggles, so a script can say set(id, true).
    const float v = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1.0f : 0.0f) : float(luaL_checknumber(L, 2));
    const Tunable* t = h ? h->find(id) : nullptr;
    if (!t || !t->set) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "engine.set: no tunable '%s'", id);
        lua_pushboolean(L, 0);
        return 1;
    }
    float c = v < t->min ? t->min : (v > t->max ? t->max : v);
    t->set(c);
    lua_pushboolean(L, 1);
    return 1;
}

void push_display(lua_State* L, const DisplayState& d) {
    lua_newtable(L);
    lua_pushinteger(L, d.width);  lua_setfield(L, -2, "width");
    lua_pushinteger(L, d.height); lua_setfield(L, -2, "height");
    lua_pushstring(L, d.borderless ? "borderless" : "windowed");
    lua_setfield(L, -2, "mode");
    lua_pushboolean(L, d.vsync);  lua_setfield(L, -2, "vsync");
    lua_pushboolean(L, d.forced); lua_setfield(L, -2, "forced");
}

int l_display(lua_State* L) {
    Host* h = host_of(L);
    push_display(L, h && h->display ? h->display() : DisplayState());
    return 1;
}

int l_display_modes(lua_State* L) {
    Host* h = host_of(L);
    lua_newtable(L);
    if (!h || !h->display_modes) return 1;
    int i = 1;
    for (const std::pair<int, int>& m : h->display_modes()) {
        lua_newtable(L);
        lua_pushinteger(L, m.first);  lua_setfield(L, -2, "width");
        lua_pushinteger(L, m.second); lua_setfield(L, -2, "height");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int l_apply_display(lua_State* L) {
    Host* h = host_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    DisplayState d = h && h->display ? h->display() : DisplayState();
    lua_getfield(L, 1, "width");
    if (lua_isnumber(L, -1)) d.width = int(lua_tointeger(L, -1));
    lua_getfield(L, 1, "height");
    if (lua_isnumber(L, -1)) d.height = int(lua_tointeger(L, -1));
    lua_getfield(L, 1, "mode");
    if (lua_isstring(L, -1)) d.borderless = std::string(lua_tostring(L, -1)) == "borderless";
    lua_getfield(L, 1, "vsync");
    if (lua_isboolean(L, -1)) d.vsync = lua_toboolean(L, -1) != 0;
    lua_pop(L, 4);
    if (h && h->apply_display) h->apply_display(d);
    return 0;
}

int l_log(lua_State* L) {
    Rml::Log::Message(Rml::Log::LT_INFO, "[lua] %s", luaL_tolstring(L, 1, nullptr));
    lua_pop(L, 1);
    return 0;
}

int l_time(lua_State* L) {
    static LARGE_INTEGER f = {}, t0 = {};
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    lua_pushnumber(L, double(now.QuadPart - t0.QuadPart) / double(f.QuadPart));
    return 1;
}

}  // namespace

void open_engine_module(lua_State* L, Host* host) {
    lua_pushlightuserdata(L, host);
    lua_setfield(L, LUA_REGISTRYINDEX, kHostKey);
    static const luaL_Reg fns[] = {
        {"quit", l_quit},
        {"set_paused", l_set_paused},
        {"paused", l_paused},
        {"capture_mouse", l_capture_mouse},
        {"tunables", l_tunables},
        {"get", l_get},
        {"set", l_set},
        {"display", l_display},
        {"display_modes", l_display_modes},
        {"apply_display", l_apply_display},
        {"log", l_log},
        {"time", l_time},
        {nullptr, nullptr},
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "engine");
}

}  // namespace ui
}  // namespace engine
