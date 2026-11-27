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
//
// The player (engine/player/survival.h). Slots are 1..32, the hotbar 1..8:
//   engine.player() -> {health, max_health, stamina, max_stamina, since_stamina, hurt,
//                       dead, walking, day, hour, equipped (slot or 0),
//                       foods = { {id, name, value, share, time_left, can_eat}, ... }}
//   engine.inventory() -> { {id, name, count}, ... } 32 of them, id "" empty
//   engine.item(id) -> {id, name, description, kind, stack, health, stamina,
//                       regen, duration} or nil
//   engine.use_slot(i)            eat a food, equip or put away a tool
//   engine.move_slot(from, to) -> bool, engine.split_slot(from, to) -> bool
//   engine.prompt() -> text       what E would do now, "" for nothing
//   engine.messages() -> { text, ... }  what happened since the last call
//   engine.captured() -> bool     whether the mouse is captured for looking
// An item's icon is the image "icon:<id>" (ui_render_d3d12.cpp).
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

struct FoodView {
    std::string id, name;
    float value = 0.0f, share = 0.0f, time_left = 0.0f;
    bool can_eat = false;
};

struct PlayerView {
    float health = 0.0f, max_health = 0.0f, stamina = 0.0f, max_stamina = 0.0f;
    float since_stamina = 0.0f;
    bool dead = false, walking = false;
    int day = 1;
    float hour = 0.0f;
    int equipped = 0;   // 1-based slot, 0 for empty hands
    std::vector<FoodView> foods;
    // The player as creatures hear and see them (engine/player/stealth.h).
    bool crouching = false;
    float noise_db = -100.0f;   // source level, dB at 1 m; -100 silent
    float visibility = 0.0f;    // posture and speed, 0..1
    float light = 0.0f;         // the light share an ordinary eye gets, 0..1
    float lux = 0.0f;
    // 1 the moment a creature's blow lands, fading over kHurtFade: the
    // HUD's red edge.
    float hurt = 0.0f;
};

struct SlotView {
    std::string id, name;
    int count = 0;
};

struct ItemView {
    bool found = false;
    std::string id, name, description, kind;
    int stack = 1;
    float health = 0.0f, stamina = 0.0f, regen = 0.0f, duration = 0.0f;
};

// An image the UI asks for by name ("icon:<id>"): RGBA8, premultiplied sRGB.
using ImageSource = std::function<bool(const std::string& name, std::vector<unsigned char>& rgba, int& w, int& h)>;

struct Host {
    std::vector<Tunable> tunables;
    std::function<void()> quit;
    std::function<void(bool)> set_paused;
    std::function<bool()> paused;
    std::function<void(bool)> capture_mouse;
    std::function<DisplayState()> display;
    std::function<std::vector<std::pair<int, int> >()> display_modes;
    std::function<void(const DisplayState&)> apply_display;
    // The player. Unset: the functions return nothing.
    std::function<PlayerView()> player;
    std::function<std::vector<SlotView>()> inventory;
    std::function<ItemView(const std::string&)> item;
    std::function<void(int)> use_slot;                 // 0-based
    std::function<bool(int, int)> move_slot;           // 0-based
    std::function<bool(int, int)> split_slot;
    std::function<std::string()> prompt;
    std::function<std::vector<std::string>()> messages;
    std::function<bool()> captured;
    ImageSource image;

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
