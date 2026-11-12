// ---------------------------------------------------------------------------
// The engine's window and frame loop.
//
// Windows-only, like tools/view. CI stays headless and gates worldgen/ on both
// Linux and Windows, so nothing about determinism coverage depends on this
// binary. See ADR-003.
//
// Run from the repository root: shaders are compiled from source at startup
// and their paths are relative, the same convention validate.exe uses for
// archetypes.json and the golden hashes.
//
//   WASD / QE   fly, shift to sprint
//   mouse       look (click to capture, Esc to release)
//   M           raymarch / mesh / wireframe overlay
//   F5          reload every shader and remesh
//   F2          write a capture to build/shot.bmp
// ---------------------------------------------------------------------------
#include "engine/render/renderer.h"
#include "engine/camera.h"
#include "engine/physics/character.h"
#include "engine/field/trivial_field.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

engine::Renderer  g_renderer;
engine::Camera    g_camera;
engine::Character g_character;
bool g_walk = false;
bool g_captured = false;
bool g_want_shot = false;

void set_capture(HWND hwnd, bool on) {
    if (on == g_captured) return;
    g_captured = on;
    ShowCursor(on ? FALSE : TRUE);
    if (on) {
        SetCapture(hwnd);
    } else {
        ReleaseCapture();
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) g_renderer.resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_LBUTTONDOWN:
            set_capture(hwnd, true);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (g_captured) set_capture(hwnd, false);
                else PostQuitMessage(0);
                return 0;
            }
            if (wp == 'M') {
                // The comparison B2 turned on and B3 still needs: same camera,
                // same field, marched or meshed or both at once.
                const engine::Mode m = g_renderer.mode();
                g_renderer.set_mode(m == engine::Mode::Raymarch ? engine::Mode::Mesh
                                  : m == engine::Mode::Mesh     ? engine::Mode::Overlay
                                                                : engine::Mode::Raymarch);
                return 0;
            }
            if (wp == VK_TAB) {
                g_walk = !g_walk;
                if (g_walk) {
                    // Drop in above the camera's ground so the first frames
                    // are a short fall rather than a long one.
                    g_character.x = g_camera.x;
                    g_character.z = g_camera.z;
                    g_character.y = g_camera.y;
                    g_character.vy = 0.0f;
                }
                return 0;
            }
            if (wp == VK_F2) { g_want_shot = true; return 0; }
            if (wp == VK_F5) {
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

void fill_view(const engine::Camera& cam, float w, float h, float t,
               engine::ViewConstants& v) {
    float fwd[3], right[3], up[3];
    cam.basis(fwd, right, up);
    v.cam_pos[0] = cam.x;     v.cam_pos[1] = cam.y;     v.cam_pos[2] = cam.z;
    v.cam_fwd[0] = fwd[0];    v.cam_fwd[1] = fwd[1];    v.cam_fwd[2] = fwd[2];
    v.cam_right[0] = right[0]; v.cam_right[1] = right[1]; v.cam_right[2] = right[2];
    v.cam_up[0] = up[0];      v.cam_up[1] = up[1];      v.cam_up[2] = up[2];
    v.time = t;
    v.aspect = h > 0.0f ? w / h : 1.0f;
    v.tan_half_fov = 0.5773503f;   // 60 degrees vertical
    v.pad0 = 0.0f;
    // The capsule is only drawn in walk mode; a radius of zero switches it off
    // rather than needing a second shader.
    v.cap_pos[0] = g_character.x;
    v.cap_pos[1] = g_character.y;
    v.cap_pos[2] = g_character.z;
    v.cap_r = g_walk ? g_character.radius : 0.0f;
    v.cap_half = g_character.half_height;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    // --shot <path> renders one frame at a fixed camera and writes it out
    // without showing the window, with residency filled first so the image is
    // the finished world rather than eight chunks of it.
    std::string shot_path;
    bool start_overlay = false, start_raymarch = false, debug_layer = false;
    double flight_seconds = 0.0;
    double walk_seconds = 0.0;
    bool flight_vsync = false;
    {
        const std::string cl = cmdline ? cmdline : "";
        const size_t at = cl.find("--shot");
        if (at != std::string::npos) {
            size_t p = cl.find_first_not_of(" \t", at + 6);
            if (p != std::string::npos && cl[p] != '-') {
                size_t e = cl.find_first_of(" \t", p);
                shot_path = cl.substr(p, e == std::string::npos ? e : e - p);
            }
            if (shot_path.empty()) shot_path = "build/shot.bmp";
        }
        const size_t wk = cl.find("--walk");
        if (wk != std::string::npos) {
            walk_seconds = 8.0;
            size_t p = cl.find_first_not_of(" \t", wk + 6);
            if (p != std::string::npos && cl[p] != '-') walk_seconds = std::atof(cl.c_str() + p);
            if (walk_seconds <= 0.0) walk_seconds = 8.0;
        }
        const size_t fl = cl.find("--flight");
        if (fl != std::string::npos) {
            flight_seconds = 8.0;
            size_t p = cl.find_first_not_of(" \t", fl + 8);
            if (p != std::string::npos && cl[p] != '-') flight_seconds = std::atof(cl.c_str() + p);
            if (flight_seconds <= 0.0) flight_seconds = 8.0;
        }
        flight_vsync = cl.find("--vsync") != std::string::npos;
        start_overlay = cl.find("--overlay") != std::string::npos;
        start_raymarch = cl.find("--raymarch") != std::string::npos;
        debug_layer = cl.find("--debuglayer") != std::string::npos;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "biomes2_engine";
    RegisterClassA(&wc);

    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("biomes2_engine", "biomes2 engine",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    std::string error;
    if (!g_renderer.init(hwnd, error, debug_layer)) {
        MessageBoxA(hwnd, error.c_str(), "Renderer init failed", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (start_overlay) g_renderer.set_mode(engine::Mode::Overlay);
    if (start_raymarch) g_renderer.set_mode(engine::Mode::Raymarch);

    if (!shot_path.empty()) {
        g_camera.x = 10.0f; g_camera.y = 46.0f; g_camera.z = -70.0f;
        g_camera.yaw = 0.15f; g_camera.pitch = -0.42f;
        engine::ViewConstants v = {};
        fill_view(g_camera, 1280.0f, 720.0f, 0.0f, v);

        const bool ok = g_renderer.capture(v, shot_path.c_str(), error);
        const engine::FrameStats st = g_renderer.stats();
        float mx = 0.0f, mn = 0.0f;
        unsigned checked = 0;
        g_renderer.check_against_field(mx, mn, checked);
        if (debug_layer) {
            g_renderer.drain_debug_messages("build/d3d12_validation.txt");
            g_renderer.dump_draw_args("build/draw_args.txt");
        }
        g_renderer.shutdown();

        const std::string sp = shot_path + ".txt";
        if (FILE* f = std::fopen(sp.c_str(), "w")) {
            std::fprintf(f, "resident chunks %zu\npending %zu\nfree slots %zu\n",
                         st.resident, st.pending, st.free_slots);
            std::fprintf(f, "vertices %u\nindices %u\ntriangles %u\nslots overflowed %u\n",
                         st.vertices, st.indices, st.indices / 3, st.overflowed);
            std::fprintf(f, "field residual at %u vertices of slot 0: max %.4f m, mean %.4f m\n",
                         checked, mx, mn);
            std::fclose(f);
        }
        if (!ok) {
            MessageBoxA(nullptr, error.c_str(), "Capture failed", MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    ShowWindow(hwnd, show);

    if (walk_seconds > 0.0) {
        g_renderer.set_vsync(true);
        g_walk = true;
        g_character.x = 0.0f; g_character.z = 0.0f; g_character.y = 30.0f;
        g_camera.pitch = -0.12f;

        LARGE_INTEGER wfreq, wstart, wprev;
        QueryPerformanceFrequency(&wfreq);
        QueryPerformanceCounter(&wstart);
        wprev = wstart;

        unsigned long long frames = 0, airborne = 0, waiting = 0, settled_frames = 0;
        double resid_sum = 0.0, resid_max = 0.0;
        double worst_step = 0.0, travelled = 0.0;
        float  last_feet = 0.0f;
        bool   have_last = false;

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - wstart.QuadPart) / double(wfreq.QuadPart);
            const double dt = double(now.QuadPart - wprev.QuadPart) / double(wfreq.QuadPart);
            wprev = now;
            if (total >= walk_seconds) break;

            g_camera.yaw = float(total) * 0.20f;
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            const float sp = 6.0f;
            const float px = g_character.x, pz = g_character.z;
            g_character.update(g_renderer.height_field(), float(dt), sy * sp, cy * sp, false);
            travelled += std::sqrt((g_character.x - px) * (g_character.x - px) +
                                   (g_character.z - pz) * (g_character.z - pz));

            g_camera.x = g_character.x;
            g_camera.y = g_character.y + g_character.eye;
            g_camera.z = g_character.z;

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);

            ++frames;
            if (g_character.waiting_for_ground) ++waiting;
            else if (!g_character.grounded) ++airborne;

            // Give the world a second to stream in and the capsule a moment to
            // land before judging it.
            if (total > 1.5 && g_character.grounded) {
                ++settled_frames;
                const float fy = g_character.feet();
                const float d = trivial_density(g_character.x, fy, g_character.z);
                const double a = d < 0.0f ? -double(d) : double(d);
                resid_sum += a;
                if (a > resid_max) resid_max = a;
                if (have_last) {
                    const double step = std::fabs(double(fy) - double(last_feet));
                    if (step > worst_step) worst_step = step;
                }
                last_feet = fy;
                have_last = true;
            }
        }

        const engine::FrameStats st = g_renderer.stats();
        if (FILE* f = std::fopen("build/walk.txt", "w")) {
            std::fprintf(f, "capsule walk over the trivial world, %.1f s\n", walk_seconds);
            std::fprintf(f, "frames %llu, travelled %.0f m\n", frames, travelled);
            std::fprintf(f, "frames airborne: %llu, frames with no terrain streamed yet: %llu\n",
                         airborne, waiting);
            std::fprintf(f, "settled frames judged: %llu\n", settled_frames);
            std::fprintf(f, "feet vs the field: max %.4f m, mean %.4f m\n",
                         resid_max, settled_frames ? resid_sum / double(settled_frames) : 0.0);
            std::fprintf(f, "worst single-frame vertical step: %.4f m\n", worst_step);
            std::fprintf(f, "LOD changes under the feet: %d\n", g_character.lod_changes);
            std::fprintf(f, "collision patches resident at the end: %zu\n",
                         st.collision_patches);
            std::fprintf(f, "height readback latency: %u frames\n",
                         st.readback_latency_frames);
            std::fclose(f);
        }

        // A third-person look at where the capsule ended up, drawn against the
        // analytic field rather than the mesh -- see the note in the shader.
        {
            engine::Camera c = g_camera;
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            c.x = g_character.x - sy * 7.0f;
            c.y = g_character.y + 3.2f;
            c.z = g_character.z - cy * 7.0f;
            c.pitch = -0.33f;
            engine::ViewConstants v = {};
            fill_view(c, 1280.0f, 720.0f, 0.0f, v);
            g_renderer.set_mode(engine::Mode::Raymarch);
            std::string err;
            g_renderer.capture(v, "build/b4_walk.bmp", err);
        }
        g_renderer.shutdown();
        return 0;
    }

    if (flight_seconds > 0.0) {
        // A long straight run with a slow turn, so chunks enter and leave
        // residency the whole way rather than the camera orbiting inside an
        // already-loaded set.
        // Capped is the exit criterion -- does it HOLD the target -- and
        // uncapped is the headroom behind it. They stress different things:
        // at 60 Hz the camera covers a metre a frame and streaming has real
        // work to do, while uncapped it covers a fraction of a millimetre.
        g_renderer.set_vsync(flight_vsync);
        g_camera.x = 0.0f; g_camera.y = 34.0f; g_camera.z = 0.0f;
        g_camera.pitch = -0.22f;
        g_camera.speed = 60.0f;

        LARGE_INTEGER ffreq, fstart, fprev;
        QueryPerformanceFrequency(&ffreq);
        QueryPerformanceCounter(&fstart);
        fprev = fstart;

        std::vector<double> frame_ms;
        frame_ms.reserve(20000);
        size_t max_pending = 0;
        unsigned long long meshed_total = 0;
        double mesh_ms_sum = 0.0;
        int mesh_ms_n = 0;

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - fstart.QuadPart) / double(ffreq.QuadPart);
            const double dt = double(now.QuadPart - fprev.QuadPart) / double(ffreq.QuadPart);
            fprev = now;
            if (total >= flight_seconds) break;

            // Constant forward motion with a gentle yaw.
            g_camera.yaw = float(total) * 0.25f;
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            const float step = 60.0f * float(dt < 0.1 ? dt : 0.1);
            g_camera.x += fwd[0] * step;
            g_camera.z += fwd[2] * step;

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;

            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);

            const engine::FrameStats st = g_renderer.stats();
            if (st.pending > max_pending) max_pending = st.pending;
            meshed_total += st.meshed_this_frame;
            if (st.mesh_ms > 0.0) { mesh_ms_sum += st.mesh_ms; ++mesh_ms_n; }
            // The first frames include pipeline warm-up and an empty world.
            if (total > 0.5) frame_ms.push_back(dt * 1000.0);
        }

        const engine::FrameStats st = g_renderer.stats();
        std::sort(frame_ms.begin(), frame_ms.end());
        FILE* f = std::fopen("build/flight.txt", "w");
        if (f && !frame_ms.empty()) {
            double sum = 0.0;
            for (size_t i = 0; i < frame_ms.size(); ++i) sum += frame_ms[i];
            const double mean = sum / double(frame_ms.size());
            const double p50 = frame_ms[frame_ms.size() / 2];
            const double p99 = frame_ms[(frame_ms.size() * 99) / 100];
            const double worst = frame_ms.back();
            std::fprintf(f, "flight over the trivial world, %.1f s, vsync %s\n",
                         flight_seconds, flight_vsync ? "ON (60 Hz cap)" : "off");
            std::fprintf(f, "frames %zu\n", frame_ms.size());
            std::fprintf(f, "mean %.2f ms (%.0f fps)\n", mean, 1000.0 / mean);
            std::fprintf(f, "median %.2f ms, 99th %.2f ms, worst %.2f ms\n", p50, p99, worst);
            std::fprintf(f, "chunks meshed over the flight: %llu, %.1f per frame\n",
                         meshed_total, double(meshed_total) / double(frame_ms.size()));
            std::fprintf(f, "mean meshing time per frame: %.3f ms\n",
                         mesh_ms_n ? mesh_ms_sum / mesh_ms_n : 0.0);
            std::fprintf(f, "worst backlog: %zu chunks waiting\n", max_pending);
            std::fprintf(f, "at the end: %zu chunks resident, %u triangles, %zu slots free\n",
                         st.resident, st.indices / 3, st.free_slots);
            std::fprintf(f, "slots overflowed: %u\n", st.overflowed);
            std::fprintf(f, "height patches read back: %llu, latency %u frames\n",
                         st.patches_total, st.readback_latency_frames);
            std::fprintf(f, "height residual over %llu samples: max %.4f m, mean %.4f m\n",
                         st.height_samples_checked, st.height_residual_max,
                         st.height_residual_mean);
            std::fprintf(f, "uncapped presentation: %s\n",
                         g_renderer.tearing_available()
                             ? "yes, tearing allowed"
                             : "NO - these times are capped at the refresh rate");
            std::fclose(f);
        }
        g_renderer.shutdown();
        return 0;
    }

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    double last = 0.0, fps_accum = 0.0, worst_frame_ms = 0.0;
    int    fps_frames = 0;
    float  t = 0.0f;

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
        double dt = elapsed - last;
        last = elapsed;
        if (dt > 0.1) dt = 0.1;   // a breakpoint must not teleport the camera
        t += static_cast<float>(dt);

        RECT cr;
        GetClientRect(hwnd, &cr);
        const float w = static_cast<float>(cr.right - cr.left);
        const float h = static_cast<float>(cr.bottom - cr.top);
        if (w < 1.0f || h < 1.0f) continue;

        if (g_walk) {
            // The capsule moves; the camera rides it at eye height. Streaming
            // follows the camera, so this is also what keeps chunks loading
            // around the character rather than around a detached viewpoint.
            float mx = 0.0f, mz = 0.0f;
            if (g_captured && GetForegroundWindow() == hwnd) {
                if (GetAsyncKeyState('W') & 0x8000) mz += 1.0f;
                if (GetAsyncKeyState('S') & 0x8000) mz -= 1.0f;
                if (GetAsyncKeyState('D') & 0x8000) mx += 1.0f;
                if (GetAsyncKeyState('A') & 0x8000) mx -= 1.0f;
            }
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            const float sp = 6.0f;
            const float vx = (sy * mz + cy * mx) * sp;
            const float vz = (cy * mz - sy * mx) * sp;
            const bool jump = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            g_character.update(g_renderer.height_field(), float(dt), vx, vz, jump);
            g_camera.x = g_character.x;
            g_camera.y = g_character.y + g_character.eye;
            g_camera.z = g_character.z;
        }

        if (g_captured && GetForegroundWindow() == hwnd) {
            // Delta from the client centre, then recentre. Simpler than raw
            // input and good enough for a debug flycam.
            POINT c = {static_cast<LONG>(w) / 2, static_cast<LONG>(h) / 2};
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            const float dx = static_cast<float>(p.x - c.x);
            const float dy = static_cast<float>(p.y - c.y);
            if (dx != 0.0f || dy != 0.0f) {
                g_camera.look(dx * 0.0030f, -dy * 0.0030f);
                POINT back = c;
                ClientToScreen(hwnd, &back);
                SetCursorPos(back.x, back.y);
            }
            if (!g_walk) g_camera.update(static_cast<float>(dt));
        }

        engine::ViewConstants view = {};
        fill_view(g_camera, w, h, t, view);
        g_renderer.render(view);

        if (g_want_shot) {
            g_want_shot = false;
            std::string err;
            g_renderer.capture(view, "build/shot.bmp", err);
        }

        const double frame_ms = dt * 1000.0;
        if (frame_ms > worst_frame_ms) worst_frame_ms = frame_ms;
        fps_accum += dt;
        ++fps_frames;
        if (fps_accum >= 0.5) {
            const engine::FrameStats st = g_renderer.stats();
            char title[300];
            std::snprintf(title, sizeof(title),
                          "biomes2 engine - %s - %.0f fps (worst %.1f ms) - "
                          "%zu chunks, %u tris - %u meshed/frame in %.3f ms - "
                          "%zu pending, %zu slots free%s",
                          g_renderer.mode() == engine::Mode::Mesh    ? "mesh"
                          : g_renderer.mode() == engine::Mode::Overlay ? "overlay"
                                                                      : "raymarch",
                          fps_frames / fps_accum, worst_frame_ms,
                          st.resident, st.indices / 3,
                          st.meshed_this_frame, st.mesh_ms,
                          st.pending, st.free_slots,
                          st.overflowed ? " - SLOT OVERFLOW" : "");
            SetWindowTextA(hwnd, title);
            fps_accum = 0.0;
            fps_frames = 0;
            worst_frame_ms = 0.0;
        }
    }

    g_renderer.shutdown();
    return 0;
}
