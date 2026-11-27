// ---------------------------------------------------------------------------
// The UI and scripting layer: RmlUi documents driven by Lua.
//
// C++ owns the frame, the window and input routing; Lua owns what the UI does.
// game/scripts/main.lua is the entry point. It defines a global `game` table
// whose hooks this class calls:
//
//   game.on_key(name) -> bool   "escape", "f1", "grave", "tab", "1".."9" -- true if consumed
//   game.on_update(dt)          every frame, with the WALL clock's dt
//
// Input: while the game is paused the UI takes the keyboard and the mouse.
// Otherwise it takes the mouse only while the mouse is free (not captured for
// looking), so the tuning panel can be used over the running world, and a
// click that lands on no element falls through to the game (and captures).
//
// Headless modes never create one: a --shot holds no UI.
// ---------------------------------------------------------------------------
#ifndef ENGINE_UI_UI_H
#define ENGINE_UI_UI_H

#include "engine/ui/script_api.h"

#include <windows.h>
#include <d3d12.h>

#include <memory>
#include <string>

namespace Rml { class Context; }

namespace engine {
namespace ui {

class Ui {
public:
    Ui();
    ~Ui();

    // Fails softly: the engine runs without a UI, and the reason is in
    // build/ui_log.txt as well as `error`.
    bool init(HWND hwnd, ID3D12Device* device, UINT width, UINT height, Host* host, std::string& error);
    void shutdown();   // after the GPU is idle
    bool ready() const { return context_ != nullptr; }

    // Win32 messages, before the game sees them. True: the UI consumed it.
    bool on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool mouse_captured);
    void update(float dt);
    // Runs a chunk of Lua in the UI's state (--shot --ui). Errors go to the log.
    bool run(const std::string& code);
    // The renderer's overlay hook.
    void draw(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height);

private:
    bool call_on_key(const char* name);
    bool modal() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    Rml::Context* context_ = nullptr;
    Host* host_ = nullptr;
};

}  // namespace ui
}  // namespace engine

#endif
