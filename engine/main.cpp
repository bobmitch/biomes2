// ---------------------------------------------------------------------------
// B1 -- "App, swapchain, trivial analytic field. Something on screen."
//
// Windows-only, like tools/view. CI stays headless and gates worldgen/ on both
// Linux and Windows, so nothing about determinism coverage depends on this
// binary. See ADR-003.
//
// Run from the repository root: the shader is compiled from source at startup
// and its path is relative, the same convention validate.exe uses for
// archetypes.json and the golden hashes.
// ---------------------------------------------------------------------------
#include "engine/render/renderer.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

engine::Renderer g_renderer;
bool g_paused = false;
bool g_start_mesh = false;
bool g_start_overlay = false;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_renderer.resize(LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
            if (wp == VK_SPACE)  { g_paused = !g_paused; return 0; }
            if (wp == 'M') {
                // The comparison this milestone turns on: same camera, same
                // field, marched or meshed or both at once.
                const engine::Mode m = g_renderer.mode();
                g_renderer.set_mode(m == engine::Mode::Raymarch ? engine::Mode::Mesh
                                  : m == engine::Mode::Mesh     ? engine::Mode::Overlay
                                                                : engine::Mode::Raymarch);
                return 0;
            }
            if (wp == VK_F5) {
                // Shader hot-reload. Cheap here and the thing B2 will lean on
                // hardest -- a kernel is mostly shader edits.
                std::string err;
                if (!g_renderer.reload_shaders(err)) {
                    MessageBoxA(hwnd, err.c_str(), "Shader reload failed",
                                MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

struct Vec3 { float x, y, z; };

Vec3 sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 norm(Vec3 v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l <= 0.0f) return Vec3{0.0f, 0.0f, 1.0f};
    return Vec3{v.x / l, v.y / l, v.z / l};
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    // --shot <path>: render one frame at a fixed camera and write it out,
    // without showing the window. For looking at what the engine drew from a
    // headless context, and the obvious basis for comparing B2's mesh against
    // B1's raymarch of the same field.
    std::string shot_path;
    {
        const std::string cl = cmdline ? cmdline : "";
        const size_t at = cl.find("--shot");
        if (at != std::string::npos) {
            size_t p = cl.find_first_not_of(" \t", at + 6);
            if (p != std::string::npos) {
                size_t e = cl.find_first_of(" \t", p);
                shot_path = cl.substr(p, e == std::string::npos ? e : e - p);
            }
            if (shot_path.empty()) shot_path = "build/b1.bmp";
        }
        if (cl.find("--mesh") != std::string::npos) g_start_mesh = true;
        if (cl.find("--overlay") != std::string::npos) g_start_overlay = true;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "biomes2_engine";
    RegisterClassA(&wc);

    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("biomes2_engine", "biomes2 engine - B1 trivial field",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    std::string error;
    if (!g_renderer.init(hwnd, error)) {
        MessageBoxA(hwnd, error.c_str(), "Renderer init failed", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!shot_path.empty()) {
        // A fixed camera, so two runs produce the same image and a diff means
        // something changed rather than that time moved on.
        const Vec3 target{0.0f, 0.0f, 0.0f};
        const Vec3 pos{10.0f, 22.0f, 8.0f};
        const Vec3 fwd = norm(sub(target, pos));
        const Vec3 right = norm(cross(fwd, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = cross(right, fwd);
        engine::ViewConstants v = {};
        v.cam_pos[0] = pos.x;   v.cam_pos[1] = pos.y;   v.cam_pos[2] = pos.z;
        v.cam_fwd[0] = fwd.x;   v.cam_fwd[1] = fwd.y;   v.cam_fwd[2] = fwd.z;
        v.cam_right[0] = right.x; v.cam_right[1] = right.y; v.cam_right[2] = right.z;
        v.cam_up[0] = up.x;     v.cam_up[1] = up.y;     v.cam_up[2] = up.z;
        v.time = 0.0f;
        v.aspect = 1280.0f / 720.0f;
        v.tan_half_fov = 0.5773503f;

        std::string err;
        if (g_start_mesh) g_renderer.set_mode(engine::Mode::Mesh);
        if (g_start_overlay) g_renderer.set_mode(engine::Mode::Overlay);
        const bool ok = g_renderer.capture(v, shot_path.c_str(), err);

        // Stats beside the image. A windowed build puts these in the title
        // bar, which a headless capture never sees.
        {
            const std::string sp = shot_path + ".txt";
            FILE* f = std::fopen(sp.c_str(), "w");
            if (f) {
                float mx = 0.0f, mn = 0.0f;
                unsigned checked = 0;
                g_renderer.check_against_field(mx, mn, checked);
                std::fprintf(f, "vertices %u\nindices %u\ntriangles %u\nmesh_ms %.4f\n",
                             g_renderer.mesh_vertices(), g_renderer.mesh_indices(),
                             g_renderer.mesh_indices() / 3, g_renderer.mesh_ms());
                std::fprintf(f, "field residual at %u vertices: max %.4f m, mean %.4f m"
                                " (voxel 0.500 m)\n", checked, mx, mn);
                std::fclose(f);
            }
        }
        g_renderer.shutdown();
        if (!ok) {
            MessageBoxA(nullptr, err.c_str(), "Capture failed", MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    if (g_start_mesh) g_renderer.set_mode(engine::Mode::Mesh);
    if (g_start_overlay) g_renderer.set_mode(engine::Mode::Overlay);
    ShowWindow(hwnd, show);

    const LARGE_INTEGER zero = {};
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    (void)zero;

    float t = 0.0f;
    double last = 0.0;
    double fps_accum = 0.0;
    int    fps_frames = 0;

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            continue;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) /
                               static_cast<double>(freq.QuadPart);
        const double dt = elapsed - last;
        last = elapsed;
        if (!g_paused) t += static_cast<float>(dt);

        RECT cr;
        GetClientRect(hwnd, &cr);
        const float w = static_cast<float>(cr.right - cr.left);
        const float h = static_cast<float>(cr.bottom - cr.top);
        if (w < 1.0f || h < 1.0f) continue;

        // A slow orbit. Free flight arrives at B3, where its exit criterion is
        // holding frame rate while moving through a streamed world -- there is
        // nothing to stream yet, so a fixed path is the honest version.
        const float a = t * 0.18f;
        const Vec3 target{0.0f, 0.0f, 0.0f};
        const Vec3 pos{target.x + std::cos(a) * 13.0f,
                       22.0f,
                       target.z + std::sin(a) * 13.0f};
        const Vec3 fwd = norm(sub(target, pos));
        const Vec3 right = norm(cross(fwd, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = cross(right, fwd);

        engine::ViewConstants view = {};
        view.cam_pos[0] = pos.x;   view.cam_pos[1] = pos.y;   view.cam_pos[2] = pos.z;
        view.cam_fwd[0] = fwd.x;   view.cam_fwd[1] = fwd.y;   view.cam_fwd[2] = fwd.z;
        view.cam_right[0] = right.x; view.cam_right[1] = right.y; view.cam_right[2] = right.z;
        view.cam_up[0] = up.x;     view.cam_up[1] = up.y;     view.cam_up[2] = up.z;
        view.time = t;
        view.aspect = w / h;
        view.tan_half_fov = 0.5773503f;   // 60 degrees vertical

        g_renderer.render(view);

        fps_accum += dt;
        ++fps_frames;
        if (fps_accum >= 0.5) {
            char title[160];
            std::snprintf(title, sizeof(title),
                          "biomes2 engine - B2 - %s - %.0f fps - %u verts, %u tris,"
                          " meshed in %.3f ms - M switches, F5 reloads",
                          g_renderer.mode() == engine::Mode::Mesh    ? "mesh"
                          : g_renderer.mode() == engine::Mode::Overlay ? "overlay"
                                                                      : "raymarch",
                          fps_frames / fps_accum,
                          g_renderer.mesh_vertices(), g_renderer.mesh_indices() / 3,
                          g_renderer.mesh_ms());
            SetWindowTextA(hwnd, title);
            fps_accum = 0.0;
            fps_frames = 0;
        }
    }

    g_renderer.shutdown();
    return 0;
}
