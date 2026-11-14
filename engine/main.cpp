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
//   Tab         walk (a capsule under physics) / fly
//   Space       jump; in the sea, swim up -- and out over a ledge
//   Ctrl        in the sea, dive
//   [ ]         sea state, calm to storm
//   M           raymarch / mesh / wireframe overlay
//   F5          reload every shader and remesh
//   F2          write a capture to build/shot.bmp
// ---------------------------------------------------------------------------
#include "engine/render/renderer.h"
#include "engine/render/water_wave.h"
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
        const float mS = engine::lod_chunk_size(e.key.lod);
        const float morigin[3] = {float(e.key.x) * mS, float(e.key.y) * mS,
                                  float(e.key.z) * mS};
        const float mvoxel = engine::lod_voxel(e.key.lod);
        for (size_t k = 0; k < nv; k += 7) {
            const float vy = e.verts[k * 3 + 1];
            if (vy < g_mesh_y_lo) g_mesh_y_lo = vy;
            if (vy > g_mesh_y_hi) g_mesh_y_hi = vy;
            // Same exclusion as the capture check: the lip is not an error.
            const float vpos[3] = {e.verts[k * 3 + 0], vy, e.verts[k * 3 + 2]};
            if (engine::is_lip_vertex(vpos, morigin, mvoxel)) continue;
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

// Hand physics the sea the renderer is about to draw: same level, same sea
// state, same clock. `t` must be the time this frame passes to fill_view, or
// the swimmer bobs on a swell that is somewhere else on screen. --no-water
// turns the physics off with the drawing, because a sea you cannot see but
// can float on is the same disagreement the other way round.
void sync_sea(float t) {
    engine::SeaParams s;
    s.on = g_renderer.water();
    s.level = g_renderer.world().params().sea_level;
    s.state = g_renderer.sea_state();
    s.time = t;
    g_physics.set_sea(s);
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
            if (wp == VK_OEM_4 || wp == VK_OEM_6) {
                // [ and ]: calm to storm without leaving the water. Seeing the
                // surface change under one camera is the only way to judge
                // whether the sea state actually reads as weather.
                const float d = (wp == VK_OEM_6) ? 0.1f : -0.1f;
                g_renderer.set_sea_state(g_renderer.sea_state() + d);
                return 0;
            }
            if (wp == 'K') {
                // Cycle the water debug views.
                float m = g_renderer.water_debug() + 1.0f;
                if (m > 4.5f) m = 0.0f;
                g_renderer.set_water_debug(m);
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

// --sea-report: what the sea state actually produces, measured on the C++ side
// of the SAME function the shader draws. Storms are a planned mechanic, so the
// thing that matters is not that waves exist but that the dial maps onto a
// usable range of weather -- and the formula in water_wave.h is not evidence
// of that, only the numbers it produces are.
//
// The assertion worth making is the one you would never catch by looking: the
// MEAN surface height must stay at sea level whatever the sea state. A Gerstner
// sum has zero mean by construction, so a non-zero mean here means an amplitude
// or a steepness term has acquired a bias -- the sea would silently rise or
// fall with the weather, flooding or draining coastlines, and no screenshot of
// a rough sea would look wrong.
static int run_sea_report(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return 1;

    std::fprintf(f, "The sea surface, measured from engine/render/water_wave.h\n");
    std::fprintf(f, "-- the same text the shader compiles.\n\n");
    std::fprintf(f, "%-6s %10s %10s %10s %10s %9s\n",
                 "state", "crest-m", "trough-m", "height-m", "max-slope", "mean-m");

    const int N = 160;          // sample grid a side
    const float span = 420.0f;  // metres, several of the longest wavelength
    double worst_mean = 0.0;
    bool ok = true;
    double inv_miss[11] = {0}, inv_naive[11] = {0};

    for (int s = 0; s <= 10; s += 2) {
        const float state = float(s) / 10.0f;
        double lo = 1e30, hi = -1e30, sum = 0.0, slope = 0.0;
        int n = 0;
        // Two times, because a single snapshot of a travelling wave can miss
        // the crest entirely and report a calm sea.
        for (int ti = 0; ti < 2; ++ti) {
            const float t = ti == 0 ? 0.0f : 7.3f;
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < N; ++i) {
                    const float x = -span * 0.5f + span * float(i) / float(N - 1);
                    const float z = -span * 0.5f + span * float(j) / float(N - 1);
                    ww_float3 nrm;
                    const ww_float3 p = ww_surface(x, z, t, state, 1.0f, 0.0f, nrm);
                    if (p.y < lo) lo = p.y;
                    if (p.y > hi) hi = p.y;
                    sum += p.y;
                    ++n;
                    // ny is cos of the angle from vertical; smaller means steeper.
                    const double sl = nrm.y > 1e-6f ? (1.0 / nrm.y) : 1e6;
                    if (sl > slope) slope = sl;

                    // The waterline physics asks for: the surface ABOVE
                    // (x, z), not the water that started there. Every
                    // fourth sample is plenty to find the worst case.
                    if ((i & 3) == 0 && (j & 3) == 0) {
                        const ww_float2 r = ww_rest_point(x, z, t, state, 0.0f);
                        ww_float3 n2;
                        const ww_float3 q = ww_surface(r.x, r.y, t, state, 1.0f, 0.0f, n2);
                        const double mx = double(q.x) - x, mz = double(q.z) - z;
                        const double miss = std::sqrt(mx * mx + mz * mz);
                        if (miss > inv_miss[s]) inv_miss[s] = miss;
                        const double naive = std::fabs(double(p.y) - double(q.y));
                        if (naive > inv_naive[s]) inv_naive[s] = naive;
                    }
                }
            }
        }
        const double mean = n ? sum / n : 0.0;
        const double amean = mean < 0.0 ? -mean : mean;
        if (amean > worst_mean) worst_mean = amean;
        if (amean > 0.05) ok = false;

        std::fprintf(f, "%-6.1f %10.2f %10.2f %10.2f %10.2f %9.4f\n",
                     state, hi, lo, hi - lo, slope, mean);
    }

    std::fprintf(f, "\nmean surface height must stay at sea level: worst |mean| %.4f m -> %s\n",
                 worst_mean, ok ? "PASS" : "FAIL (the sea rises with the weather)");

    // The swimmer's waterline. ww_height_at inverts the Gerstner throw so it
    // returns the surface directly above a point; "naive" is what you would
    // get by skipping that and reading the water that STARTED there.
    std::fprintf(f, "\nwaterline inversion (%d Newton steps), what physics floats on:\n",
                 WW_INVERT_STEPS);
    std::fprintf(f, "%-6s %16s %22s\n", "state", "miss-m (horiz)", "naive-error-m (vert)");
    bool inv_ok = true;
    for (int s = 0; s <= 10; s += 2) {
        std::fprintf(f, "%-6.1f %16.6f %22.3f\n", float(s) / 10.0f, inv_miss[s], inv_naive[s]);
        if (inv_miss[s] > 0.001) inv_ok = false;
    }
    std::fprintf(f, "inversion lands within 1 mm of the asked point: %s\n",
                 inv_ok ? "PASS" : "FAIL (raise WW_INVERT_STEPS)");
    if (!inv_ok) ok = false;
    std::fprintf(f, "\nmax-slope is 1/n.y, so 1.00 is flat and the Gerstner limit is\n");
    std::fprintf(f, "where crests come to a point. Steepness rises with the sea\n");
    std::fprintf(f, "state on purpose -- sharpening crests is most of what makes a\n");
    std::fprintf(f, "storm read as one rather than as calm water scaled up.\n");
    std::fclose(f);
    return ok ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    // --shot <path> renders one frame at a fixed camera and writes it out
    // without showing the window, with residency filled first so the image is
    // the finished world rather than eight chunks of it.
    std::string shot_path;
    bool start_overlay = false, debug_layer = false;
    float sea_state = 0.25f;
    float water_debug = 0.0f;
    bool  no_water = false;
    double flight_seconds = 0.0;
    double walk_seconds = 0.0;
    double swim_seconds = 0.0;
    double overhang_seconds = 0.0;
    double dig_seconds = 0.0;
    bool mode_dig_save = false, mode_dig_verify = false, mode_dig_stress = false;
    bool flight_vsync = false;
    double shot_altitude = 0.0;
    bool   sea_shot = false;
    bool   have_cam = false;
    bool   no_lod_hold = false;
    int    lod_fade = -1;
    float  cam_arg[5] = {0, 0, 0, 0, 0};
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
        const size_t sw = cl.find("--swim");
        if (sw != std::string::npos) {
            swim_seconds = 40.0;
            size_t p = cl.find_first_not_of(" \t", sw + 6);
            if (p != std::string::npos && cl[p] != '-') swim_seconds = std::atof(cl.c_str() + p);
            if (swim_seconds <= 0.0) swim_seconds = 40.0;
        }
        const size_t fl = cl.find("--flight");
        if (fl != std::string::npos) {
            flight_seconds = 8.0;
            size_t p = cl.find_first_not_of(" \t", fl + 8);
            if (p != std::string::npos && cl[p] != '-') flight_seconds = std::atof(cl.c_str() + p);
            if (flight_seconds <= 0.0) flight_seconds = 8.0;
        }
        flight_vsync = cl.find("--vsync") != std::string::npos;

        // Sea state, 0 calm to 1 storm. Storms are a planned mechanic, so this
        // has to be drivable from outside the shader -- here, and on [ and ]
        // while the window is up.
        const size_t ss = cl.find("--sea-state");
        if (ss != std::string::npos) {
            size_t p = cl.find_first_not_of(" \t", ss + 11);
            if (p != std::string::npos) sea_state = float(std::atof(cl.c_str() + p));
        }
        no_water = cl.find("--no-water") != std::string::npos;
        const size_t wd = cl.find("--water-debug");
        if (wd != std::string::npos) {
            size_t p = cl.find_first_not_of(" \t", wd + 13);
            if (p != std::string::npos && cl[p] != '-') water_debug = float(std::atof(cl.c_str() + p));
            else water_debug = 1.0f;
        }
        // The shot camera stands on the ground, which is the right place to
        // judge the terrain underfoot and the wrong one to judge a 20 km
        // disc. --altitude lifts it so the clipmap's outer rings are in
        // frame: without something like this the only way to see whether the
        // world reads as an archipelago is the top-down contact sheet, which
        // is not what a player will ever look at.
        sea_shot = cl.find("--sea-shot") != std::string::npos;
        const size_t al = cl.find("--altitude");
        if (al != std::string::npos) {
            size_t p = cl.find_first_not_of(" 	", al + 10);
            if (p != std::string::npos && cl[p] != '-') shot_altitude = std::atof(cl.c_str() + p);
            if (shot_altitude <= 0.0) shot_altitude = 1200.0;
        }
        // --cam x y z yaw pitch: an exact vantage for --shot, so a picture
        // someone points at can be rebuilt and A/B'd rather than described.
        const size_t ca = cl.find("--cam");
        if (ca != std::string::npos) {
            float c[5] = {0, 0, 0, 0, 0};
            if (std::sscanf(cl.c_str() + ca + 5, "%f %f %f %f %f",
                            &c[0], &c[1], &c[2], &c[3], &c[4]) == 5) {
                have_cam = true;
                for (int k = 0; k < 5; ++k) cam_arg[k] = c[k];
            }
        }
        no_lod_hold = cl.find("--no-lod-hold") != std::string::npos;
        const size_t lf = cl.find("--lod-fade");
        if (lf != std::string::npos) lod_fade = std::atoi(cl.c_str() + lf + 10);
        if (cl.find("--no-overhang") != std::string::npos) {
            engine::WorldGpu::diag_no_overhang = true;
        }
        start_overlay = cl.find("--wire") != std::string::npos;
        debug_layer = cl.find("--debuglayer") != std::string::npos;
    }

    // --check-dialect answers ADR-001's central question and nothing else:
    // does the real field function compile as HLSL? It needs no device, no
    // window and no world, so it runs before any of them exist.
    if (std::string(cmdline ? cmdline : "").find("--sea-report") != std::string::npos) {
        return run_sea_report("build/sea_report.txt");
    }

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

    // Every batch mode, not just --shot. The narrow version of this check
    // covered --shot only and --dig-save hung on a message box the very next
    // time a shader failed to compile -- ten minutes of nothing, with the real
    // error already written to build/engine_error.txt. A mode that was asked
    // to run without a human is headless whatever it is called.
    const bool headless = !shot_path.empty() || flight_seconds > 0.0 ||
                          walk_seconds > 0.0 || swim_seconds > 0.0 ||
                          overhang_seconds > 0.0 ||
                          dig_seconds > 0.0 || mode_dig_save || mode_dig_verify ||
                          mode_dig_stress || sea_shot;

    std::string error;
    if (!g_physics.init(error)) {
        if (FILE* ef = std::fopen("build/engine_error.txt", "w")) {
            std::fprintf(ef, "physics init failed:\n%s\n", error.c_str());
            std::fclose(ef);
        }
        if (!headless) {
            MessageBoxA(hwnd, error.c_str(), "Physics init failed", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
    if (!g_renderer.init(hwnd, error, debug_layer)) {
        // To a file as well as a box: a headless run exits with a code and no
        // other trace of why, which is a long way to debug blind.
        if (FILE* ef = std::fopen("build/engine_error.txt", "w")) {
            std::fprintf(ef, "renderer init failed:\n%s\n", error.c_str());
            std::fclose(ef);
        }
        // Never a modal box in a headless run. A box nobody can see is not an
        // error report, it is a hang.
        if (!headless) {
            MessageBoxA(hwnd, error.c_str(), "Renderer init failed", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
    if (start_overlay) g_renderer.set_mode(engine::Mode::Wireframe);
    g_renderer.set_sea_state(sea_state);
    g_renderer.set_water_debug(water_debug);
    if (no_water) g_renderer.set_water(false);
    if (no_lod_hold) g_renderer.set_lod_hold(false);
    if (lod_fade >= 0) g_renderer.set_lod_fade_frames(lod_fade);
    g_physics.set_column_height([](float x, float z) {
        return g_renderer.world().height(x, z);
    });

    if (sea_shot) {
        // Waves are metres tall and the world is twenty kilometres wide,
        // so no camera that can judge the ARCHIPELAGO can also judge the
        // SURFACE. This finds the nearest coast to the origin, stands just
        // off it at eye height, and looks out to sea -- which is the only
        // vantage from which a 7 m storm swell is a swell rather than a
        // texture.
        const engine::WorldGpu& wg = g_renderer.world();
        const float sea = wg.params().sea_level;
        float best_x = 0.0f, best_z = 0.0f, best_d = 1e9f;
        bool found = false;
        for (int ring = 1; ring <= 400 && !found; ++ring) {
            const float rad = float(ring) * 20.0f;
            const int spokes = ring * 6;
            for (int sp = 0; sp < spokes; ++sp) {
                const float a = 6.2831853f * float(sp) / float(spokes);
                const float x = rad * std::cos(a), z = rad * std::sin(a);
                const float h = wg.height(x, z);
                // Four to twelve metres of water: deep enough to read as
                // sea, shallow enough that the shore is in the frame.
                const float depth = sea - h;
                if (depth < 4.0f || depth > 12.0f) continue;
                const float d = rad;
                if (d < best_d) { best_d = d; best_x = x; best_z = z; found = true; }
            }
            if (found) break;
        }
        if (!found) {
            const std::string where = shot_path.empty() ? std::string("build/sea_shot.txt")
                                                        : shot_path + ".txt";
            if (FILE* e = std::fopen(where.c_str(), "w")) {
                std::fprintf(e, "no water 4-12 m deep within 8 km of the origin,\n"
                                "so there was nothing to stand beside.\n");
                std::fclose(e);
            }
            g_renderer.shutdown();
            g_physics.shutdown();
            return 0;
        }
        // Face away from the land: out along the radius, which at a coast
        // found by walking outwards is seaward by construction.
        g_camera.x = best_x;
        g_camera.z = best_z;
        // Above the biggest sea this dial produces (7.5 m crest to
        // trough), or a storm shot is taken from underwater.
        g_camera.y = sea + 5.0f;
        g_camera.yaw = std::atan2(best_x, best_z);
        g_camera.pitch = -0.06f;
    }

    if (!shot_path.empty()) {
        // --sea-shot has already placed the camera on a coast; do not stamp
        // the fixed vantage over it.
        if (!sea_shot) {
            g_camera.x = 10.0f; g_camera.y = 46.0f; g_camera.z = -70.0f;
            g_camera.yaw = 0.15f; g_camera.pitch = -0.42f;
            if (shot_altitude > 0.0) {
                g_camera.y = float(shot_altitude);
                g_camera.pitch = -0.30f;
            }
        }
        if (have_cam) {
            g_camera.x = cam_arg[0]; g_camera.y = cam_arg[1]; g_camera.z = cam_arg[2];
            g_camera.yaw = cam_arg[3]; g_camera.pitch = cam_arg[4];
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

        // --pick px py [px py ...]: what is AT a pixel. Marches the CPU
        // field along that pixel's ray and reports the hit, how far the 3D
        // terms moved the surface off the 2.5D heightfield, and how sharp the
        // field is there. The dark patches went two rounds of confident wrong
        // explanations for want of exactly this: a world position.
        const std::string cls = cmdline ? cmdline : "";
        const size_t pk = cls.find("--pick");
        if (pk != std::string::npos) {
            const std::string pp = shot_path + ".pick.txt";
            FILE* pf = std::fopen(pp.c_str(), "w");
            const engine::WorldGpu& wg = g_renderer.world();
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            const char* s = cls.c_str() + pk + 6;
            float px = 0.0f, py = 0.0f;
            int used = 0;
            while (pf && std::sscanf(s, "%f %f%n", &px, &py, &used) == 2) {
                s += used;
                const float nx = ((px + 0.5f) / 1280.0f) * 2.0f - 1.0f;
                const float ny = 1.0f - ((py + 0.5f) / 720.0f) * 2.0f;
                float d[3];
                for (int k = 0; k < 3; ++k) {
                    d[k] = fwd[k] + right[k] * nx * v.tan_half_fov * v.aspect +
                           up[k] * ny * v.tan_half_fov;
                }
                const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int k = 0; k < 3; ++k) d[k] /= dl;
                float t0 = 0.0f, t1 = -1.0f;
                for (float t = 0.0f; t < 12000.0f; t += (t < 200.0f ? 0.1f : 2.0f)) {
                    if (wg.density(g_camera.x + d[0] * t, g_camera.y + d[1] * t,
                                   g_camera.z + d[2] * t) > 0.0f) { t1 = t; break; }
                    t0 = t;
                }
                std::fprintf(pf, "pixel (%.0f, %.0f): ", px, py);
                if (t1 < 0.0f) { std::fprintf(pf, "no hit within 2 km\n"); continue; }
                for (int it = 0; it < 30; ++it) {
                    const float tm = 0.5f * (t0 + t1);
                    if (wg.density(g_camera.x + d[0] * tm, g_camera.y + d[1] * tm,
                                   g_camera.z + d[2] * tm) > 0.0f) t1 = tm; else t0 = tm;
                }
                const float hx = g_camera.x + d[0] * t1, hy = g_camera.y + d[1] * t1,
                            hz = g_camera.z + d[2] * t1;
                const float e = 0.05f;
                float g[3] = {
                    wg.density(hx + e, hy, hz) - wg.density(hx - e, hy, hz),
                    wg.density(hx, hy + e, hz) - wg.density(hx, hy - e, hz),
                    wg.density(hx, hy, hz + e) - wg.density(hx, hy, hz - e)};
                const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                // Outward normal points toward air, which is -grad(density).
                const float n[3] = {-g[0] / gl, -g[1] / gl, -g[2] / gl};
                const float h = wg.height(hx, hz);
                // The 3D terms' contribution here: density minus the
                // heightfield's own (h - y). Near the surface that is the
                // overhang displacement; caves are masked off near grade.
                const float term = wg.density(hx, hy, hz) - (h - hy);
                // Crease sharpness: the largest second difference of that term
                // over a 0.5 m step, in metres. A smooth field gives a few
                // centimetres; a knife edge gives metres.
                float crease = 0.0f;
                const float st = 0.5f;
                const float c0 = term;
                for (int ax = 0; ax < 3; ++ax) {
                    float o[3] = {0, 0, 0};
                    o[ax] = st;
                    const float tp = wg.density(hx + o[0], hy + o[1], hz + o[2]) -
                                     (wg.height(hx + o[0], hz + o[2]) - (hy + o[1]));
                    const float tm2 = wg.density(hx - o[0], hy - o[1], hz - o[2]) -
                                      (wg.height(hx - o[0], hz - o[2]) - (hy - o[1]));
                    const float c = std::fabs(tp - 2.0f * c0 + tm2);
                    if (c > crease) crease = c;
                }
                // Which LOD ring owns the hit: the finest level whose 7x7
                // columns around the camera's chunk reach it.
                int lod = -1;
                for (int L = 0; L < engine::kLodCount; ++L) {
                    const float S = engine::lod_chunk_size(L);
                    const int cx = int(std::floor(g_camera.x / S)), cz = int(std::floor(g_camera.z / S));
                    const int hxk = int(std::floor(hx / S)), hzk = int(std::floor(hz / S));
                    if (std::abs(hxk - cx) <= engine::kRingHalf &&
                        std::abs(hzk - cz) <= engine::kRingHalf) { lod = L; break; }
                }
                const float S = lod >= 0 ? engine::lod_chunk_size(lod) : 0.0f;
                float edge = 0.0f;
                if (S > 0.0f) {
                    const float fx = hx / S - std::floor(hx / S), fz = hz / S - std::floor(hz / S);
                    const float ex = (fx < 1.0f - fx ? fx : 1.0f - fx) * S;
                    const float ez = (fz < 1.0f - fz ? fz : 1.0f - fz) * S;
                    edge = ex < ez ? ex : ez;
                }
                std::fprintf(pf, "hit (%.2f, %.2f, %.2f) at %.1f m\n", hx, hy, hz, t1);
                std::fprintf(pf, "  normal (%.2f, %.2f, %.2f) -- %s\n", n[0], n[1], n[2],
                             n[1] < 0.0f ? "FACES DOWN" : (n[1] < 0.3f ? "near vertical" : "faces up"));
                std::fprintf(pf, "  heightfield y %.2f, so the 3D terms moved the surface %+.2f m\n",
                             h, hy - h);
                std::fprintf(pf, "  3D term at the hit %+.2f m; crease (2nd diff over 0.5 m) %.3f m\n",
                             term, crease);
                std::fprintf(pf, "  ring LOD %d (voxel %.1f m), %.2f m from a chunk edge\n",
                             lod, lod >= 0 ? engine::lod_voxel(lod) : 0.0f, edge);

                // The region-cell blend either side of the hit, half a metre
                // each way along the surface normal's horizontal part. The
                // field blends biome properties over the 4 NEAREST cells only;
                // if a 5th would still have had weight, the blend jumps where
                // the 4th and 5th trade places -- a straight bisector.
                const worldgen::SeedWorld& sw = wg.world();
                const worldgen::WorldParams& wp = wg.params();
                const float hl = std::sqrt(n[0] * n[0] + n[2] * n[2]);
                {
                    // A STEP is a change that does not shrink with the
                    // distance: compare 2 mm across against 2 cm across.
                    const float ux = hl > 1e-4f ? n[0] / hl : 1.0f;
                    const float uz = hl > 1e-4f ? n[2] / hl : 0.0f;
                    float jh[2], jt[2];
                    const float arms[2] = {0.001f, 0.01f};
                    for (int a = 0; a < 2; ++a) {
                        const float r = arms[a];
                        const float ax = hx - ux * r, az = hz - uz * r;
                        const float bx = hx + ux * r, bz = hz + uz * r;
                        jh[a] = wg.height(bx, bz) - wg.height(ax, az);
                        jt[a] = (wg.density(bx, hy, bz) - (wg.height(bx, bz) - hy)) -
                                (wg.density(ax, hy, az) - (wg.height(ax, az) - hy));
                    }
                    std::fprintf(pf, "  across the line: heightfield %+.4f m over 2 mm, %+.4f m over"
                                     " 2 cm; 3D term %+.4f over 2 mm, %+.4f over 2 cm\n",
                                 jh[0], jh[1], jt[0], jt[1]);
                }
                for (int side = -1; side <= 1; side += 2) {
                    const float qx = hx + (hl > 1e-4f ? n[0] / hl : 1.0f) * 0.5f * float(side);
                    const float qz = hz + (hl > 1e-4f ? n[2] / hl : 0.0f) * 0.5f * float(side);
                    const float wx = qx + wp.warp_amp * ::fbm(qx * wp.warp_freq,
                                         qz * wp.warp_freq, 2, 2.0f, 0.5f, sw.s_wx);
                    const float wz = qz + wp.warp_amp * ::fbm(qx * wp.warp_freq,
                                         qz * wp.warp_freq, 2, 2.0f, 0.5f, sw.s_wz);
                    std::vector<std::pair<float, int> > all;
                    for (size_t c = 0; c < sw.pts.size(); ++c) {
                        const float ddx = sw.pts[c].first - wx, ddz = sw.pts[c].second - wz;
                        all.push_back(std::make_pair(std::sqrt(ddx * ddx + ddz * ddz), int(c)));
                    }
                    std::partial_sort(all.begin(), all.begin() + 6, all.end());
                    std::fprintf(pf, "  %s side: heightfield %.2f m, nearest cells (dist-d0 m, "
                                     "biome, overhang, weight):",
                                 side < 0 ? "-" : "+", wg.height(qx, qz));
                    for (int k = 0; k < 6; ++k) {
                        const int b = sw.biome[all[k].second];
                        const float t = 1.0f - (all[k].first - all[0].first) / wp.blend_width;
                        const float wt = t > 0.0f ? t * t : 0.0f;
                        std::fprintf(pf, " %s[%.0f b%d %.2f w%.3f]", k == 4 ? "| " : "",
                                     all[k].first - all[0].first, b,
                                     b >= 0 ? wg.table()[b].overhang_strength : -1.0f, wt);
                    }
                    std::fprintf(pf, "\n");
                }
            }
            if (pf) std::fclose(pf);
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

    if (std::string(cmdline ? cmdline : "").find("--normal-check") != std::string::npos) {
        g_renderer.normal_check("build/normal_check.txt");
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (std::string(cmdline ? cmdline : "").find("--blend-report") != std::string::npos) {
        // --blend-report: is the region-cell blend continuous? The field
        // blends biome properties over the 4 NEAREST cells, with a weight
        // that is non-zero out to blend_width. Where a 5th cell would still
        // have weight, the 4th and 5th trading places is a jump, on a
        // straight bisector. This walks random lines across the disc, finds
        // every place the nearest-4 set changes, and measures the step in the
        // heightfield and in the 3D term there -- a step being a change that
        // does not shrink with the distance it is measured over.
        const engine::WorldGpu& wg = g_renderer.world();
        const worldgen::SeedWorld& sw = wg.world();
        const worldgen::WorldParams& wp = wg.params();
        const float sea = wp.sea_level;

        struct Near { int id[5]; float d[5]; };
        auto nearest5 = [&](float x, float z, Near& o) {
            const float wx = x + wp.warp_amp * ::fbm(x * wp.warp_freq, z * wp.warp_freq,
                                                     2, 2.0f, 0.5f, sw.s_wx);
            const float wz = z + wp.warp_amp * ::fbm(x * wp.warp_freq, z * wp.warp_freq,
                                                     2, 2.0f, 0.5f, sw.s_wz);
            for (int k = 0; k < 5; ++k) { o.id[k] = -1; o.d[k] = 3e38f; }
            for (size_t c = 0; c < sw.pts.size(); ++c) {
                const float dx = sw.pts[c].first - wx, dz = sw.pts[c].second - wz;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d >= o.d[4]) continue;
                int k = 4;
                while (k > 0 && o.d[k - 1] > d) { o.d[k] = o.d[k - 1]; o.id[k] = o.id[k - 1]; --k; }
                o.d[k] = d; o.id[k] = int(c);
            }
        };
        auto set_key = [](const Near& n) {
            int s[4] = {n.id[0], n.id[1], n.id[2], n.id[3]};
            std::sort(s, s + 4);
            return (unsigned long long)(s[0] & 0xFFFF) | ((unsigned long long)(s[1] & 0xFFFF) << 16) |
                   ((unsigned long long)(s[2] & 0xFFFF) << 32) | ((unsigned long long)(s[3] & 0xFFFF) << 48);
        };
        auto term_at = [&](float x, float z) {
            const float h = wg.height(x, z);
            return wg.density(x, h, z);   // (h - y) is 0 at y = h
        };

        unsigned long long rng = 0x9E3779B97F4A7C15ull;
        auto rnd = [&]() {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            return float((rng >> 40) & 0xFFFFFF) / float(0x1000000);
        };

        int swaps = 0, weighted = 0, weighted_land = 0;
        std::vector<double> hsteps, tsteps;
        double hmax = 0.0, tmax = 0.0;
        float hmax_at[2] = {0, 0}, tmax_at[2] = {0, 0};
        const int kLines = 300;
        const float kLen = 3000.0f, kStep = 2.0f;
        for (int L = 0; L < kLines; ++L) {
            float sx, sz;
            do { sx = (rnd() * 2.0f - 1.0f) * wp.radius; sz = (rnd() * 2.0f - 1.0f) * wp.radius; }
            while (sx * sx + sz * sz > 0.8f * wp.radius * wp.radius);
            float dx, dz, dl;
            do { dx = rnd() * 2.0f - 1.0f; dz = rnd() * 2.0f - 1.0f; dl = std::sqrt(dx * dx + dz * dz); }
            while (dl > 1.0f || dl < 0.1f);
            dx /= dl; dz /= dl;
            Near prev; nearest5(sx, sz, prev);
            unsigned long long pk = set_key(prev);
            for (float s = kStep; s <= kLen; s += kStep) {
                Near cur; nearest5(sx + dx * s, sz + dz * s, cur);
                const unsigned long long ck = set_key(cur);
                if (ck != pk) {
                    ++swaps;
                    // Bisect to the crossing.
                    float a = s - kStep, b = s;
                    for (int it = 0; it < 30; ++it) {
                        const float m = 0.5f * (a + b);
                        Near q; nearest5(sx + dx * m, sz + dz * m, q);
                        if (set_key(q) == pk) a = m; else b = m;
                    }
                    const float cx = sx + dx * 0.5f * (a + b), cz = sz + dz * 0.5f * (a + b);
                    Near q; nearest5(cx, cz, q);
                    // Weight the 4th/5th carry at the crossing, where they tie.
                    const float t = 1.0f - (q.d[3] - q.d[0]) / wp.blend_width;
                    const bool both_land = sw.biome[q.id[3]] >= 0 || sw.biome[q.id[4]] >= 0;
                    if (t > 0.0f && both_land) {
                        ++weighted;
                        const float h = wg.height(cx, cz);
                        if (h > sea) ++weighted_land;
                        const float r1 = 0.001f, r2 = 0.01f;
                        const float h1 = wg.height(cx + dx * r1, cz + dz * r1) - wg.height(cx - dx * r1, cz - dz * r1);
                        const float h2 = wg.height(cx + dx * r2, cz + dz * r2) - wg.height(cx - dx * r2, cz - dz * r2);
                        const float t1 = term_at(cx + dx * r1, cz + dz * r1) - term_at(cx - dx * r1, cz - dz * r1);
                        const float t2 = term_at(cx + dx * r2, cz + dz * r2) - term_at(cx - dx * r2, cz - dz * r2);
                        // The part that does not scale with the arm is the step.
                        const double hs = std::fabs(double(h1) - double(h2) * 0.1) / 0.9;
                        const double ts = std::fabs(double(t1) - double(t2) * 0.1) / 0.9;
                        hsteps.push_back(hs); tsteps.push_back(ts);
                        if (hs > hmax) { hmax = hs; hmax_at[0] = cx; hmax_at[1] = cz; }
                        if (ts > tmax) { tmax = ts; tmax_at[0] = cx; tmax_at[1] = cz; }
                    }
                    pk = ck;
                }
            }
        }
        std::sort(hsteps.begin(), hsteps.end());
        std::sort(tsteps.begin(), tsteps.end());
        auto pct = [](const std::vector<double>& v, double p) {
            return v.empty() ? 0.0 : v[size_t(p * double(v.size() - 1))];
        };
        auto over = [](const std::vector<double>& v, double th) {
            int n = 0; for (size_t i = 0; i < v.size(); ++i) if (v[i] > th) ++n; return n;
        };
        if (FILE* f = std::fopen("build/blend_report.txt", "w")) {
            std::fprintf(f, "region-cell blend continuity, %d random lines of %.0f m, %zu cells, "
                            "blend_width %.0f m\n", kLines, kLen, sw.pts.size(), wp.blend_width);
            std::fprintf(f, "places the nearest-4 set changes: %d\n", swaps);
            std::fprintf(f, "  where the cell leaving or joining still had weight: %d (%d on land)\n",
                         weighted, weighted_land);
            std::fprintf(f, "\nheightfield step at those places (m):\n");
            std::fprintf(f, "  median %.4f, 90th %.4f, 99th %.4f, max %.4f at (%.0f, %.0f)\n",
                         pct(hsteps, 0.5), pct(hsteps, 0.9), pct(hsteps, 0.99), hmax,
                         hmax_at[0], hmax_at[1]);
            std::fprintf(f, "  over 0.05 m: %d, over 0.25 m (half a LOD 0 voxel): %d, over 1 m: %d\n",
                         over(hsteps, 0.05), over(hsteps, 0.25), over(hsteps, 1.0));
            std::fprintf(f, "\n3D term (overhang) step at those places (m):\n");
            std::fprintf(f, "  median %.4f, 90th %.4f, 99th %.4f, max %.4f at (%.0f, %.0f)\n",
                         pct(tsteps, 0.5), pct(tsteps, 0.9), pct(tsteps, 0.99), tmax,
                         tmax_at[0], tmax_at[1]);
            std::fprintf(f, "  over 0.05 m: %d, over 0.25 m: %d, over 1 m: %d\n",
                         over(tsteps, 0.05), over(tsteps, 0.25), over(tsteps, 1.0));
            // What the worst places look like: their six nearest cells, and
            // how many of the weighted ones are land. A step that survives
            // a continuous weight must come from the normalisation instead.
            const float* worst[2] = {hmax_at, tmax_at};
            const char*  name[2] = {"height", "3D term"};
            for (int wv = 0; wv < 2; ++wv) {
                Near q; nearest5(worst[wv][0], worst[wv][1], q);
                std::fprintf(f, "\nworst %s step at (%.0f, %.0f), heightfield %.1f m; nearest five"
                                " (dist-d0, biome):", name[wv], worst[wv][0], worst[wv][1],
                             wg.height(worst[wv][0], worst[wv][1]));
                for (int k = 0; k < 5; ++k) {
                    std::fprintf(f, " [%.0f b%d]", q.d[k] - q.d[0], sw.biome[q.id[k]]);
                }
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
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

    // Never honour SW_HIDE: every mode that genuinely wants no window has
    // already returned above this line, so there is no caller for whom hidden
    // is the right answer.
    //
    // This is NOT why a window takes a while to appear. Startup compiles the
    // field shaders and builds the whole seed before the first frame, which
    // is about fifteen to twenty seconds -- during which the process burns a
    // core with no window up, and looks exactly like a hang. It is not one.
    // I diagnosed it as one and was wrong; the guard below is defensive, not
    // a fix. No SetForegroundWindow: grabbing focus twenty seconds after
    // launch would interrupt whatever the user moved on to.
    ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);

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

            sync_sea(float(total));
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

            sync_sea(float(total));
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

            sync_sea(float(total));
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
            // A run where the window never got a size renders nothing and
            // classifies nothing, but the time-driven flags below still fire
            // -- so the report cheerfully concluded "it was STOPPED by the
            // overhang" off zero frames of simulation. A check that draws a
            // conclusion from no data is worse than no check, so say it here,
            // first, where it cannot be missed.
            if (st.chunks_classified == 0) {
                std::fprintf(f, "\n*** THIS RUN SIMULATED NOTHING. No chunk was ever\n");
                std::fprintf(f, "*** classified, so no geometry and no physics existed.\n");
                std::fprintf(f, "*** Every number below is void. Run it again.\n\n");
            }
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

    if (swim_seconds > 0.0) {
        // --swim: walk off a beach, swim out, turn round, swim back and walk
        // out. The thing being measured is the bug this replaces -- a
        // character walking along the seabed in water over its head -- so the
        // headline number is frames spent standing in deep water, and it is
        // only worth anything beside the count of deep-water frames it is out
        // of.
        const engine::WorldGpu& wg = g_renderer.world();
        const float sea = wg.params().sea_level;
        const float state = g_renderer.sea_state();
        // "Deep" is deep enough that no trough at this sea state can put the
        // floating feet on the bottom: float depth, plus the full amplitude
        // the trough can fall, plus a metre. Below this, touching the seabed
        // while swimming is legitimate and is not counted.
        const float deep = engine::PhysicsWorld::kFloatDepth +
                           ww_total_amplitude(state) + 1.0f;

        // A beach: the nearest coast to the origin along any of 72 spokes
        // where the land side is low enough to walk down (under 6 m, 20 m
        // in) and the sea side stays sea and gets deep within 40 m.
        float best_rc = 1e9f, dir_x = 0.0f, dir_z = 1.0f;
        for (int sp = 0; sp < 72; ++sp) {
            const float a = 6.2831853f * float(sp) / 72.0f;
            const float dx = std::sin(a), dz = std::cos(a);
            float rc = -1.0f;
            for (float r = 20.0f; r < 6000.0f && r < best_rc; r += 5.0f) {
                if (wg.height(dx * r, dz * r) < sea) { rc = r; break; }
            }
            if (rc < 0.0f) continue;
            const float land = wg.height(dx * (rc - 20.0f), dz * (rc - 20.0f));
            if (land < sea || land > sea + 6.0f) continue;
            bool open = true;
            for (float r = rc; r <= rc + 70.0f; r += 5.0f) {
                if (wg.height(dx * r, dz * r) >= sea) { open = false; break; }
            }
            if (!open) continue;
            if (sea - wg.height(dx * (rc + 40.0f), dz * (rc + 40.0f)) < deep) continue;
            best_rc = rc; dir_x = dx; dir_z = dz;
        }

        FILE* f = std::fopen("build/swim.txt", "w");
        if (best_rc >= 1e9f) {
            if (f) {
                std::fprintf(f, "no beach found within 6 km of the origin that is walkable on\n"
                                "the land side and deeper than %.2f m 40 m out. Nothing was swum.\n",
                             deep);
                std::fclose(f);
            }
            g_renderer.shutdown();
            g_physics.shutdown();
            return 1;
        }

        g_renderer.set_vsync(true);
        g_walk = true;
        const float sx = dir_x * (best_rc - 20.0f), sz = dir_z * (best_rc - 20.0f);
        g_physics.place_character(sx, Spawn::above(sx, sz), sz);
        g_physics.character_position(g_feet);
        const float yaw_out = std::atan2(dir_x, dir_z);
        g_camera.pitch = -0.05f;

        LARGE_INTEGER wfreq, wstart, wprev;
        QueryPerformanceFrequency(&wfreq);
        QueryPerformanceCounter(&wstart);
        wprev = wstart;

        unsigned long long frames = 0, judged = 0;
        unsigned long long n_dry = 0, n_wade = 0, n_swim = 0;
        unsigned long long deep_frames = 0, deep_standing = 0, deep_clear = 0;
        unsigned long long eye_under = 0, fully_under = 0;
        double eye_min = 1e9, eye_max = -1e9, eye_sum = 0.0;
        double flt_min = 1e9, flt_max = -1e9, flt_sum = 0.0;
        double out_max = -1e9, swum = 0.0, deepest_column = 0.0;
        bool shot_taken = false;
        bool returned = false;
        double shot_eye_local = 0.0, shot_eye_sea = 0.0;
        double clamp_lost = 0.0, worst_dt = 0.0;
        engine::Camera shot_cam = g_camera;
        float shot_time = 0.0f;

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
            if (total >= swim_seconds) break;
            // PhysicsWorld::update clamps a step to 50 ms, so a slow frame
            // loses simulated time and the character covers less ground than
            // the clock says. One calm run came back short of the beach on
            // 1522 frames where every other run took ~2390; without this
            // number that read as a swimming failure.
            if (dt > 0.05) clamp_lost += dt - 0.05;
            if (dt > worst_dt) worst_dt = dt;

            // Out for the first half, back for the second. Standing still for
            // the first 1.5 s while residency fills, so the walk starts on
            // ground rather than mid-fall.
            const bool back = total >= swim_seconds * 0.5;
            g_camera.yaw = back ? yaw_out + 3.14159265f : yaw_out;
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            const float spd = total < 1.5 ? 0.0f : 6.0f;
            sync_physics_terrain();
            sync_sea(float(total));
            const float px = g_feet[0], pz = g_feet[2];
            g_physics.update(float(dt), sy * spd, cy * spd, false);
            g_physics.character_position(g_feet);

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
            if (total < 1.5) continue;
            ++judged;

            const engine::Medium md = g_physics.medium();
            if (md == engine::Medium::Dry) ++n_dry;
            else if (md == engine::Medium::Wading) ++n_wade;
            else ++n_swim;
            if (md == engine::Medium::Swimming) {
                swum += std::sqrt((g_feet[0] - px) * (g_feet[0] - px) +
                                  (g_feet[2] - pz) * (g_feet[2] - pz));
            }

            const double along = double(g_feet[0]) * dir_x + double(g_feet[2]) * dir_z;
            if (along - best_rc > out_max) out_max = along - best_rc;

            const float column = sea - wg.height(g_feet[0], g_feet[2]);
            if (column > deepest_column) deepest_column = column;
            if (column > deep) {
                ++deep_frames;
                // The old behaviour, exactly: standing on the bottom in water
                // over your head. Measured as Jolt's ground contact, not as
                // the medium label -- the first version of this counted
                // "not labelled swimming", and a storm reported 53 failures
                // that were all a swimmer thrown clear of a trough with
                // nothing underneath it at all.
                if (g_physics.character_grounded()) ++deep_standing;
                if (md == engine::Medium::Dry) ++deep_clear;
            }

            if (md == engine::Medium::Swimming && g_physics.sea_present()) {
                const double wy = g_physics.water_surface();
                // Eye against the surface ABOVE the eye, recomputed here
                // rather than reusing the feet's: the same column, but a
                // check that read physics' own number back would be a check
                // of nothing.
                const double eye_y = double(g_feet[1]) + kEyeAboveFeet;
                const double surf = double(sea) +
                    ww_height_at(g_feet[0], g_feet[2], float(total), state);
                const double clear = eye_y - surf;
                if (clear < eye_min) eye_min = clear;
                if (clear > eye_max) eye_max = clear;
                eye_sum += clear;
                if (clear < 0.0) ++eye_under;
                // How far under the feet float, against the same surface
                // physics used at the start of the step.
                const double fl = wy - double(g_feet[1]);
                if (fl < flt_min) flt_min = fl;
                if (fl > flt_max) flt_max = fl;
                flt_sum += fl;
                if (g_physics.submersion() >= 2.0f * (kCapsuleHalf + kCapsuleRadius) - 1e-3f)
                    ++fully_under;
            }

            // A first-person frame after the turn, looking back at the beach:
            // the waterline at eye level, which is the thing a screenshot
            // can judge and a number cannot. Taken when the eye is clear of
            // the water -- and, in a sea big enough to have them, in a TROUGH,
            // below still sea level, which is the vantage the water grid
            // could not draw from. The first version shot at the turn
            // whatever was happening, and twice photographed a camera 7 cm
            // under the surface that the report then had to explain.
            const double eye_local = double(g_camera.y) - (double(sea) +
                ww_height_at(g_camera.x, g_camera.z, float(total), state));
            const bool trough_sea = ww_total_amplitude(state) > 1.5f;
            const bool vantage = md == engine::Medium::Swimming && eye_local > 0.2 &&
                                 (!trough_sea || double(g_camera.y) < double(sea) - 1.0);
            if (back && !shot_taken && (vantage || total >= swim_seconds * 0.8)) {
                shot_taken = true;
                engine::ViewConstants cv = {};
                fill_view(g_camera, 1280.0f, 720.0f, float(total), cv);
                g_renderer.set_mode(engine::Mode::Mesh);
                std::string err;
                g_renderer.capture(cv, "build/swim.bmp", err);
                shot_eye_local = double(g_camera.y) - (double(sea) +
                    ww_height_at(g_camera.x, g_camera.z, float(total), state));
                shot_eye_sea = double(g_camera.y) - double(sea);
                shot_cam = g_camera;
                shot_time = float(total);
            }
        }
        returned = g_physics.medium() == engine::Medium::Dry && g_physics.character_grounded();

        const engine::FrameStats st = g_renderer.stats();
        if (f) {
            // An engine run that never got a window size renders nothing and
            // judges nothing, and its verdicts below would still print.
            if (judged == 0 || st.chunks_classified == 0) {
                std::fprintf(f, "VOID RUN: %llu frames judged, %llu chunks classified."
                                " Nothing below means anything.\n\n",
                             judged, st.chunks_classified);
            }
            std::fprintf(f, "swim off a beach and back, %.1f s, sea state %.2f\n",
                         swim_seconds, state);
            std::fprintf(f, "beach: coast %.0f m from the origin along (%.3f, %.3f), "
                            "start 20 m inland\n", best_rc, dir_x, dir_z);
            std::fprintf(f, "frames %llu, judged %llu (after 1.5 s of streaming)\n",
                         frames, judged);
            std::fprintf(f, "worst frame %.0f ms; simulated time lost to the 50 ms step "
                            "clamp: %.1f s of %.1f\n",
                         worst_dt * 1000.0, clamp_lost, swim_seconds);
            std::fprintf(f, "  dry %llu, wading %llu, swimming %llu\n", n_dry, n_wade, n_swim);
            std::fprintf(f, "furthest out: %.1f m past the coast, deepest column %.1f m\n",
                         out_max, deepest_column);
            std::fprintf(f, "distance swum: %.1f m\n", swum);
            std::fprintf(f, "\n'deep' is a column over %.2f m: float depth %.2f + the sea's "
                            "full amplitude %.2f + 1 m\n",
                         deep, engine::PhysicsWorld::kFloatDepth, ww_total_amplitude(state));
            std::fprintf(f, "frames in deep water: %llu\n", deep_frames);
            std::fprintf(f, "  of which standing on the seabed: %llu -> %s\n",
                         deep_standing,
                         deep_frames == 0 ? "NOT TESTED (never reached deep water)"
                                          : (deep_standing == 0 ? "PASS" : "FAIL"));
            std::fprintf(f, "  of which clear of the water entirely (a trough fell away): %llu\n",
                         deep_clear);
            if (n_swim) {
                std::fprintf(f, "\nwhile swimming, against the surface from water_wave.h:\n");
                std::fprintf(f, "  feet below the surface: min %.3f m, mean %.3f m, max %.3f m "
                                "(buoyancy balances at %.2f m)\n",
                             flt_min, flt_sum / double(n_swim), flt_max,
                             engine::PhysicsWorld::kFloatDepth);
                std::fprintf(f, "  eye above the surface:  min %.3f m, mean %.3f m, max %.3f m\n",
                             eye_min, eye_sum / double(n_swim), eye_max);
                std::fprintf(f, "  frames with the eye under: %llu of %llu (%.1f%%)\n",
                             eye_under, n_swim, 100.0 * double(eye_under) / double(n_swim));
                std::fprintf(f, "  frames fully submerged: %llu\n", fully_under);
            }
            std::fprintf(f, "\nback on dry ground at the end: %s%s\n", returned ? "yes" : "NO",
                         !returned && clamp_lost > 2.0
                             ? " -- but the step clamp ate enough time to explain it"
                             : "");
            std::fprintf(f, "chunks classified %llu, physics bodies %zu\n",
                         st.chunks_classified, g_physics.body_count());
            // Which side of the water the eye was on when the picture was
            // taken: a picture of the sea is unreadable without it.
            std::fprintf(f, "shot at the turn: build/swim.bmp, eye %+.2f m from the water above"
                            " it, %+.2f m from still sea level\n",
                         shot_eye_local, shot_eye_sea);
            std::fprintf(f, "  rebuild it: --shot <path> --cam %.2f %.2f %.2f %.4f %.4f"
                            " (water time was %.2f s)\n",
                         shot_cam.x, shot_cam.y, shot_cam.z, shot_cam.yaw, shot_cam.pitch,
                         shot_time);
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
        unsigned long long wet_wading = 0, wet_swimming = 0;
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
            sync_sea(float(total));
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
            // The walk never set out to reach the sea. If it does, its
            // ground numbers stop being about ground, so say so.
            if (g_physics.medium() == engine::Medium::Wading) ++wet_wading;
            else if (g_physics.medium() == engine::Medium::Swimming) ++wet_swimming;

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
            std::fprintf(f, "frames in the sea: wading %llu, swimming %llu\n",
                         wet_wading, wet_swimming);
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
        const std::string fcl = cmdline ? cmdline : "";
        float flight_speed = 60.0f;
        {
            const size_t fs = fcl.find("--flight-speed");
            if (fs != std::string::npos) flight_speed = float(std::atof(fcl.c_str() + fs + 14));
            if (flight_speed <= 0.0f) flight_speed = 60.0f;
        }
        // --settle: hover until nothing is queued before the clock starts.
        // Without it the flight begins in an empty world and spends its whole
        // length on the initial load -- a 1200-chunk backlog that never
        // drains at 60 m/s -- and every "hole" past LOD 2 is that load, not a
        // LOD transition.
        double settle_s = 0.0;
        size_t settle_left = 0;
        if (fcl.find("--settle") != std::string::npos) {
            LARGE_INTEGER sf, s0, s1;
            QueryPerformanceFrequency(&sf);
            QueryPerformanceCounter(&s0);
            for (;;) {
                MSG m = {};
                while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageA(&m);
                }
                RECT cr;
                GetClientRect(hwnd, &cr);
                const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
                QueryPerformanceCounter(&s1);
                settle_s = double(s1.QuadPart - s0.QuadPart) / double(sf.QuadPart);
                if (w < 1.0f || h < 1.0f) { if (settle_s > 30.0) break; continue; }
                engine::ViewConstants v = {};
                fill_view(g_camera, w, h, 0.0f, v);
                g_renderer.render(v);
                const engine::FrameStats ss = g_renderer.stats();
                settle_left = ss.pending;
                if ((ss.pending == 0 && g_renderer.streamer().held_count() == 0) ||
                    settle_s > 30.0) break;
            }
        }

        LARGE_INTEGER ffreq, fstart, fprev;
        QueryPerformanceFrequency(&ffreq);
        QueryPerformanceCounter(&fstart);
        fprev = fstart;

        // --holes: count, every frame, surface chunks queued with nothing drawn
        // over them -- the LOD pop. Off by default because the count costs a
        // height query per queued chunk, which would be charged to the fps.
        const bool measure_holes = std::string(cmdline ? cmdline : "").find("--holes") != std::string::npos;
        g_renderer.set_measure_holes(measure_holes);
        unsigned long long hole_frames[engine::kLodCount] = {0, 0, 0, 0, 0, 0, 0, 0};
        unsigned long long hole_chunks[engine::kLodCount] = {0, 0, 0, 0, 0, 0, 0, 0};
        unsigned long long inner_hole_frames = 0, judged_frames = 0;
        size_t worst_inner = 0, peak_held = 0;
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
            const float step = flight_speed * float(dt < 0.1 ? dt : 0.1);
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
            // After the first two seconds, which are the world loading from
            // nothing and would count every chunk as a hole.
            if (measure_holes && total > 2.0) {
                ++judged_frames;
                const size_t* hb = g_renderer.streamer().holes_by_lod();
                size_t inner = 0;
                for (int L = 0; L < engine::kLodCount; ++L) {
                    if (hb[L]) { ++hole_frames[L]; hole_chunks[L] += hb[L]; }
                    if (L < engine::kLodCount - 1) inner += hb[L];
                }
                if (inner) ++inner_hole_frames;
                if (inner > worst_inner) worst_inner = inner;
            }
            if (g_renderer.streamer().held_count() > peak_held)
                peak_held = g_renderer.streamer().held_count();
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
            std::fprintf(f, "speed %.0f m/s; settled first: %s\n", flight_speed,
                         fcl.find("--settle") == std::string::npos ? "no"
                         : (settle_left == 0 ? "yes" : "NO, gave up with chunks still queued"));
            if (fcl.find("--settle") != std::string::npos)
                std::fprintf(f, "  settling took %.1f s, %zu still queued\n", settle_s, settle_left);
            std::fprintf(f, "LOD hold: %s; peak chunks held %zu, retired early for want of a"
                            " slot %llu\n",
                         no_lod_hold ? "OFF (old behaviour)" : "on",
                         peak_held, g_renderer.streamer().forced_retires());
            if (measure_holes) {
                std::fprintf(f, "holes (a queued surface chunk with nothing drawn over it), "
                                "%llu frames judged after 2 s:\n", judged_frames);
                std::fprintf(f, "  frames with a hole inside the view (LOD 0-%d): %llu (%.1f%%),"
                                " worst frame %zu chunks\n",
                             engine::kLodCount - 2, inner_hole_frames,
                             judged_frames ? 100.0 * double(inner_hole_frames) / double(judged_frames) : 0.0,
                             worst_inner);
                for (int L = 0; L < engine::kLodCount; ++L) {
                    std::fprintf(f, "  LOD %d: %llu frames, %llu chunk-frames%s\n", L,
                                 hole_frames[L], hole_chunks[L],
                                 L == engine::kLodCount - 1 ? "  (the horizon growing, not a hole in it)" : "");
                }
            }
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
            // Ctrl dives. Space is both jump and swim-up, so the way out of
            // the water is the key you would already be pressing.
            const float swim = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? -1.0f : 0.0f;
            sync_physics_terrain();
            sync_sea(t);
            g_physics.update(float(dt), vx, vz, jump, swim);
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
                          "%zu pending, %zu slots free%s%s",
                          g_renderer.mode() == engine::Mode::Mesh ? "mesh" : "wireframe",
                          fps_frames / fps_accum, worst_frame_ms,
                          st.resident, st.indices / 3,
                          st.meshed_this_frame, st.mesh_ms,
                          st.pending, st.free_slots,
                          st.overflowed ? " - SLOT OVERFLOW" : "",
                          !g_walk ? ""
                          : g_physics.medium() == engine::Medium::Swimming ? " - swimming"
                          : g_physics.medium() == engine::Medium::Wading   ? " - wading"
                                                                           : "");
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
