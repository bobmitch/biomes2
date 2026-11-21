// ---------------------------------------------------------------------------
// What Lua may ask of the engine: the `engine` module.
//
// The engine side is a table of callbacks and a registry of TUNABLES -- named
// numbers with a range, a getter and a setter -- filled in by the window loop
// (main.cpp), which owns the renderer, the camera and the window. Scripts
// never see a C++ object. They see numbers by name, so a panel can be built
// from the registry without the script knowing what any of them mean, and a
// new knob is one line in C++.
//
// Lua functions (all under the global `engine`):
//   engine.quit()
//   engine.set_paused(bool), engine.paused() -> bool
//   engine.capture_mouse(bool)
//   engine.tunables() -> { {id, group, label, kind, min, max, step, value,
//                           unit, choices = {..}}, ... }
//   engine.get(id) -> number, engine.set(id, number)
//   engine.display() -> {width, height, mode, vsync, forced}   mode: "windowed" | "borderless"
//   engine.display_modes() -> { {width, height}, ... }  what this monitor offers
//   engine.apply_display{width=, height=, mode=, vsync=}
//   engine.log(text)
//   engine.time() -> seconds since start (wall clock, runs while paused)
// ---------------------------------------------------------------------------
#ifndef ENGINE_UI_SCRIPT_API_H
#define ENGINE_UI_SCRIPT_API_H

#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace engine {
namespace ui {

enum class TunableKind { Range, Toggle, Choice };

struct Tunable {
    std::string id;       // stable: settings files and scripts use it
    std::string group;    // "Graphics" is the options screen; the rest is the tuning panel
    std::string label;
    TunableKind kind = TunableKind::Range;
    float min = 0.0f, max = 1.0f, step = 0.01f;
    std::string unit;                     // shown after the value
    std::vector<std::string> choices;     // Choice: value i is choices[i]
    std::function<float()> get;
    std::function<void(float)> set;
};

struct DisplayState {
    int width = 1280, height = 720;
    bool borderless = false;
    bool vsync = true;
    bool forced = false;   // --res on the command line: saved settings must not override it
};

struct Host {
    std::vector<Tunable> tunables;
    std::function<void()> quit;
    std::function<void(bool)> set_paused;
    std::function<bool()> paused;
    std::function<void(bool)> capture_mouse;
    std::function<DisplayState()> display;
    std::function<std::vector<std::pair<int, int> >()> display_modes;
    std::function<void(const DisplayState&)> apply_display;

    const Tunable* find(const std::string& id) const {
        for (const Tunable& t : tunables)
            if (t.id == id) return &t;
        return nullptr;
    }
};

// Registers the `engine` table in L. The host must outlive the state.
void open_engine_module(lua_State* L, Host* host);

}  // namespace ui
}  // namespace engine

#endif
