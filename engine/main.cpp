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
#include "engine/render/shader_compile.h"
#include "engine/camera.h"
#include "engine/physics/physics_world.h"

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
engine::PhysicsWorld g_physics;
bool  g_walk = false;
// The capsule's feet, read back from Jolt each frame. Kept here so the camera
// and the raymarch's capsule agree with what physics actually did, rather than
// with what was asked for.
float g_feet[3] = {0.0f, 0.0f, 0.0f};
// Where the mesh shapes handed to physics actually are, and whether their
// vertices lie on the isosurface. If a shape is not the terrain, nothing about
// Jolt's behaviour will explain why the character goes through it.
double g_mesh_resid_max = 0.0;
// Which LOD the worst one came from. A surface-nets vertex is placed by
// linear interpolation along an edge, so the density there is only as close
// to zero as the field is linear over one voxel -- at an 8 m voxel beside an
// overhang, where layer 6 moves the surface tens of metres, "far from zero"
// is the expected answer and not a fault. Without the voxel beside it the
// number cannot be read at all.
double g_mesh_resid_voxel = 0.0;
unsigned long long g_mesh_verts_checked = 0;
double g_mesh_y_lo = 1e9, g_mesh_y_hi = -1e9;
size_t g_mesh_max_verts = 0, g_mesh_max_idx = 0;
const float kCapsuleRadius = 0.35f;
const float kCapsuleHalf = 0.80f;
const float kEyeAboveFeet = 1.60f;

// Hand this frame's streamed height patches to physics, and drop the ones
// whose chunk is gone. Called before the character moves, so it never steps on
// terrain that was retired a frame ago.
void sync_physics_terrain() {
    const std::vector<engine::Renderer::PatchEvent>& added = g_renderer.patches_added();
    for (size_t i = 0; i < added.size(); ++i) {
        g_physics.set_chunk(added[i].key, added[i].origin, added[i].voxel,
                            added[i].h.data());
    }
    // The real triangles for chunks the cheap shape cannot hold. These land
    // later than the heights and replace them.
    const std::vector<engine::Renderer::MeshPatchEvent>& meshes =
        g_renderer.mesh_patches_added();
    for (size_t i = 0; i < meshes.size(); ++i) {
        const engine::Renderer::MeshPatchEvent& e = meshes[i];
        const size_t nv = e.verts.size() / 3;
        for (size_t k = 0; k < nv; k += 7) {
            const float vy = e.verts[k * 3 + 1];
            if (vy < g_mesh_y_lo) g_mesh_y_lo = vy;
            if (vy > g_mesh_y_hi) g_mesh_y_hi = vy;
            const float d = g_renderer.world().density(e.verts[k * 3 + 0], vy,
                                                      e.verts[k * 3 + 2]);
            const double a = d < 0.0f ? -double(d) : double(d);
            if (a > g_mesh_resid_max) {
                g_mesh_resid_max = a;
                g_mesh_resid_voxel = engine::lod_voxel(e.key.lod);
            }
            ++g_mesh_verts_checked;
        }
        if (nv > g_mesh_max_verts) g_mesh_max_verts = nv;
        if (e.idx.size() > g_mesh_max_idx) g_mesh_max_idx = e.idx.size();

        g_physics.set_chunk_mesh(meshes[i].key, meshes[i].verts.data(),
                                 unsigned(meshes[i].verts.size() / 3),
                                 meshes[i].idx.data(),
                                 unsigned(meshes[i].idx.size()));
    }
    const std::vector<engine::ChunkKey>& gone = g_renderer.patches_removed();
    for (size_t i = 0; i < gone.size(); ++i) g_physics.remove_chunk(gone[i]);
}
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
                // Wireframe over the same camera: where the chunk boundaries
                // are, and which LOD a piece of ground came from.
                const engine::Mode m = g_renderer.mode();
                g_renderer.set_mode(m == engine::Mode::Mesh ? engine::Mode::Wireframe
                                                            : engine::Mode::Mesh);
                return 0;
            }
            if (wp == VK_TAB) {
                g_walk = !g_walk;
                if (g_walk) {
                    // Drop in where the camera is, so the first frames are a
                    // short fall rather than a long one.
                    g_physics.place_character(g_camera.x, g_camera.y, g_camera.z);
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
    // The raymarch draws the capsule about its centre, so the feet are lifted
    // by the half extent.
    v.cap_pos[0] = g_feet[0];
    v.cap_pos[1] = g_feet[1] + kCapsuleHalf + kCapsuleRadius;
    v.cap_pos[2] = g_feet[2];
    v.cap_r = g_walk ? kCapsuleRadius : 0.0f;
    v.cap_half = kCapsuleHalf;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    // --shot <path> renders one frame at a fixed camera and writes it out
    // without showing the window, with residency filled first so the image is
    // the finished world rather than eight chunks of it.
    std::string shot_path;
    bool start_overlay = false, debug_layer = false;
    double flight_seconds = 0.0;
    double walk_seconds = 0.0;
    double overhang_seconds = 0.0;
    double dig_seconds = 0.0;
    bool mode_dig_save = false, mode_dig_verify = false, mode_dig_stress = false;
    bool flight_vsync = false;
    double shot_altitude = 0.0;
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
        if (cl.find("--overhang") != std::string::npos) overhang_seconds = 14.0;
        if (cl.find("--dig-save") != std::string::npos) mode_dig_save = true;
        else if (cl.find("--dig-verify") != std::string::npos) mode_dig_verify = true;
        else if (cl.find("--dig-stress") != std::string::npos) mode_dig_stress = true;
        else if (cl.find("--dig") != std::string::npos) dig_seconds = 20.0;
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
        // The shot camera stands on the ground, which is the right place to
        // judge the terrain underfoot and the wrong one to judge a 20 km
        // disc. --altitude lifts it so the clipmap's outer rings are in
        // frame: without something like this the only way to see whether the
        // world reads as an archipelago is the top-down contact sheet, which
        // is not what a player will ever look at.
        const size_t al = cl.find("--altitude");
        if (al != std::string::npos) {
            size_t p = cl.find_first_not_of(" 	", al + 10);
            if (p != std::string::npos && cl[p] != '-') shot_altitude = std::atof(cl.c_str() + p);
            if (shot_altitude <= 0.0) shot_altitude = 1200.0;
        }
        start_overlay = cl.find("--wire") != std::string::npos;
        debug_layer = cl.find("--debuglayer") != std::string::npos;
    }

    // --check-dialect answers ADR-001's central question and nothing else:
    // does the real field function compile as HLSL? It needs no device, no
    // window and no world, so it runs before any of them exist.
    if (std::string(cmdline ? cmdline : "").find("--check-dialect") != std::string::npos) {
        engine::ComPtr<ID3DBlob> blob;
        std::string err;
        const bool ok = engine::compile_shader("engine/field/dialect_probe.hlsl",
                                               "cs_probe", "cs_5_1", blob, err);
        if (FILE* f = std::fopen("build/dialect_check.txt", "w")) {
            std::fprintf(f, "compiling worldgen/shaders/field.h as HLSL: %s\n",
                         ok ? "OK" : "FAILED");
            if (ok) {
                std::fprintf(f, "bytecode: %zu bytes\n",
                             size_t(blob->GetBufferSize()));
            } else {
                std::fprintf(f, "\n%s\n", err.c_str());
            }
            std::fclose(f);
        }
        return ok ? 0 : 1;
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
    if (!g_physics.init(error)) {
        MessageBoxA(hwnd, error.c_str(), "Physics init failed", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!g_renderer.init(hwnd, error, debug_layer)) {
        // To a file as well as a box: a headless run exits with a code and no
        // other trace of why, which is a long way to debug blind.
        if (FILE* ef = std::fopen("build/engine_error.txt", "w")) {
            std::fprintf(ef, "renderer init failed:\n%s\n", error.c_str());
            std::fclose(ef);
        }
        MessageBoxA(hwnd, error.c_str(), "Renderer init failed", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (start_overlay) g_renderer.set_mode(engine::Mode::Wireframe);

    if (!shot_path.empty()) {
        g_camera.x = 10.0f; g_camera.y = 46.0f; g_camera.z = -70.0f;
        g_camera.yaw = 0.15f; g_camera.pitch = -0.42f;
        if (shot_altitude > 0.0) {
            g_camera.y = float(shot_altitude);
            g_camera.pitch = -0.30f;
        }
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
        g_physics.shutdown();

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

    if (std::string(cmdline ? cmdline : "").find("--band-report") != std::string::npos) {
        g_renderer.band_report("build/band_report.txt");
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (std::string(cmdline ? cmdline : "").find("--probe-field") != std::string::npos) {
        // A chunk straddling the surface near the origin, where the centre
        // floor guarantees land.
        const float surface = 0.20f * 140.0f;
        const float origin[3] = {-8.0f, surface - 8.0f, -8.0f};
        std::string e;
        g_renderer.probe_field(origin, 0.5f, "build/field_probe.txt", e);
        if (debug_layer) g_renderer.drain_debug_messages("build/d3d12_validation.txt");
        if (!e.empty()) {
            if (FILE* f = std::fopen("build/field_probe.txt", "a")) {
                std::fprintf(f, "error: %s\n", e.c_str());
                std::fclose(f);
            }
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    ShowWindow(hwnd, show);

    // The three holes both runs use. Fixed so the two processes agree
    // without passing anything but the measurement.
    const float kHoleX[3] = {0.0f, 11.0f, 0.0f};
    const float kHoleZ[3] = {0.0f, 0.0f, 11.0f};

    // Drop the character just above the real ground rather than at a literal
    // height. The literal was the trivial field's: against the generated world
    // it put the capsule 180 m in the air over one spot and inside a hill over
    // another, and every measurement downstream was then of a long fall.
    struct Spawn {
        static float above(float x, float z) {
            return g_renderer.world().height(x, z) + 2.0f;
        }
    };

    if (mode_dig_save || mode_dig_verify) {
        g_renderer.set_vsync(true);
        g_walk = true;
        g_physics.place_character(kHoleX[0], Spawn::above(kHoleX[0], kHoleZ[0]),
                                  kHoleZ[0]);
        g_physics.character_position(g_feet);

        bool  mismatch = false;
        std::string err;
        if (mode_dig_verify) {
            if (!g_renderer.load_edits("build/edits.bin", err, mismatch)) {
                MessageBoxA(nullptr, err.c_str(), "Load failed", MB_OK | MB_ICONERROR);
                return 1;
            }
        }

        LARGE_INTEGER sfreq, sstart, sprev;
        QueryPerformanceFrequency(&sfreq);
        QueryPerformanceCounter(&sstart);
        sprev = sstart;

        bool dug = false, saved = false;
        double settled = 0.0;
        const double run_for = 14.0;

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - sstart.QuadPart) / double(sfreq.QuadPart);
            const double dt = double(now.QuadPart - sprev.QuadPart) / double(sfreq.QuadPart);
            sprev = now;
            if (total >= run_for) break;

            sync_physics_terrain();

            if (mode_dig_save && !dug && total >= 4.0) {
                dug = true;
                for (int h = 0; h < 3; ++h) {
                    g_renderer.dig(kHoleX[h], g_feet[1] - 2.0f, kHoleZ[h], 3.5f, -4.0f, 1);
                }
            }
            if (mode_dig_save && dug && !saved && total >= 9.0) {
                saved = true;
                if (!g_renderer.save_edits("build/edits.bin", err)) {
                    MessageBoxA(nullptr, err.c_str(), "Save failed", MB_OK | MB_ICONERROR);
                    return 1;
                }
            }

            g_physics.update(float(dt), 0.0f, 0.0f, false);
            g_physics.character_position(g_feet);
            if (total > 12.0) settled = g_feet[1];

            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h2 = float(cr.bottom - cr.top);
            if (w < 1.0f || h2 < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h2, float(total), v);
            g_renderer.render(v);
        }

        const engine::FrameStats st = g_renderer.stats();
        if (mode_dig_save) {
            if (FILE* f = std::fopen("build/dig_save.txt", "w")) {
                std::fprintf(f, "%.6f\n", settled);
                std::fprintf(f, "three holes dug, saved to build/edits.bin\n");
                std::fprintf(f, "overlay: %zu bricks, %zu edited voxels\n",
                             st.edit_bricks, g_renderer.edits().edited_voxels());
                std::fclose(f);
            }
        } else {
            double expected = 0.0;
            if (FILE* f = std::fopen("build/dig_save.txt", "r")) {
                if (std::fscanf(f, "%lf", &expected) != 1) expected = 0.0;
                std::fclose(f);
            }
            if (FILE* f = std::fopen("build/dig_verify.txt", "w")) {
                std::fprintf(f, "a SEPARATE process: loaded build/edits.bin and measured\n\n");
                std::fprintf(f, "the save run came to rest at %.3f m\n", expected);
                std::fprintf(f, "this run came to rest at      %.3f m\n", settled);
                std::fprintf(f, "difference: %.4f m -- the edits %s\n",
                             settled - expected,
                             (settled - expected < 0.05 && settled - expected > -0.05)
                                 ? "SURVIVED the restart"
                                 : "did NOT survive the restart");
                std::fprintf(f, "\noverlay after loading: %zu bricks, %zu edited voxels\n",
                             st.edit_bricks, g_renderer.edits().edited_voxels());
                std::fprintf(f, "field id matched the save: %s\n", mismatch ? "NO" : "yes");
                std::fclose(f);
            }
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (mode_dig_stress) {
        // More bricks than the pool holds, then fly past them all.
        //
        // Spread in three dimensions at 9 m, which is just over a brick's 8 m,
        // so each hole lands in its own. A line of holes 3 m apart shares
        // bricks between neighbours and produced 364 of them from 240 holes --
        // nowhere near enough to make the pager evict anything.
        g_renderer.set_vsync(false);
        int kHoles = 0;
        for (int iy = 0; iy < 6; ++iy) {
            for (int iz = 0; iz < 13; ++iz) {
                for (int ix = 0; ix < 13; ++ix) {
                    const float x = float(ix) * 9.0f - 54.0f;
                    const float y = float(iy) * 9.0f - 10.0f;
                    const float z = float(iz) * 9.0f - 54.0f;
                    g_renderer.dig(x, y, z, 3.0f, -4.0f, 1);
                    ++kHoles;
                }
            }
        }

        LARGE_INTEGER tf, t0, tp;
        QueryPerformanceFrequency(&tf);
        QueryPerformanceCounter(&t0);
        tp = t0;
        g_camera.x = -140.0f; g_camera.y = 25.0f; g_camera.z = 0.0f;
        g_camera.yaw = 1.5708f; g_camera.pitch = -0.3f;

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - t0.QuadPart) / double(tf.QuadPart);
            const double dt = double(now.QuadPart - tp.QuadPart) / double(tf.QuadPart);
            tp = now;
            if (total >= 16.0) break;
            // Orbit the dug region so chunks all round it are meshed and every
            // brick is asked for at some point.
            const float a = float(total) * 0.6f;
            g_camera.x = std::cos(a) * 95.0f;
            g_camera.z = std::sin(a) * 95.0f;
            g_camera.yaw = a + 3.14159f;

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h2 = float(cr.bottom - cr.top);
            if (w < 1.0f || h2 < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h2, float(total), v);
            g_renderer.render(v);
            (void)dt;
        }

        const engine::FrameStats st = g_renderer.stats();
        if (FILE* f = std::fopen("build/dig_stress.txt", "w")) {
            std::fprintf(f, "%d holes on a 9 m lattice, then orbited\n\n", kHoles);
            std::fprintf(f, "overlay: %zu bricks, %zu edited voxels, %.1f MB\n",
                         st.edit_bricks, g_renderer.edits().edited_voxels(),
                         double(st.edit_bytes) / (1024.0 * 1024.0));
            std::fprintf(f, "GPU pool: %zu of %u slots leased\n",
                         st.bricks_resident, engine::kMaxBricks);
            std::fprintf(f, "evictions: %llu\n", st.brick_evictions);
            std::fprintf(f, "bricks a frame could not page in at all: %llu\n",
                         st.brick_misses);
            std::fclose(f);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (dig_seconds > 0.0) {
        g_renderer.set_vsync(true);
        g_walk = true;
        g_physics.place_character(0.0f, Spawn::above(0.0f, 0.0f), 0.0f);
        g_physics.character_position(g_feet);
        g_camera.yaw = 0.6f;
        g_camera.pitch = -0.35f;

        LARGE_INTEGER dfreq, dstart, dprev;
        QueryPerformanceFrequency(&dfreq);
        QueryPerformanceCounter(&dstart);
        dprev = dstart;

        double ground_before = 0.0, ground_after = 0.0, ground_returned = 0.0;
        unsigned long long frames = 0, dig_frame = 0, fall_frame = 0;
        bool dug = false, sent_away = false, brought_back = false, fell = false;
        bool unsupported = false;
        unsigned long long unsupported_frame = 0;
        float dig_at[3] = {0.0f, 0.0f, 0.0f};

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - dstart.QuadPart) / double(dfreq.QuadPart);
            const double dt = double(now.QuadPart - dprev.QuadPart) / double(dfreq.QuadPart);
            dprev = now;
            if (total >= dig_seconds) break;
            ++frames;

            sync_physics_terrain();

            if (!dug && total >= 4.0) {
                dug = true;
                dig_frame = frames;
                ground_before = g_feet[1];
                dig_at[0] = g_feet[0];
                dig_at[1] = g_feet[1] - 2.0f;
                dig_at[2] = g_feet[2];
                // Absolute density, strongly negative: this is air now,
                // whatever the field says here.
                g_renderer.dig(dig_at[0], dig_at[1], dig_at[2], 3.5f, -4.0f, 1);
            }
            // Two different things, and the first test conflated them. The
            // moment the ground stops supporting the character is when physics
            // has followed the edit. Falling a whole metre from rest under
            // gravity of 22 takes another 18 frames all by itself, which is
            // not latency, it is gravity.
            if (dug && !unsupported && total > 4.0 && !g_physics.character_grounded()) {
                unsupported = true;
                unsupported_frame = frames;
            }
            if (dug && !fell && total > 4.0 && g_feet[1] < ground_before - 1.0) {
                fell = true;
                fall_frame = frames;
            }
            if (dug && !sent_away && total >= 9.0) {
                sent_away = true;
                ground_after = g_feet[1];
                // Far enough that every chunk around the hole retires.
                g_physics.place_character(400.0f, Spawn::above(400.0f, 400.0f),
                                          400.0f);
            }
            if (sent_away && !brought_back && total >= 14.0) {
                brought_back = true;
                g_physics.place_character(dig_at[0], ground_before + 12.0f, dig_at[2]);
            }

            g_physics.update(float(dt), 0.0f, 0.0f, false);
            g_physics.character_position(g_feet);
            if (brought_back) ground_returned = g_feet[1];

            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);
        }

        const engine::FrameStats st = g_renderer.stats();
        if (FILE* f = std::fopen("build/dig.txt", "w")) {
            std::fprintf(f, "dig a hole, %.0f s\n\n", dig_seconds);
            std::fprintf(f, "ground under the character before digging: %.2f m\n",
                         ground_before);
            std::fprintf(f, "dug a sphere of radius 3.5 m centred 2 m below it\n");
            if (unsupported) {
                std::fprintf(f, "physics stopped supporting it %llu frames after the edit\n",
                             unsupported_frame - dig_frame);
            } else {
                std::fprintf(f, "physics NEVER stopped supporting it\n");
            }
            if (fell) {
                std::fprintf(f, "it had fallen a metre %llu frames after the edit\n",
                             fall_frame - dig_frame);
                std::fprintf(f, "  of which %llu are gravity, not latency\n",
                             fall_frame - (unsupported ? unsupported_frame : dig_frame));
            } else {
                std::fprintf(f, "the character NEVER fell -- physics did not follow\n");
            }
            std::fprintf(f, "where it came to rest: %.2f m (%.2f m lower)\n",
                         ground_after, ground_before - ground_after);
            std::fprintf(f, "\nsent 400 m away so every chunk around the hole retired,\n");
            std::fprintf(f, "then brought back and dropped in again:\n");
            std::fprintf(f, "  it came to rest at %.2f m\n", ground_returned);
            std::fprintf(f, "  the hole %s\n",
                         ground_returned < ground_before - 1.0
                             ? "SURVIVED the chunk being rebuilt"
                             : "was LOST when the chunk was rebuilt");
            std::fprintf(f, "\noverlay: %zu bricks, %zu bytes (%.1f KB)\n",
                         st.edit_bricks, st.edit_bytes, double(st.edit_bytes) / 1024.0);
            std::fprintf(f, "chunks thrown away by the edit: %zu\n",
                         st.chunks_invalidated);
            std::fprintf(f, "physics bodies: %zu\n", g_physics.body_count());
            std::fclose(f);
        }

        // Captured in MESH mode, not the raymarch. The raymarch evaluates the
        // analytic field, which has no hole in it -- the edit exists only in
        // the overlay and therefore only in the meshed geometry.
        {
            engine::Camera c;
            // Straight down from well above. An oblique camera near a
            // hole keeps ending up inside the hillside beside it.
            // 45 degrees from 30 m: steep enough to see into the pit, far
            // enough that a 7 m hole is a fifth of the frame.
            c.x = dig_at[0] + 21.0f;
            c.y = dig_at[1] + 21.0f;
            c.z = dig_at[2];
            c.yaw = -1.5708f;
            c.pitch = -0.785f;
            engine::ViewConstants cv = {};
            fill_view(c, 1280.0f, 720.0f, 0.0f, cv);
            g_renderer.set_mode(engine::Mode::Mesh);
            std::string e3;
            g_renderer.capture(cv, "build/b6_dig.bmp", e3);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (overhang_seconds > 0.0) {
        // B5 ran this against the trivial field's floating sphere, whose
        // underside sat at a literal y = 13. The generated world has no such
        // landmark, so the test now goes looking for a real one: a column with
        // rock over air over rock, deep enough to stand in and solid enough
        // overhead to stop a jump. Without this the check would still pass and
        // mean nothing, because there would be nothing above the character at
        // all.
        float ov_x = 0.0f, ov_z = 0.0f;       // under the ceiling
        float ov_ground = 0.0f;               // the floor of the void
        float ov_under = 0.0f;                // the ceiling's underside
        float ov_gap = 0.0f;                  // how much headroom it leaves
        float ov_span = 0.0f;                 // how far it reaches in plan
        float ov_thick = 0.0f;                // how thick the slab is
        bool  ov_found = false;
        bool  ov_fallback = false;
        {
            const engine::WorldGpu& wg = g_renderer.world();
            const float step = 0.25f;
            for (int ring = 1; ring <= 300 && !ov_found; ++ring) {
                const float rad = float(ring) * 6.0f;
                const int spokes = ring * 8;
                for (int sp = 0; sp < spokes && !ov_found; ++sp) {
                    // Rejection-free and good enough for a search: this is a
                    // test fixture, not the generator, so an angle is fine
                    // here where invariant 9 would forbid it upstream.
                    const float a = 6.2831853f * float(sp) / float(spokes);
                    const float x = rad * std::cos(a), z = rad * std::sin(a);
                    const float grade = wg.height(x, z);
                    // From well above the heightfield, because the layer-6
                    // overhang term puts rock up to its own scale ABOVE it,
                    // and that rock is the thing being looked for.
                    const float top = grade + 40.0f;

                    // Downward: the first rock, then the air under it, then
                    // the floor. The first rock must have real thickness, or a
                    // single noisy sample reads as a ceiling.
                    float roof_bottom = 0.0f, roof_top = 0.0f;
                    bool in_roof = false, have_roof = false;
                    for (float y = top; y > grade - 20.0f; y -= step) {
                        const bool rock = wg.density(x, y, z) > 0.0f;
                        if (rock && !in_roof) { in_roof = true; roof_top = y; }
                        if (!rock && in_roof) {
                            roof_bottom = y;
                            if (roof_top - roof_bottom >= 1.5f) have_roof = true;
                            break;
                        }
                    }
                    if (!have_roof) continue;

                    // The void below it, and the floor it stands on.
                    float floor_y = 0.0f;
                    bool have_floor = false;
                    for (float y = roof_bottom; y > roof_bottom - 60.0f; y -= step) {
                        if (wg.density(x, y, z) > 0.0f) {
                            floor_y = y;
                            have_floor = true;
                            break;
                        }
                    }
                    if (!have_floor) continue;
                    const float gap = roof_bottom - floor_y;
                    if (gap < 3.0f) continue;        // no room for the capsule
                    // Near grade, or it is a cave roof rather than an
                    // overhang. The first version of this search did not say
                    // so and found a ceiling 45 m underground, which the
                    // character could only reach by being placed inside it.
                    if (floor_y < grade - 10.0f) continue;
                    if (roof_top < grade) continue;  // the slab is above the surface

                    // How far the roof reaches in plan, at the height it
                    // would have to stop something. A slab that is thick but
                    // only as wide as the capsule does not stop a launch --
                    // the capsule rises beside it -- so the width is measured
                    // and printed rather than assumed, and the first version
                    // of this test reported "passed straight THROUGH" for a
                    // roof 1.5 m thick without saying how wide it was.
                    const float probe = roof_bottom + 0.5f;
                    float span = 0.0f;
                    for (float rr = 0.5f; rr <= 12.0f; rr += 0.5f) {
                        bool all_solid = true;
                        for (int d = 0; d < 4 && all_solid; ++d) {
                            const float ox = (d == 0 ? rr : d == 1 ? -rr : 0.0f);
                            const float oz = (d == 2 ? rr : d == 3 ? -rr : 0.0f);
                            if (wg.density(x + ox, probe, z + oz) <= 0.0f) all_solid = false;
                        }
                        if (!all_solid) break;
                        span = rr;
                    }
                    // A roof the capsule can rise beside is not a test of
                    // anything, so a wide one is preferred -- but a narrow one
                    // is kept as a fallback rather than reporting that the
                    // world has no overhangs at all, which would be the wrong
                    // conclusion from the wrong threshold.
                    if (span < 1.0f) {
                        if (!ov_fallback) {
                            ov_fallback = true;
                            ov_x = x; ov_z = z;
                            ov_ground = floor_y; ov_under = roof_bottom;
                            ov_gap = gap; ov_span = span;
                            ov_thick = roof_top - roof_bottom;
                        }
                        continue;
                    }
                    ov_span = span;
                    ov_thick = roof_top - roof_bottom;

                    ov_x = x; ov_z = z;
                    ov_ground = floor_y;
                    ov_under = roof_bottom;
                    ov_gap = gap;
                    ov_found = true;
                }
            }
        }
        if (!ov_found && ov_fallback) ov_found = true;
        if (!ov_found) {
            if (FILE* f = std::fopen("build/overhang.txt", "w")) {
                std::fprintf(f, "no overhang within 1800 m of the origin, so there\n"
                                "was nothing to test. That is a finding about the\n"
                                "world, not a pass.\n");
                std::fclose(f);
            }
            g_renderer.shutdown();
            g_physics.shutdown();
            return 0;
        }

        g_renderer.set_vsync(true);
        g_walk = true;
        // Approach from 24 m away along -z, on the surface, so the walk in is
        // over open ground.
        g_physics.place_character(ov_x, g_renderer.world().height(ov_x, ov_z - 24.0f) + 2.0f,
                                  ov_z - 24.0f);
        g_physics.character_position(g_feet);
        g_camera.yaw = 0.0f;
        g_camera.pitch = -0.05f;

        LARGE_INTEGER ofreq, ostart, oprev;
        QueryPerformanceFrequency(&ofreq);
        QueryPerformanceCounter(&ostart);
        oprev = ostart;

        double y_max = -1e9, y_under_max = -1e9, y_under_min = 1e9;
        unsigned long long under_frames = 0, climbed_frames = 0;
        bool shot_taken = false;
        // Phase two: once under the sphere, go straight up at it.
        bool launched = false;
        bool placed_under = false;
        double launch_peak = -1e9;

        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - ostart.QuadPart) / double(ofreq.QuadPart);
            const double dt = double(now.QuadPart - oprev.QuadPart) / double(ofreq.QuadPart);
            oprev = now;
            if (total >= overhang_seconds) break;

            sync_physics_terrain();
            // Stand still for the first two seconds: the mesh shape takes four
            // frames to arrive and the world has to stream in at all.
            float speed = total < 2.0 ? 0.0f : 4.0f;

            // Phase two, on a clock rather than on position. The capsule
            // slides laterally on slopes, so "near the centre" drifted it to
            // the rim of the sphere's footprint where the underside is high
            // or absent -- which is not a test of anything. Put it exactly
            // under the middle, let it settle, then fire.
            //
            // 45 m/s against gravity of 22 is about 46 m of rise, three times
            // the 14.6 m to the underside, so a short peak means something
            // stopped it rather than that it ran out of speed.
            if (total >= 9.0 && !placed_under) {
                placed_under = true;
                g_physics.place_character(ov_x, ov_ground + 1.0f, ov_z);
            }
            if (total >= 10.5 && !launched) {
                launched = true;
                g_physics.launch_character(45.0f);
            }
            if (placed_under) speed = 0.0f;

            g_physics.update(float(dt), 0.0f, speed, false);
            g_physics.character_position(g_feet);
            if (launched && g_feet[1] > launch_peak) launch_peak = g_feet[1];

            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);

            if (total > 2.0) {
                if (g_feet[1] > y_max) y_max = g_feet[1];
                // Inside the ceiling's footprint in plan: if the character
                // is here and below its underside, it is beneath the overhang.
                const float dx = g_feet[0] - ov_x, dz = g_feet[2] - ov_z;
                if (dx * dx + dz * dz < 100.0f) {
                    if (g_feet[1] > ov_under) {
                        ++climbed_frames;
                    } else {
                        ++under_frames;
                        if (g_feet[1] > y_under_max) y_under_max = g_feet[1];
                        if (g_feet[1] < y_under_min) y_under_min = g_feet[1];
                    }
                    if (!shot_taken && dx * dx + dz * dz < 9.0f) {
                        shot_taken = true;
                        engine::Camera c = g_camera;
                        // Beside and slightly below, looking up past the
                        // capsule at the sphere it is standing under.
                        // Far enough back that the capsule and the thing
                        // 14 m above it both fit one 60-degree frame: closer
                        // in, the sphere is out of shot above the character.
                        // Back along the path just walked: open ground, so
                        // nothing occludes the capsule, and far enough that
                        // the sphere 14 m above it is still in frame.
                        c.x = g_feet[0];
                        c.y = g_feet[1] + 2.5f;
                        c.z = g_feet[2] - 22.0f;
                        c.yaw = 0.0f;
                        c.pitch = 0.25f;
                        engine::ViewConstants cv = {};
                        fill_view(c, 1280.0f, 720.0f, 0.0f, cv);
                        const engine::Mode was = g_renderer.mode();
                        g_renderer.set_mode(engine::Mode::Mesh);
                        std::string e2;
                        g_renderer.capture(cv, "build/b5_overhang.bmp", e2);
                        g_renderer.set_mode(was);
                    }
                }
            }
        }

        const engine::FrameStats st = g_renderer.stats();
        if (FILE* f = std::fopen("build/overhang.txt", "w")) {
            std::fprintf(f, "walking under the overhang, %.0f s\n", overhang_seconds);
            std::fprintf(f, "the ceiling found: over (%.1f, %.1f), underside at\n",
                         ov_x, ov_z);
            std::fprintf(f, "y = %.2f with a floor at y = %.2f, so %.2f m of headroom\n",
                         ov_under, ov_ground, ov_gap);
            std::fprintf(f, "the slab is %.2f m thick and stays solid %.1f m out in\n"
                            "every direction, against a capsule %.2f m across\n\n",
                         ov_thick, ov_span, kCapsuleRadius * 2.0f);
            std::fprintf(f, "frames inside the ceiling's footprint: %llu\n", under_frames);
            std::fprintf(f, "feet while under it: min %.2f m, max %.2f m\n",
                         under_frames ? y_under_min : 0.0,
                         under_frames ? y_under_max : 0.0);
            std::fprintf(f, "frames spent ABOVE the underside (climbed onto it): %llu\n",
                         climbed_frames);
            std::fprintf(f, "highest the feet ever reached: %.2f m\n", y_max);
            std::fprintf(f, "\nlaunched straight up at the underside: %s\n",
                         launched ? "yes" : "NO - never reached the centre");
            if (launched) {
                std::fprintf(f, "  the capsule's feet peaked at %.2f m\n", launch_peak);
                std::fprintf(f, "  its head therefore reached %.2f m\n",
                             launch_peak + double(kCapsuleHalf + kCapsuleRadius) * 2.0);
                if (ov_span < 1.0f) {
                    std::fprintf(f, "  the roof is only %.1f m wide, which a capsule"
                                    " %.2f m across can\n  rise beside, so what"
                                    " follows is not evidence about the ceiling\n",
                                 ov_span, kCapsuleRadius * 2.0f);
                }
                std::fprintf(f, "  the underside is at %.2f m, so %s\n", ov_under,
                             launch_peak + double(kCapsuleHalf + kCapsuleRadius) * 2.0 <
                                     double(ov_under) + 1.0
                                 ? "it was STOPPED by the overhang"
                                 : "it passed straight THROUGH the overhang");
            }
            std::fprintf(f, "chunks classified: %llu, needing a mesh: %llu (%.1f%%)\n",
                         st.chunks_classified, st.chunks_needing_mesh,
                         st.chunks_classified ? 100.0 * double(st.chunks_needing_mesh) /
                                                     double(st.chunks_classified) : 0.0);
            std::fprintf(f, "mesh shapes delivered: %llu, still waiting: %zu\n",
                         st.mesh_shapes_delivered, st.mesh_backlog);
            std::fprintf(f, "physics bodies: %zu total, %zu of them mesh shapes\n",
                         g_physics.body_count(), g_physics.mesh_body_count());
            std::fprintf(f, "mesh vertices checked against the field: %llu, worst %.4f m at a %.1f m voxel\n",
                         g_mesh_verts_checked, g_mesh_resid_max, g_mesh_resid_voxel);
            std::fprintf(f, "mesh shape vertices span y %.2f .. %.2f m\n",
                         g_mesh_y_lo, g_mesh_y_hi);
            std::fprintf(f, "biggest mesh patch: %zu vertices, %zu indices\n",
                         g_mesh_max_verts, g_mesh_max_idx);
            std::fclose(f);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (walk_seconds > 0.0) {
        g_renderer.set_vsync(true);
        g_walk = true;
        g_physics.place_character(0.0f, Spawn::above(0.0f, 0.0f), 0.0f);
        g_physics.character_position(g_feet);
        g_camera.pitch = -0.12f;

        LARGE_INTEGER wfreq, wstart, wprev;
        QueryPerformanceFrequency(&wfreq);
        QueryPerformanceCounter(&wstart);
        wprev = wstart;

        unsigned long long frames = 0, airborne = 0, waiting = 0, settled_frames = 0;
        double resid_sum = 0.0, resid_max = 0.0;
        double sink_max = 0.0, hover_max = 0.0, excess_sum = 0.0, excess_max = 0.0;
        unsigned long long steep_frames = 0;
        unsigned long long lod_frames[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double lod_resid[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double lod_max[8] = {0, 0, 0, 0, 0, 0, 0, 0};
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
            sync_physics_terrain();
            const float px = g_feet[0], pz = g_feet[2];
            g_physics.update(float(dt), sy * sp, cy * sp, false);
            g_physics.character_position(g_feet);
            travelled += std::sqrt((g_feet[0] - px) * (g_feet[0] - px) +
                                   (g_feet[2] - pz) * (g_feet[2] - pz));

            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];

            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);

            ++frames;
            // Jolt distinguishes standing from merely touching something, and
            // the difference is what a slope too steep to stand on looks like.
            // Jolt separates three states, and they mean different things:
            // nothing under us at all, standing, and touching a slope too
            // steep to stand on. The terrain reaches about 52 degrees and the
            // character's limit is 50, so the third is expected, not a fault.
            if (!g_physics.character_supported()) ++airborne;
            else if (!g_physics.character_grounded()) ++steep_frames;

            // Give the world a second to stream in and the capsule a moment to
            // land before judging it.
            if (total > 1.5 && g_physics.character_grounded()) {
                ++settled_frames;
                const float fy = g_feet[1];
                const float d = g_renderer.world().density(g_feet[0], fy, g_feet[2]);
                // density > 0 is rock, so a positive reading means the feet
                // are INSIDE the ground. That is the direction that would be
                // a bug; the other is the capsule sitting on its own radius.
                if (d > 0.0f && double(d) > sink_max) sink_max = double(d);
                const double hover = d < 0.0f ? -double(d) : 0.0;
                if (hover > hover_max) hover_max = hover;

                // What the capsule's radius alone accounts for on this slope.
                const float e = 0.25f;
                const engine::WorldGpu& wg = g_renderer.world();
                const float gx = (wg.height(g_feet[0] + e, g_feet[2]) -
                                  wg.height(g_feet[0] - e, g_feet[2])) / (2.0f * e);
                const float gz = (wg.height(g_feet[0], g_feet[2] + e) -
                                  wg.height(g_feet[0], g_feet[2] - e)) / (2.0f * e);
                const double grad = std::sqrt(double(gx) * gx + double(gz) * gz);
                const double expected = kCapsuleRadius * (std::sqrt(1.0 + grad * grad) - 1.0);
                const double excess = hover - expected;
                const double aex = excess < 0.0 ? -excess : excess;
                excess_sum += aex;
                if (aex > excess_max) excess_max = aex;

                const double a = d < 0.0f ? -double(d) : double(d);
                resid_sum += a;
                if (a > resid_max) resid_max = a;
                // Which chunk is actually holding it up. Once several LOD
                // levels have bodies overlapping, the position alone cannot
                // say, and the residual is meaningless without it.
                const int gl = g_physics.character_ground_lod();
                if (gl >= 0 && gl < 8) {
                    ++lod_frames[gl];
                    lod_resid[gl] += a;
                    if (a > lod_max[gl]) lod_max[gl] = a;
                }
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
            std::fprintf(f, "capsule walk over the generated world, %.1f s\n", walk_seconds);
            std::fprintf(f, "frames %llu, travelled %.0f m\n", frames, travelled);
            // Jolt cannot tell "in the air" from "that chunk has not arrived":
            // in both cases nothing is underneath. One number, not two labels
            // for the same count.
            std::fprintf(f, "frames with nothing underneath: %llu\n", airborne);
            (void)waiting;
            std::fprintf(f, "settled frames judged: %llu\n", settled_frames);
            std::fprintf(f, "feet vs the field: max %.4f m, mean %.4f m\n",
                         resid_max, settled_frames ? resid_sum / double(settled_frames) : 0.0);
            std::fprintf(f, "  of which sinking INTO the ground: max %.4f m\n", sink_max);
            std::fprintf(f, "  hovering above it: max %.4f m\n", hover_max);
            std::fprintf(f, "  beyond what the capsule radius explains on that slope:"
                            " max %.4f m, mean %.4f m\n",
                         excess_max, settled_frames ? excess_sum / double(settled_frames) : 0.0);
            std::fprintf(f, "frames on a slope too steep to stand on: %llu\n", steep_frames);
            std::fprintf(f, "worst single-frame vertical step: %.4f m\n", worst_step);
            std::fprintf(f, "static bodies in the physics world: %zu\n",
                         g_physics.body_count());
            std::fprintf(f, "chunks classified: %llu, needing a mesh shape: %llu (%.1f%%)\n",
                         st.chunks_classified, st.chunks_needing_mesh,
                         st.chunks_classified
                             ? 100.0 * double(st.chunks_needing_mesh) /
                                   double(st.chunks_classified)
                             : 0.0);
            for (int L = 0; L < 8; ++L) {
                if (!lod_frames[L]) continue;
                std::fprintf(f, "  standing on LOD %d: %llu frames, residual max %.4f mean %.4f\n",
                             L, lod_frames[L], lod_max[L],
                             lod_resid[L] / double(lod_frames[L]));
            }
            std::fprintf(f, "height readback latency: %u frames\n",
                         st.readback_latency_frames);
            std::fclose(f);
        }

        // Where the walk ended up. The capsule itself is no longer in the
        // picture: it was drawn by the raymarch's capsule SDF, and M1 retired
        // the raymarch along with the trivial field it was the last caller of.
        // What the shot still shows is the ground the walk finished on.
        {
            engine::Camera c = g_camera;
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            c.x = g_feet[0] - sy * 7.0f;
            c.z = g_feet[2] - cy * 7.0f;
            // Lifted clear of whatever is at the camera's own position: seven
            // metres behind the character is often inside a hillside, and the
            // first attempt at this shot was a screen of solid rock.
            const float behind = g_renderer.world().height(c.x, c.z);
            c.y = g_feet[1] + 3.2f;
            if (c.y < behind + 2.5f) c.y = behind + 2.5f;
            c.pitch = -0.33f;
            engine::ViewConstants v = {};
            fill_view(c, 1280.0f, 720.0f, 0.0f, v);
            g_renderer.set_mode(engine::Mode::Mesh);
            std::string err;
            g_renderer.capture(v, "build/b4_walk.bmp", err);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
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
            std::fprintf(f, "flight over the generated world, %.1f s, vsync %s\n",
                         flight_seconds, flight_vsync ? "ON (60 Hz cap)" : "off");
            std::fprintf(f, "frames %zu\n", frame_ms.size());
            std::fprintf(f, "mean %.2f ms (%.0f fps)\n", mean, 1000.0 / mean);
            std::fprintf(f, "median %.2f ms, 99th %.2f ms, worst %.2f ms\n", p50, p99, worst);
            std::fprintf(f, "chunks meshed over the flight: %llu, %.1f per frame\n",
                         meshed_total, double(meshed_total) / double(frame_ms.size()));
            std::fprintf(f, "mean meshing time per frame: %.3f ms\n",
                         mesh_ms_n ? mesh_ms_sum / mesh_ms_n : 0.0);
            std::fprintf(f, "worst backlog: %zu chunks waiting\n", max_pending);
            std::fprintf(f, "of those resident, holding no slot because they"
                            " meshed to nothing: %zu\n", st.empty_resident);
            std::fprintf(f, "at the end: %zu chunks resident, %u triangles, %zu slots free\n",
                         st.resident, st.indices / 3, st.free_slots);
            std::fprintf(f, "slots overflowed: %u\n", st.overflowed);
            // An overflow is dropped geometry -- a hole. The peaks say how
            // much a chunk actually wanted, so the slot can be sized from a
            // measurement instead of doubled on a guess.
            std::fprintf(f, "worst chunk wanted %u vertices (cap %u) and %u indices (cap %u)\n",
                         st.peak_slot_vertices, engine::kSlotVertices,
                         st.peak_slot_indices, engine::kSlotIndices);
            std::fprintf(f, "height patches read back: %llu, latency %u frames\n",
                         st.patches_total, st.readback_latency_frames);
            std::fprintf(f, "height residual over %llu samples: max %.4f m at a %.1f m voxel, mean %.4f m\n",
                         st.height_samples_checked, st.height_residual_max,
                         st.height_residual_voxel, st.height_residual_mean);
            std::fprintf(f, "uncapped presentation: %s\n",
                         g_renderer.tearing_available()
                             ? "yes, tearing allowed"
                             : "NO - these times are capped at the refresh rate");
            std::fclose(f);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
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
            sync_physics_terrain();
            g_physics.update(float(dt), vx, vz, jump);
            g_physics.character_position(g_feet);
            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];
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
                          g_renderer.mode() == engine::Mode::Mesh ? "mesh" : "wireframe",
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
        g_physics.shutdown();
    return 0;
}
