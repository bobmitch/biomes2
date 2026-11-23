#include "engine/ui/ui.h"
#include "engine/ui/ui_render_d3d12.h"

#include <RmlUi/Core.h>
#include <RmlUi/Lua.h>
#include <RmlUi/Lua/IncludeLua.h>
#include <RmlUi_Platform_Win32.h>

#include <cstdio>

namespace engine {
namespace ui {

namespace {

const char* kLogPath = "build/ui_log.txt";
const char* kMainScript = "game/scripts/main.lua";

// The Win32 system interface (time, cursors, clipboard), logging to a file:
// the engine is a GUI-subsystem binary with no console, and a script error
// that only reached OutputDebugString would be invisible.
class System : public SystemInterface_Win32 {
public:
    System() { log_ = std::fopen(kLogPath, "w"); }
    ~System() override {
        if (log_) std::fclose(log_);
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        const char* tag = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? "ERROR"
                          : type == Rml::Log::LT_WARNING                            ? "warn "
                                                                                    : "info ";
        if (log_) {
            std::fprintf(log_, "%s %s\n", tag, message.c_str());
            std::fflush(log_);
        }
        OutputDebugStringA((message + "\n").c_str());
        return true;   // never break into a debugger over a script error
    }

private:
    FILE* log_ = nullptr;
};

int traceback(lua_State* s) {
    luaL_traceback(s, s, lua_tostring(s, 1), 1);
    return 1;
}

// Call game.<fn>(args...) -- the arguments already pushed -- with a traceback
// on error. Leaves `nres` results on success, nothing on failure.
bool call_game(lua_State* L, const char* fn, int nargs, int nres) {
    const int base = lua_gettop(L) - nargs + 1;   // the first argument
    lua_pushcfunction(L, traceback);
    lua_getglobal(L, "game");
    if (!lua_istable(L, -1)) {
        lua_settop(L, base - 1);
        return false;
    }
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base - 1);
        return false;
    }
    lua_insert(L, base);   // fn, args..., handler
    lua_insert(L, base);   // handler, fn, args...
    const int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "game.%s: %s", fn, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace

struct Ui::Impl {
    System system;
    RenderD3D12 render;
    TextInputMethodEditor_Win32 ime;
    lua_State* L = nullptr;
    bool rml_up = false;
    HWND hwnd = nullptr;
};

Ui::Ui() = default;
Ui::~Ui() { shutdown(); }

bool Ui::init(HWND hwnd, ID3D12Device* device, UINT width, UINT height, Host* host, std::string& error) {
    impl_.reset(new Impl());
    host_ = host;
    impl_->hwnd = hwnd;
    impl_->system.SetWindow(hwnd);
    if (!impl_->render.init(device, DXGI_FORMAT_R8G8B8A8_UNORM, error)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "%s", error.c_str());
        impl_->system.LogMessage(Rml::Log::LT_ERROR, error);
        impl_.reset();
        return false;
    }
    Rml::SetSystemInterface(&impl_->system);
    Rml::SetRenderInterface(&impl_->render);
    Rml::SetTextInputHandler(&impl_->ime);
    if (!Rml::Initialise()) {
        error = "Rml::Initialise failed";
        impl_->render.shutdown();
        impl_.reset();
        return false;
    }
    impl_->rml_up = true;

    // Our own state, so its lifetime is ours: closed after Rml::Shutdown.
    impl_->L = luaL_newstate();
    luaL_openlibs(impl_->L);
    Rml::Lua::Initialise(impl_->L);
    open_engine_module(impl_->L, host_);

    context_ = Rml::CreateContext("main", Rml::Vector2i(int(width), int(height)));
    if (!context_) {
        error = "Rml::CreateContext failed";
        shutdown();
        return false;
    }
    // Documents are authored in dp for 1080 lines.
    context_->SetDensityIndependentPixelRatio(Rml::Math::Max(0.75f, float(height) / 1080.0f));

    if (!Rml::Lua::Interpreter::LoadFile(kMainScript)) {
        error = std::string("could not run ") + kMainScript + " (see " + kLogPath + ")";
        shutdown();
        return false;
    }
    Rml::Log::Message(Rml::Log::LT_INFO, "ui up: %ux%u, %d documents", width, height, context_->GetNumDocuments());
    return true;
}

void Ui::shutdown() {
    if (!impl_) return;
    if (impl_->rml_up) {
        Rml::Shutdown();
        impl_->rml_up = false;
    }
    context_ = nullptr;
    if (impl_->L) {
        lua_close(impl_->L);
        impl_->L = nullptr;
    }
    impl_->render.shutdown();
    impl_.reset();
}

bool Ui::modal() const { return host_ && host_->paused && host_->paused(); }

bool Ui::call_on_key(const char* name) {
    if (!impl_ || !impl_->L) return false;
    lua_pushstring(impl_->L, name);
    if (!call_game(impl_->L, "on_key", 1, 1)) return false;
    const bool consumed = lua_toboolean(impl_->L, -1) != 0;
    lua_pop(impl_->L, 1);
    return consumed;
}

bool Ui::on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool mouse_captured) {
    if (!context_) return false;

    // The keys the script owns, whatever else is going on.
    if (msg == WM_KEYDOWN) {
        const char* name = wp == VK_ESCAPE ? "escape" : wp == VK_F1 ? "f1" : wp == VK_OEM_3 ? "grave" : nullptr;
        if (name && !(lp & (1 << 30)) && call_on_key(name)) return true;   // not on auto-repeat
    }

    const bool is_mouse = msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL || msg == WM_MOUSELEAVE ||
                          (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK);
    const bool is_key = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                        (msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) ||
                        msg == WM_IME_CHAR || msg == WM_IME_REQUEST;
    if (!is_mouse && !is_key) return false;

    const bool paused = modal();
    if (is_key) {
        if (!paused) return false;
        RmlWin32::WindowProcedure(context_, impl_->ime, hwnd, msg, wp, lp);
        return true;   // nothing reaches the game behind a menu
    }

    if (mouse_captured) return false;
    // The swapchain can be smaller than the window (borderless at a lower
    // resolution, stretched), so window pixels are scaled to the context's.
    if (msg != WM_MOUSEWHEEL && msg != WM_MOUSELEAVE) {
        RECT cr;
        GetClientRect(hwnd, &cr);
        const Rml::Vector2i dim = context_->GetDimensions();
        const int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw > 0 && ch > 0 && (cw != dim.x || ch != dim.y)) {
            const int x = int(short(LOWORD(lp))) * dim.x / cw;
            const int y = int(short(HIWORD(lp))) * dim.y / ch;
            lp = MAKELPARAM(WORD(short(x)), WORD(short(y)));
        }
    }
    // "Propagating" is RmlUi's "the mouse is over no element": a click on
    // no element is the game's (it captures the mouse).
    const bool propagating = RmlWin32::WindowProcedure(context_, impl_->ime, hwnd, msg, wp, lp);
    return paused || !propagating;
}

bool Ui::run(const std::string& code) {
    if (!context_ || code.empty()) return false;
    const bool ok = Rml::Lua::Interpreter::DoString(code, "--ui");
    context_->Update();
    return ok;
}

void Ui::update(float dt) {
    if (!context_) return;
    lua_pushnumber(impl_->L, dt);
    call_game(impl_->L, "on_update", 1, 0);
    context_->Update();
}

void Ui::draw(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height) {
    if (!context_) return;
    const Rml::Vector2i dim = context_->GetDimensions();
    if (dim.x != int(width) || dim.y != int(height)) {
        context_->SetDimensions(Rml::Vector2i(int(width), int(height)));
        context_->SetDensityIndependentPixelRatio(Rml::Math::Max(0.75f, float(height) / 1080.0f));
        context_->Update();
    }
    impl_->render.begin_frame(cmd, rtv, width, height);
    context_->Render();
    impl_->render.end_frame();
}

}  // namespace ui
}  // namespace engine
