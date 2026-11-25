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
//   WASD / QE   move; fly, shift to go faster. Walking: a jog, Shift
//               sprints, Caps Lock (toggled) walks
//   mouse       look (click to capture, Esc to release); walking, it
//               orbits the third-person camera round the character
//   wheel       walking: camera distance, in to first person; flying: speed
//   Tab         walk (the character, a capsule under physics) / fly
//   Space       jump; in the sea, swim up -- and out over a ledge
//   Ctrl        in the sea, dive
//   [ ]         sea state, calm to storm
//   M           shaded / wireframe
//   T           surface texture: flat, detail (default), 1 m/10 m grid, both
//   F5          reload every shader and remesh
//   F2          write a capture to build/shot.bmp
//   click       (once captured) chop the tree in front of you; enough
//               swings fell it, and the log falls under physics
//   right click (once captured) dig where the screen centre meets the
//               ground; held, keep digging
//   , .         dig radius, 0.5 m (one voxel) to 8 m; Shift: exposure
//   - =         tree draw distance, 100 to 2000 m in 50 m steps
//   F6 / F9     save / load: dig edits and felled trees, under build/
//   F8          snapshot: camera, time, weather, edits and a still, under
//               build/snapshots/<time>/; --snapshot <that dir> stands you there
//   L           sun shadows on / off
//   G           ground cover (grass, flowers, bushes) on / off
//   Y O J       TAA, ambient occlusion, contact shadows on / off
//   Esc         pause menu (resume, options, controls, quit) -- game/scripts
//   F1 or `     the tuning panel: time, weather, fog, exposure, live
// ---------------------------------------------------------------------------
#include "engine/render/renderer.h"
#include "engine/render/water_wave.h"
#include "engine/render/weather.h"
#include "engine/render/shader_compile.h"
#include "engine/render/sky.h"
#include "engine/camera.h"
#include "engine/physics/physics_world.h"
#include "engine/trees/felling.h"
#include "engine/ui/ui.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

engine::Renderer  g_renderer;
engine::Camera    g_camera;
engine::PhysicsWorld g_physics;
engine::TreeFelling  g_felling;
// Where the player starts: worldgen::find_spawn, the most central point well
// inside the calmest biome. Every mode that starts "at spawn" starts here.
worldgen::SpawnPoint g_spawn;
bool  g_walk = false;
bool  g_want_chop = false;
bool  g_want_pick = false;   // E: pick up what the crosshair is on (a glowing mushroom)
bool  g_want_save = false, g_want_load = false;
// F8: a snapshot -- everything needed to stand exactly here again
// (write_snapshot; --snapshot <dir> reads it back).
bool  g_want_snapshot = false;
std::string g_snapshot_dir;          // --snapshot: the folder being reproduced
int   g_snap_walk_frames = -1;       // frames until the snapshot's character is placed
float g_snap_feet[3] = {0, 0, 0};
float g_snap_cam_dist = -1.0f;
char  g_tree_msg[160] = "";
// Digging (right mouse): a sphere carved where the screen centre meets the
// ground. The smallest removes ONE 0.5 m edit voxel; , and . step the size.
// Held, it digs again every kDigRepeat seconds.
const float kDigRadii[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
const int   kDigSizes = int(sizeof(kDigRadii) / sizeof(kDigRadii[0]));
const float kDigReach = 80.0f;
// How far from the head E reaches to pick something up.
const float kPickReach = 2.5f;
const float kDigRepeat = 0.2f;
int   g_dig_size = 0;
bool  g_want_dig = false, g_dig_held = false;
float g_dig_timer = 0.0f;
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

// --- The player in walk mode: a character and a third-person camera -------
//
// The camera orbits a point over the feet at the camera's own yaw and pitch
// (the mouse), g_cam_dist behind it; the wheel sets the distance, and in
// close enough it becomes the old first-person eye with the character
// hidden. Movement is relative to the camera; the character turns to face
// where it is going. A ray through physics (terrain, trunks, rocks) and the
// terrain height pull the camera in so it never sits inside a hill.
engine::CharacterAnimator g_anim;
engine::AnimLibrary g_anim_lib;   // the Universal Animation Library, retargeted
// On foot: a jog by default, Shift sprints, Caps Lock (toggled) walks --
// each near the speed its clip covers (5.4, 8.3 and 1.0 m/s), so the
// stride a clip shows is the stride physics moves.
const float kJogSpeed = 5.0f, kSprintSpeed = 8.0f, kWalkSpeed = 1.4f;
float g_cam_dist = 4.5f;        // wanted, metres; 0 is first person
float g_cam_dist_now = 4.5f;    // after collision, eased back out
float g_char_yaw = 0.0f;        // the character's facing, Camera's convention
float g_char_speed = 0.0f;      // horizontal, m/s, from what physics did
float g_prev_feet[3] = {0.0f, 0.0f, 0.0f};
bool  g_have_prev_feet = false;
std::vector<float> g_bones;
const float kCamDistMin = 1.2f, kCamDistMax = 18.0f;
const float kFirstPersonBelow = 0.9f;   // zooming in past this goes to the eye
const float kCamPivot = 1.55f;          // over the feet: the shoulders

bool first_person() { return g_cam_dist < kFirstPersonBelow; }

// The wheel: in walk mode the camera's distance, 15% a notch, through first
// person at the near end; in flight, the flying speed.
void zoom(float notches) {
    if (!g_walk) {
        g_camera.speed = (std::max)(2.0f, (std::min)(g_camera.speed * std::pow(1.25f, notches), 2000.0f));
        return;
    }
    if (first_person()) {
        if (notches < 0.0f) g_cam_dist = g_cam_dist_now = kCamDistMin;
        return;
    }
    g_cam_dist *= std::pow(0.85f, notches);
    if (g_cam_dist < kCamDistMin) g_cam_dist = 0.0f;
    if (g_cam_dist > kCamDistMax) g_cam_dist = kCamDistMax;
}

// After physics has moved the capsule: the character's speed, facing and
// pose. (vx, vz) is the velocity asked for, which decides the facing.
void update_character(float dt, float t, float vx, float vz) {
    if (!g_anim.ready()) return;
    float speed = 0.0f;
    if (g_have_prev_feet && dt > 1e-5f) {
        const float dx = g_feet[0] - g_prev_feet[0], dz = g_feet[2] - g_prev_feet[2];
        speed = std::sqrt(dx * dx + dz * dz) / dt;
        if (speed > 30.0f) speed = 0.0f;   // a teleport, not a stride
    }
    const float vy = (g_have_prev_feet && dt > 1e-5f) ? (g_feet[1] - g_prev_feet[1]) / dt : 0.0f;
    for (int k = 0; k < 3; ++k) g_prev_feet[k] = g_feet[k];
    g_have_prev_feet = true;
    g_char_speed += (speed - g_char_speed) * (1.0f - std::exp(-12.0f * dt));
    if (first_person()) {
        g_char_yaw = g_camera.yaw;
    } else if (vx * vx + vz * vz > 0.01f) {
        // Turn towards the way we are going, the short way round, quickly.
        const float want = std::atan2(vx, vz);
        float d = want - g_char_yaw;
        d -= 6.2831853f * std::floor((d + 3.14159265f) / 6.2831853f);
        const float step = 12.0f * dt;
        g_char_yaw += d > step ? step : (d < -step ? -step : d);
    }
    engine::LocomotionInput in;
    in.dt = dt;
    in.speed = g_char_speed;
    in.grounded = g_physics.character_supported();
    in.swimming = g_physics.medium() == engine::Medium::Swimming;
    in.vertical = vy;
    in.time = t;
    in.torch = g_renderer.carried_torch();
    g_anim.update(in);
}

// Hand the renderer this frame's pose (or nothing), and the torch's hand.
void publish_character() {
    const bool show = g_walk && g_anim.ready() && !first_person();
    if (show) g_anim.skin(g_feet, g_char_yaw, g_bones);
    g_renderer.character().set_pose(g_bones, show);
    static int hand = -2;
    if (hand == -2) hand = g_anim.ready() ? g_renderer.character().model().find("hand_r") : -1;
    float p[3];
    if (show && g_anim.joint_world(hand, g_feet, g_char_yaw, p)) {
        // A little ahead of and above the fist, where a flame would be.
        p[0] += 0.12f * std::sin(g_char_yaw);
        p[1] += 0.25f;
        p[2] += 0.12f * std::cos(g_char_yaw);
        g_renderer.set_torch_anchor(p);
    } else {
        g_renderer.set_torch_anchor(nullptr);
    }
}

// The walk-mode camera, after the mouse has turned it: the eye in first
// person, else behind the pivot, pulled in by anything in between.
void place_walk_camera(float dt) {
    if (first_person()) {
        g_camera.x = g_feet[0];
        g_camera.y = g_feet[1] + kEyeAboveFeet;
        g_camera.z = g_feet[2];
        g_cam_dist_now = 0.0f;
        return;
    }
    float fwd[3], right[3], up[3];
    g_camera.basis(fwd, right, up);
    const float pivot[3] = {g_feet[0], g_feet[1] + kCamPivot, g_feet[2]};
    const float back[3] = {-fwd[0], -fwd[1], -fwd[2]};
    const float margin = 0.3f;
    float allowed = g_cam_dist;
    int prop = 0;
    const float hit = g_physics.probe_ray(pivot, back, g_cam_dist + margin, prop);
    if (hit >= 0.0f) allowed = (std::max)(0.0f, hit - margin);
    // The heightfield too: physics holds only what has streamed near the
    // player, and the ray can slip between two chunks' shapes.
    const engine::WorldGpu& wg = g_renderer.world();
    for (int i = 0; i < 12 && allowed > 0.2f; ++i) {
        const float x = pivot[0] + back[0] * allowed, z = pivot[2] + back[2] * allowed;
        if (pivot[1] + back[1] * allowed >= wg.height(x, z) + 0.35f) break;
        allowed *= 0.85f;
    }
    // In at once, out gently: a trunk passing between must not make the
    // camera pump.
    if (allowed < g_cam_dist_now || dt <= 0.0f) g_cam_dist_now = allowed;
    else g_cam_dist_now += (allowed - g_cam_dist_now) * (1.0f - std::exp(-5.0f * dt));
    g_camera.x = pivot[0] + back[0] * g_cam_dist_now;
    g_camera.y = pivot[1] + back[1] * g_cam_dist_now;
    g_camera.z = pivot[2] + back[2] * g_cam_dist_now;
}

// Where a swing of the axe comes from: the eye in first person, the head
// otherwise -- the camera is metres behind, out of reach of any trunk.
void chop_origin(float eye[3]) {
    if (g_walk && !first_person()) {
        eye[0] = g_feet[0]; eye[1] = g_feet[1] + kEyeAboveFeet; eye[2] = g_feet[2];
    } else {
        eye[0] = g_camera.x; eye[1] = g_camera.y; eye[2] = g_camera.z;
    }
}

// Layer 9 in a report: what is resident and drawn, the largest outcrop, and
// the nearest outcrop and boulder over 2 m to (x, z), for a --cam. With
// `probe` (a mode where physics runs): colliders round each of those two and
// a ray straight down its axis, which must hit a PROP near the mesh's top --
// not the terrain under it, and not nothing.
void write_rock_report(FILE* f, float x, float z, bool probe) {
    const std::vector<worldgen::RockInstance>& rs = g_renderer.rock_field().rocks();
    size_t outcrops = 0;
    double big = 0.0;
    for (const worldgen::RockInstance& r : rs) { outcrops += r.kind; big = (std::max)(big, double(r.size)); }
    std::fprintf(f, "rocks: %zu resident (%zu outcrops, largest %.1f m), %zu drawn, %zu triangles, %zu colliders\n",
                 rs.size(), outcrops, big, g_renderer.rocks().drawn(), g_renderer.rocks().triangles(),
                 g_renderer.rocks().colliders());
    const worldgen::RockInstance* lg = nullptr;
    for (const worldgen::RockInstance& r : rs)
        if (r.kind == 1 && (!lg || r.size > lg->size)) lg = &r;
    if (lg) std::fprintf(f, "  largest outcrop: (%.1f, %.1f, %.1f) %.1f m across\n", lg->x, lg->y, lg->z, lg->size);
    for (int kind = 0; kind < 2; ++kind) {
        const worldgen::RockInstance* best = nullptr;
        float bd = 1e30f;
        for (const worldgen::RockInstance& r : rs) {
            if (r.kind != kind || (kind == 0 && r.size < 2.0f)) continue;
            const float dx = r.x - x, dz = r.z - z;
            if (dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; best = &r; }
        }
        if (!best) continue;
        std::fprintf(f, "  nearest %s: (%.1f, %.1f, %.1f) %.1f m across, %.0f m away\n",
                     kind ? "outcrop" : "boulder >2 m", best->x, best->y, best->z, best->size, std::sqrt(bd));
        if (!probe) continue;
        g_renderer.rocks().sync_colliders(&g_physics, rs, g_renderer.rock_field().version() + 1000u + kind,
                                          best->x, best->z);
        const engine::RockModel& rm = g_renderer.rocks().model(engine::rock_model(best->kind, best->variant));
        const float top = best->y + rm.height * best->size;
        const float from[3] = {best->x, top + 30.0f, best->z};
        const float down[3] = {0.0f, -1.0f, 0.0f};
        int prop = 0;
        const float hit = g_physics.probe_ray(from, down, 80.0f, prop);
        std::fprintf(f, "    collision: ray down its axis hits %s at y %.2f; mesh top %.2f (tilted up to 8 deg); "
                        "%zu rock colliders, %d shapes%s%s\n",
                     hit < 0.0f ? "NOTHING" : (prop ? "a prop" : "TERRAIN"), hit < 0.0f ? 0.0f : from[1] - hit, top,
                     g_renderer.rocks().colliders(), g_renderer.rocks().shapes_built(),
                     g_physics.prop_error().empty() ? "" : ", last shape error: ", g_physics.prop_error().c_str());
    }
}

// The caves' glowing mushrooms in a report: what is populated, drawn and
// picked, and the nearest cluster for a --cam.
void write_mushroom_report(FILE* f, float x, float y, float z) {
    engine::GlowMushrooms& gm = g_renderer.mushrooms();
    std::fprintf(f, "mushrooms: %zu caves populated, %zu clusters, %zu mushrooms, %zu drawn, %zu picked\n",
                 gm.caves_populated(), gm.clusters(), gm.mushrooms(), gm.drawn(), gm.picked());
    const float p[3] = {x, y, z};
    float c[3];
    if (gm.nearest(p, c))
        std::fprintf(f, "  nearest cluster: (%.1f, %.1f, %.1f), %.1f m away\n", c[0], c[1], c[2],
                     std::sqrt((c[0] - x) * (c[0] - x) + (c[1] - y) * (c[1] - y) + (c[2] - z) * (c[2] - z)));
}

// Hand this frame's streamed height patches to physics, and drop the ones
// whose chunk is gone. Called before the character moves, so it never steps on
// terrain that was retired a frame ago.
// The finest LODs only (engine::kCollisionMaxLod, in streamer.h). Building
// bodies at every LOD cost about 1.4 ms a frame once the sync ran during
// flight as well (--tab-test, fly phase, uncapped).
const int kPhysicsMaxLod = engine::kCollisionMaxLod;

void sync_physics_terrain() {
    const std::vector<engine::Renderer::PatchEvent>& added = g_renderer.patches_added();
    for (size_t i = 0; i < added.size(); ++i) {
        if (added[i].key.lod > kPhysicsMaxLod) continue;
        g_physics.set_chunk(added[i].key, added[i].origin, added[i].voxel,
                            added[i].h.data());
    }
    // The real triangles for chunks the cheap shape cannot hold. These land
    // later than the heights and replace them.
    const std::vector<engine::Renderer::MeshPatchEvent>& meshes =
        g_renderer.mesh_patches_added();
    for (size_t i = 0; i < meshes.size(); ++i) {
        const engine::Renderer::MeshPatchEvent& e = meshes[i];
        // Not handed to physics, so not measured as though it were: the check
        // below is about the shapes the character can stand on.
        if (e.key.lod > kPhysicsMaxLod) continue;
        const size_t nv = e.verts.size() / 3;
        for (size_t k = 0; k < nv; k += 7) {
            const float vy = e.verts[k * 3 + 1];
            if (vy < g_mesh_y_lo) g_mesh_y_lo = vy;
            if (vy > g_mesh_y_hi) g_mesh_y_hi = vy;
            // No skirt to exclude: the renderer leaves skirt vertices out of
            // the patch, so every vertex here is a surface vertex.
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

// The sea cover's cost (PhysicsWorld::set_sea_cover), for --seabed-tunnel.
unsigned long long g_cover_calls = 0, g_cover_ticks = 0;

// Hand physics the sea the renderer is about to draw: same level, same sea
// state, same clock. `t` must be the time this frame passes to fill_view, or
// the swimmer bobs on a swell that is somewhere else on screen. --no-water
// turns the physics off with the drawing, because a sea you cannot see but
// can float on is the same disagreement the other way round.
void sync_sea(float t) {
    engine::SeaParams s;
    s.on = g_renderer.water();
    s.level = g_renderer.world().params().sea_level;
    // The weather at this frame's clock, so the sea is the one about to be
    // drawn even before record_scene asks for it.
    g_renderer.update_weather(t);
    s.state = g_renderer.sea_state();
    s.sea = g_renderer.sea();
    s.swash = g_renderer.swash();
    s.time = t;
    g_physics.set_sea(s);
}

// One frame of physics for the window loop. --tab-test drives this same
// function, so what it measures is what the window does.
//
// The terrain sync runs EVERY frame, walking or not. A chunk's heights reach
// physics exactly once, in the frame its readback lands, and Renderer::render
// clears those events at the start of the next frame. Syncing only while
// walking threw away every chunk loaded during flight, so pressing Tab dropped
// the character into a physics world with no ground in it: --tab-test measured
// 0 terrain bodies at the drop and the feet at -334 m six seconds later.
void step_window_physics(float dt, float t, float vx, float vz, bool jump, float swim) {
    sync_physics_terrain();
    if (!g_walk) return;
    sync_sea(t);
    g_physics.update(dt, vx, vz, jump, swim);
    g_physics.character_position(g_feet);
}
bool g_captured = false;
// The UI layer (engine/ui, game/): absent in every headless mode. Paused
// stops the game clock, physics and the camera; the frame still draws, so the
// tuning panel's changes show behind the menu.
engine::ui::Ui   g_ui;
engine::ui::Host g_host;
bool g_paused = false;
// The display as the options screen sees it. Borderless renders at
// g_display.width x height and DXGI stretches it to the monitor; windowed
// renders at the client size.
engine::ui::DisplayState g_display;
// The window's client size, and so every mode's render size: --res W H
// (1280 x 720 by default), so a cost can be measured at the resolution it
// will be played at.
float g_res_w = 1280.0f, g_res_h = 720.0f;
// --dig-tunnel's geometry, for --adapt-test's walk in: the entrance (x, z),
// the heading, the floor's height and the ground's at the entrance.
struct TestTunnel { bool dug = false; float x0 = 0, z0 = 0, fx = 0, fz = 1, floor_y = 0, h0 = 0; } g_tunnel;
bool g_want_shot = false;
// Time of day running on its own (H): an hour every ten seconds.
bool g_time_lapse = false;

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


// Insert: jump to the next placed cave, nearest first (Shift+Insert back).
// The order is taken from where the jumps started, and kept while each jump
// is followed by another, so repeated presses walk out through every cave.
std::vector<int> g_cave_order;
int   g_cave_at = -1;
float g_cave_from[2] = {0.0f, 0.0f};

void jump_to_cave(int step) {
    const worldgen::SeedWorld& w = g_renderer.world().world();
    if (w.caves.empty()) {
        std::snprintf(g_tree_msg, sizeof g_tree_msg, "no caves in this world");
        return;
    }
    const float px = g_walk ? g_feet[0] : g_camera.x, pz = g_walk ? g_feet[2] : g_camera.z;
    // Moved away since the last jump (or never jumped): order afresh from here.
    bool fresh = g_cave_at < 0 || g_cave_order.size() != w.caves.size();
    if (!fresh) {
        const worldgen::SeedWorld::Cave& c = w.caves[size_t(g_cave_order[size_t(g_cave_at)])];
        const float dx = px - c.x, dz = pz - c.z;
        if (dx * dx + dz * dz > 200.0f * 200.0f) fresh = true;
    }
    if (fresh) {
        g_cave_order.resize(w.caves.size());
        for (size_t i = 0; i < w.caves.size(); ++i) g_cave_order[i] = int(i);
        std::sort(g_cave_order.begin(), g_cave_order.end(), [&](int a, int b) {
            const float ax = w.caves[size_t(a)].x - px, az = w.caves[size_t(a)].z - pz;
            const float bx = w.caves[size_t(b)].x - px, bz = w.caves[size_t(b)].z - pz;
            return ax * ax + az * az < bx * bx + bz * bz;
        });
        g_cave_at = step > 0 ? 0 : int(w.caves.size()) - 1;
        g_cave_from[0] = px; g_cave_from[1] = pz;
    } else {
        const int n = int(w.caves.size());
        g_cave_at = ((g_cave_at + step) % n + n) % n;
    }
    const worldgen::SeedWorld::Cave& c = w.caves[size_t(g_cave_order[size_t(g_cave_at)])];
    const float yaw = std::atan2(c.hx, c.hz);   // forward is (sin yaw, cos yaw)
    if (g_walk) {
        // On the ground just outside the bowl, facing in.
        const float back = w.entrances[size_t(c.seg_begin)].ra * 1.8f + 2.0f;
        const float x = c.x - c.hx * back, z = c.z - c.hz * back;
        g_physics.place_character(x, g_renderer.world().height(x, z) + 1.0f, z);
        g_char_yaw = yaw;
        g_camera.yaw = yaw;
        g_have_prev_feet = false;
    } else {
        g_camera.x = c.x - c.hx * 25.0f;
        g_camera.z = c.z - c.hz * 25.0f;
        g_camera.y = c.y + 12.0f;
        g_camera.yaw = yaw;
        g_camera.pitch = -0.35f;
    }
    const float dx = c.x - g_cave_from[0], dz = c.z - g_cave_from[1];
    std::snprintf(g_tree_msg, sizeof g_tree_msg, "cave %d/%d: %s, %.0f m tunnel, floor %.0f m down, %.0f m from start",
                  g_cave_at + 1, int(w.caves.size()), g_renderer.world().table()[size_t(c.biome)].name.c_str(),
                  c.length, c.y - c.floor_y, std::sqrt(dx * dx + dz * dz));
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_ui.ready() && g_ui.on_message(hwnd, msg, wp, lp, g_captured)) return 0;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                if (g_display.borderless) g_renderer.resize(UINT(g_display.width), UINT(g_display.height));
                else g_renderer.resize(LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_MOUSEWHEEL:
            // Walking: the camera's distance from the character. Flying:
            // the speed.
            zoom(float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA));
            return 0;
        case WM_LBUTTONDOWN:
            // The first click captures the mouse; once captured, a click is
            // a swing of the axe.
            if (g_captured) g_want_chop = true;
            else set_capture(hwnd, true);
            return 0;
        case WM_RBUTTONDOWN:
            // Dig where the screen centre meets the ground; held, keep digging.
            if (g_captured) { g_want_dig = true; g_dig_held = true; g_dig_timer = 0.0f; }
            else set_capture(hwnd, true);
            return 0;
        case WM_RBUTTONUP:
            g_dig_held = false;
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
                // whether the weather actually reads as weather. Fixes the
                // dial; \ hands it back to the automatic weather. The sea
                // answers over weather.sea_lag_s, as it would to a real wind.
                const float d = (wp == VK_OEM_6) ? 0.1f : -0.1f;
                g_renderer.set_sea_state(g_renderer.sea_state() + d);
                return 0;
            }
            if (wp == VK_OEM_5) {
                g_renderer.weather().set_auto();
                return 0;
            }
            if (wp == VK_OEM_1) {
                // ;: turn the wind 45 degrees.
                const engine::WeatherState& w = g_renderer.weather_state();
                float h = std::atan2(w.wind_dir[1], w.wind_dir[0]) + 0.78539816f;
                if (h < 0.0f) h += 6.2831853f;
                g_renderer.weather().set_heading(h);
                return 0;
            }
            if (wp == VK_F3) {
                // The volumetric clouds (ADR-005), for A/B; with Shift, the rain (C3).
                if (GetKeyState(VK_SHIFT) & 0x8000) g_renderer.set_rain_on(!g_renderer.rain_on());
                else g_renderer.set_clouds(!g_renderer.clouds());
                return 0;
            }
            if (wp == VK_F4) {
                // The sea's ripples (FFT detail), for A/B.
                g_renderer.set_wave_detail(!g_renderer.wave_detail());
                return 0;
            }
            if (wp == VK_F7) {
                // Screen-space reflections on the sea, for A/B.
                g_renderer.set_ssr(!g_renderer.ssr());
                return 0;
            }
            if (wp == 'K') {
                // Cycle the water debug views.
                float m = g_renderer.water_debug() + 1.0f;
                if (m > 8.5f) m = 0.0f;
                g_renderer.set_water_debug(m);
                return 0;
            }
            if (wp == 'T') {
                // Surface texture: flat, noise detail, metric grid, both.
                float m = g_renderer.texture_mode() + 1.0f;
                if (m > 3.5f) m = 0.0f;
                g_renderer.set_texture_mode(m);
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
                    g_char_yaw = g_camera.yaw;
                    g_have_prev_feet = false;
                }
                return 0;
            }
            if (wp == VK_F2) { g_want_shot = true; return 0; }
            if (wp == VK_INSERT) { jump_to_cave((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0; }
            if (wp == VK_OEM_MINUS || wp == VK_OEM_PLUS) {
                // The tree draw-distance slider. Terrain is unaffected: it
                // reaches the horizon under the same haze either way.
                float d = g_renderer.tree_settings().max_dist + (wp == VK_OEM_PLUS ? 50.0f : -50.0f);
                d = d < 100.0f ? 100.0f : (d > 2000.0f ? 2000.0f : d);
                g_renderer.tree_settings().max_dist = d;
                return 0;
            }
            if (wp == 'L') { g_renderer.set_shadows(!g_renderer.shadows()); return 0; }
            if (wp == 'G') { g_renderer.set_foliage(!g_renderer.foliage_on()); return 0; }
            if (wp == 'Y') { g_renderer.set_taa(!g_renderer.taa()); return 0; }
            if (wp == 'O') { g_renderer.set_ao(!g_renderer.ao()); return 0; }
            if (wp == 'J') { g_renderer.set_contact(!g_renderer.contact()); return 0; }
            // Exposure and tonemap (ADR-004 L1): U cycles AgX -> ACES -> raw,
            // and , . step the exposure compensation by half a stop.
            if (wp == 'U') { g_renderer.set_tonemap((g_renderer.tonemap() + 2u) % 3u); return 0; }
            // Time of day (ADR-004 L2): Page Up / Page Down step half an hour,
            // H runs the clock at an hour every ten seconds.
            // Shift for five minutes at a time.
            if (wp == VK_PRIOR || wp == VK_NEXT) {
                const float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 5.0f / 60.0f : 0.5f;
                g_renderer.set_time_of_day(g_renderer.time_of_day() + (wp == VK_PRIOR ? step : -step));
                return 0;
            }
            // Local lights (ADR-004 L3): F carries a torch; V drops a campfire
            // on the ground four metres ahead; C clears the campfires.
            if (wp == 'F') { g_renderer.set_carried_torch(!g_renderer.carried_torch()); return 0; }
            // E picks up what the crosshair is on: for now a glowing mushroom
            // in a cave (engine/props/glow_mushrooms.h), counted, no inventory.
            if (wp == 'E') { if (g_captured) g_want_pick = true; return 0; }
            if (wp == 'V') {
                float f[3], r[3], u[3];
                g_camera.basis(f, r, u);
                const float hl = std::sqrt(f[0] * f[0] + f[2] * f[2]);
                engine::LocalLight l = engine::make_campfire();
                l.pos[0] = g_camera.x + (hl > 1e-3f ? f[0] / hl : 0.0f) * 4.0f;
                l.pos[2] = g_camera.z + (hl > 1e-3f ? f[2] / hl : 1.0f) * 4.0f;
                // On the EDITED ground, found down from eye height: the
                // generated height left a fire dropped into a pit floating
                // where the surface had been, and one in a tunnel on the
                // ground above it. Eye height inside rock (ahead is a rising
                // hill): from over the generated surface instead.
                const float eye = g_walk ? g_feet[1] + 1.6f : g_camera.y;
                const float gen = g_renderer.world().height(l.pos[0], l.pos[2]);
                const float from = g_renderer.edited_density(l.pos[0], eye, l.pos[2]) > 0.0f ? gen + 1.0f : eye;
                l.pos[1] = g_renderer.ground_below(l.pos[0], l.pos[2], from, true) + 0.5f;
                g_renderer.add_light(l);
                return 0;
            }
            if (wp == 'C') { g_renderer.clear_lights(); return 0; }
            // The mist (ADR-004 L4), for A/B.
            if (wp == 'N') { g_renderer.set_fog(!g_renderer.fog()); return 0; }
            // The canopy map's shade (L6), for A/B.
            if (wp == 'B') { g_renderer.set_canopy(!g_renderer.canopy()); return 0; }
            // The screen-space bounce (L6), for A/B.
            if (wp == 'X') { g_renderer.set_ssgi(!g_renderer.ssgi()); return 0; }
            // The heightfield GI (L6), for A/B.
            if (wp == 'Z') { g_renderer.set_hfgi(!g_renderer.hfgi()); return 0; }
            if (wp == 'P') { g_renderer.set_pom(!g_renderer.pom()); return 0; }
            if (wp == 'H') { g_time_lapse = !g_time_lapse; return 0; }
            if (wp == VK_OEM_COMMA || wp == VK_OEM_PERIOD) {
                // , . the dig's size; with Shift, exposure by half a stop.
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    g_renderer.set_ev_comp(g_renderer.ev_comp() + (wp == VK_OEM_PERIOD ? 0.5f : -0.5f));
                } else {
                    g_dig_size += (wp == VK_OEM_PERIOD) ? 1 : -1;
                    g_dig_size = g_dig_size < 0 ? 0 : (g_dig_size >= kDigSizes ? kDigSizes - 1 : g_dig_size);
                    std::snprintf(g_tree_msg, sizeof g_tree_msg, "dig radius %.2f m", kDigRadii[g_dig_size]);
                }
                return 0;
            }
            if (wp == VK_F6) { g_want_save = true; return 0; }
            if (wp == VK_F8) { g_want_snapshot = true; return 0; }
            if (wp == VK_F9) { g_want_load = true; return 0; }
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

// --lights <n>, and --lights-report: n campfires on the ground round the
// camera, from a fixed sequence so a still is repeatable. 64 go within 60 m
// and the disc grows with the count, so density stays that of a large camp:
// 255 crammed into the same disc overflowed every cluster, which measured a
// test mistake rather than the engine.
void scatter_lights(int n) {
    const float rmax = 60.0f * std::sqrt((std::max)(float(n), 64.0f) / 64.0f);
    const engine::WorldGpu& wg = g_renderer.world();
    uint32_t h = 0x9e3779b9u;
    for (int i = 0; i < n; ++i) {
        h = h * 1664525u + 1013904223u;
        const float a = float(h >> 8) / 16777216.0f * 6.2831853f;
        h = h * 1664525u + 1013904223u;
        const float r = 8.0f + float(h >> 8) / 16777216.0f * (rmax - 8.0f);
        engine::LocalLight l = engine::make_campfire();
        l.pos[0] = g_camera.x + r * std::cos(a);
        l.pos[2] = g_camera.z + r * std::sin(a);
        l.pos[1] = wg.height(l.pos[0], l.pos[2]) + 0.5f;
        g_renderer.add_light(l);
    }
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

// F8: a folder that stands someone else exactly here. snapshot.txt's "args:"
// line is command-line flags (camera, time, fixed weather, exposure, torch,
// walking), which --snapshot <dir> appends to the command line; edits.bin
// and trees.bin are F6's saves; shot.bmp and shot.bmp.txt are this view as
// a still, its exposure report included (the meter, the histogram), so what
// the screen showed can be compared with what a rerun shows. Returns the
// folder, or "" if it could not be written.
std::string write_snapshot(const engine::ViewConstants& view) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    std::snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    CreateDirectoryA("build/snapshots", nullptr);
    const std::string dir = std::string("build/snapshots/") + name;
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return "";
    std::string err;
    g_renderer.save_edits((dir + "/edits.bin").c_str(), err);
    g_felling.save((dir + "/trees.bin").c_str(), g_renderer.world().field_id(), err);
    FILE* f = std::fopen((dir + "/snapshot.txt").c_str(), "w");
    if (!f) return "";
    const engine::WeatherState& ws = g_renderer.weather_state();
    float heading = std::atan2(ws.wind_dir[1], ws.wind_dir[0]) * 57.29578f;
    if (heading < 0.0f) heading += 360.0f;
    std::fprintf(f, "args: --cam %.3f %.3f %.3f %.5f %.5f --time %.4f --sea-state %.4f --wind %.3f --wind-dir %.2f "
                    "--swell %.3f --ev-comp %.2f",
                 g_camera.x, g_camera.y, g_camera.z, g_camera.yaw, g_camera.pitch, g_renderer.time_of_day(),
                 g_renderer.sea_state(), ws.wind_speed, heading, ws.swell_hs, g_renderer.ev_comp());
    if (g_renderer.rain_override() >= 0.0f) std::fprintf(f, " --rain %.3f", g_renderer.rain_override());
    // The ground's wetness is the weather's recent history, not its state
    // now: fixing the storm dial alone left a soaked tunnel dry on replay,
    // and dry soil is twice as bright.
    {
        float wet_now = 0.0f, puddle_now = 0.0f;
        g_renderer.wetness(wet_now, puddle_now);
        std::fprintf(f, " --wet %.3f", wet_now);
    }
    if (g_renderer.carried_torch()) std::fprintf(f, " --torch");
    if (engine::WorldGpu::world_seed != 0)
        std::fprintf(f, " --seed %llu", static_cast<unsigned long long>(engine::WorldGpu::world_seed));
    // Campfires (V), each where it stands.
    for (const engine::LocalLight& l : g_renderer.lights())
        std::fprintf(f, " --fire %.3f %.3f %.3f", l.pos[0], l.pos[1], l.pos[2]);
    if (g_walk) std::fprintf(f, " --snap-walk %.3f %.3f %.3f %.3f", g_feet[0], g_feet[1], g_feet[2], g_cam_dist);
    std::fprintf(f, "\n");
    std::fprintf(f, "# Reproduce: engine.exe --snapshot %s   (window, standing here)\n", dir.c_str());
    std::fprintf(f, "#            engine.exe --snapshot %s --shot build/repro.bmp   (this view as a still)\n",
                 dir.c_str());
    std::fprintf(f, "# world: field id %08x; walking: %s; feet (%.2f, %.2f, %.2f); edits: %zu voxels in %zu bricks\n",
                 g_renderer.world().field_id(), g_walk ? "yes" : "no", g_feet[0], g_feet[1], g_feet[2],
                 g_renderer.edits().edited_voxels(), g_renderer.edits().brick_count());
    std::fprintf(f, "# near field: %s, %s; heightfield GI %s\n", g_renderer.near_field_on() ? "on" : "off",
                 g_renderer.near_field().ready() ? "ready" : "not ready", g_renderer.hfgi() ? "on" : "off");
    std::fprintf(f, "# campfires: %zu; not kept: logs' poses beyond trees.bin, the clock's own weather\n",
                 g_renderer.lights().size());
    std::fclose(f);
    g_renderer.capture(view, (dir + "/shot.bmp").c_str(), err);
    // What the meter made of it: the still adapts instantly, so this is the
    // fully adapted eye -- the number to compare a rerun against.
    const engine::Renderer::ExposureReport& ex = g_renderer.last_exposure();
    if (ex.valid) {
        if (FILE* fa = std::fopen((dir + "/snapshot.txt").c_str(), "a")) {
            std::fprintf(fa, "# exposure (adapted): EV100 %.2f, metered mean %.4g nits, band %.4g..%.4g nits\n",
                         ex.ev100, ex.metered, ex.lo_nits, ex.hi_nits);
            std::fclose(fa);
        }
    }
    return dir;
}

// --- The script host: what game/scripts may read and set ---------------------
//
// Every knob is a Tunable (engine/ui/script_api.h): a name, a range, a getter
// and a setter. The "Graphics" group is the options screen and is saved to
// build/settings.lua by the script; every other group is the tuning panel,
// for auditioning values live, and is NOT saved -- the authored values stay
// in archetypes.json, where the panel's findings belong once settled.

std::vector<std::pair<int, int> > display_modes(HWND hwnd) {
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    std::vector<std::pair<int, int> > out;
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsA(mi.szDevice, i, &dm); ++i)
        if (dm.dmPelsWidth >= 800 && dm.dmPelsHeight >= 600)
            out.push_back(std::make_pair(int(dm.dmPelsWidth), int(dm.dmPelsHeight)));
    std::sort(out.begin(), out.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

engine::ui::DisplayState current_display(HWND hwnd) {
    engine::ui::DisplayState d = g_display;
    d.vsync = g_renderer.vsync();
    if (!d.borderless) {
        RECT cr;
        GetClientRect(hwnd, &cr);
        d.width = int(cr.right - cr.left);
        d.height = int(cr.bottom - cr.top);
    }
    return d;
}

// Windowed: a framed window with that client size, centred on its monitor.
// Borderless: a popup over the whole monitor, the swapchain at the asked
// size and stretched by DXGI -- so a lower resolution costs what it should
// while the window stays full screen. No exclusive mode: flip-model
// borderless presents as fast, and alt-tab never loses the device.
void apply_display(HWND hwnd, engine::ui::DisplayState d) {
    g_renderer.set_vsync(d.vsync);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    d.width = (std::max)(640, (std::min)(d.width, 7680));
    d.height = (std::max)(360, (std::min)(d.height, 4320));
    g_display = d;
    if (d.borderless) {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrA(hwnd, GWL_STYLE, LONG_PTR(WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_renderer.resize(UINT(d.width), UINT(d.height));
    } else {
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongPtrA(hwnd, GWL_STYLE, LONG_PTR(style));
        RECT rc = {0, 0, LONG(d.width), LONG(d.height)};
        AdjustWindowRect(&rc, style, FALSE);
        const RECT& wa = mi.rcWork;
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        const int x = wa.left + (std::max)(0, int(wa.right - wa.left - ww) / 2);
        const int y = wa.top + (std::max)(0, int(wa.bottom - wa.top - wh) / 2);
        SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        RECT cr;
        GetClientRect(hwnd, &cr);
        g_renderer.resize(UINT(cr.right - cr.left), UINT(cr.bottom - cr.top));
    }
}

void register_tunables() {
    using engine::ui::Tunable;
    using engine::ui::TunableKind;
    std::vector<Tunable>& T = g_host.tunables;
    auto range = [&](const std::string& id, const char* group, const std::string& label, float mn, float mx,
                     float step, const char* unit, std::function<float()> get, std::function<void(float)> set) {
        Tunable t;
        t.id = id; t.group = group; t.label = label;
        t.kind = TunableKind::Range;
        t.min = mn; t.max = mx; t.step = step; t.unit = unit;
        t.get = std::move(get); t.set = std::move(set);
        T.push_back(t);
    };
    auto toggle = [&](const char* id, const char* group, const char* label,
                      std::function<bool()> get, std::function<void(bool)> set) {
        Tunable t;
        t.id = id; t.group = group; t.label = label;
        t.kind = TunableKind::Toggle;
        t.min = 0.0f; t.max = 1.0f; t.step = 1.0f;
        t.get = [get]() { return get() ? 1.0f : 0.0f; };
        t.set = [set](float v) { set(v > 0.5f); };
        T.push_back(t);
    };
    auto choice = [&](const char* id, const char* group, const char* label, std::vector<std::string> names,
                      std::function<float()> get, std::function<void(float)> set) {
        Tunable t;
        t.id = id; t.group = group; t.label = label;
        t.kind = TunableKind::Choice;
        t.min = 0.0f; t.max = float(names.size() - 1); t.step = 1.0f;
        t.choices = std::move(names);
        t.get = std::move(get); t.set = std::move(set);
        T.push_back(t);
    };
    // Static, so the getters and setters below name it rather than capture a
    // reference to a local that dies with this function.
    static engine::Renderer& R = g_renderer;

    // Graphics: the options screen, saved.
    const char* G = "Graphics";
    toggle("shadows", G, "Sun shadows", [&] { return R.shadows(); }, [&](bool b) { R.set_shadows(b); });
    toggle("light_shadows", G, "Torch and fire shadows", [&] { return R.light_shadows(); },
           [&](bool b) { R.set_light_shadows(b); });
    toggle("ao", G, "Ambient occlusion", [&] { return R.ao(); }, [&](bool b) { R.set_ao(b); });
    toggle("contact", G, "Contact shadows", [&] { return R.contact(); }, [&](bool b) { R.set_contact(b); });
    toggle("taa", G, "Temporal anti-aliasing", [&] { return R.taa(); }, [&](bool b) { R.set_taa(b); });
    toggle("pom", G, "Parallax occlusion", [&] { return R.pom(); }, [&](bool b) { R.set_pom(b); });
    toggle("hfgi", G, "Terrain bounce light", [&] { return R.hfgi(); }, [&](bool b) { R.set_hfgi(b); });
    toggle("canopy", G, "Canopy shade", [&] { return R.canopy(); }, [&](bool b) { R.set_canopy(b); });
    toggle("foliage", G, "Grass and flowers", [&] { return R.foliage_on(); }, [&](bool b) { R.set_foliage(b); });
    toggle("trees", G, "Trees", [&] { return R.trees_on(); }, [&](bool b) { R.set_trees(b); });
    // The player's Brightness: a display gamma (dark vision's last step).
    range("brightness", G, "Brightness", 0.6f, 1.8f, 0.05f, "", [&] { return R.dark_vision().brightness; },
          [&](float v) { R.dark_vision().brightness = v; });
    range("tree_distance", G, "Tree draw distance", 100.0f, 2000.0f, 50.0f, "m",
          [&] { return R.tree_settings().max_dist; }, [&](float v) { R.tree_settings().max_dist = v; });
    choice("texture", G, "Surface texture", {"Flat", "Scans", "Grid", "Scans + grid"},
           [&] { return R.texture_mode(); }, [&](float v) { R.set_texture_mode(std::floor(v + 0.5f)); });
    // The scans' anisotropic filtering: 1, 2, 4, 8, 16 as choices 0..4.
    choice("aniso", G, "Texture filtering", {"Trilinear", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x",
                                             "Anisotropic 16x"},
           [&] { return std::floor(std::log2(R.scan_aniso()) + 0.5f); },
           [&](float v) { R.set_scan_aniso(std::exp2(std::floor(v + 0.5f))); });

    // Time.
    range("time", "Time", "Time of day", 0.0f, 24.0f, 0.05f, "h",
          [&] { return R.time_of_day(); }, [&](float v) { R.set_time_of_day(v); });
    toggle("time_lapse", "Time", "Run the clock (1 h / 10 s)", [] { return g_time_lapse; },
           [](bool b) { g_time_lapse = b; });

    // Weather. Setting the storm or the wind fixes the dial; "automatic"
    // hands it back to the seed's weather.
    const char* W = "Weather";
    toggle("weather_auto", W, "Automatic weather", [&] { return R.weather().automatic(); },
           [&](bool b) { if (b) R.weather().set_auto(); else R.set_sea_state(R.sea_state()); });
    range("storm", W, "Storm", 0.0f, 1.0f, 0.01f, "", [&] { return R.sea_state(); },
          [&](float v) { R.set_sea_state(v); });
    range("wind", W, "Wind speed", 0.0f, 30.0f, 0.5f, "m/s", [&] { return R.weather_state().wind_speed; },
          [&](float v) { R.weather().set_wind(v); });
    range("wind_dir", W, "Wind heading", 0.0f, 359.0f, 1.0f, "deg",
          [&] {
              const engine::WeatherState& w = R.weather_state();
              float h = std::atan2(w.wind_dir[1], w.wind_dir[0]) * 57.29578f;
              return h < 0.0f ? h + 360.0f : h;
          },
          [&](float v) { R.weather().set_heading(v * 0.01745329f); });
    range("swell", W, "Swell height", 0.0f, 4.0f, 0.05f, "m", [&] { return R.weather_state().swell_hs; },
          [&](float v) { R.weather().set_swell(v); });
    toggle("rain_auto", W, "Rain follows the storm", [&] { return R.rain_override() < 0.0f; },
           [&](bool b) { R.set_rain(b ? -1.0f : R.rain_intensity()); });
    range("rain", W, "Rain", 0.0f, 1.0f, 0.01f, "", [&] { return R.rain_intensity(); },
          [&](float v) { R.set_rain(v); });
    toggle("wet_auto", W, "Wetness follows the rain", [&] { return R.wet_override() < 0.0f; },
           [&](bool b) {
               float wet = 0.0f, puddle = 0.0f;
               R.wetness(wet, puddle);
               R.set_wet(b ? -1.0f : wet);
           });
    range("wet", W, "Wetness", 0.0f, 1.0f, 0.01f, "",
          [&] {
              float wet = 0.0f, puddle = 0.0f;
              R.wetness(wet, puddle);
              return wet;
          },
          [&](float v) { R.set_wet(v); });
    toggle("clouds", W, "Clouds", [&] { return R.clouds(); }, [&](bool b) { R.set_clouds(b); });
    toggle("rain_drawn", W, "Rain drawn", [&] { return R.rain_on(); }, [&](bool b) { R.set_rain_on(b); });
    toggle("lightning", W, "Lightning", [&] { return R.lightning_on(); }, [&](bool b) { R.set_lightning(b); });

    // Fog. The height layers as their sea-level VISIBILITY (the 2% contrast
    // distance, 3.912 / sigma) and their thickness (the e-folding height);
    // the shader still takes sigma.
    // Bare ground past the near field (scan_library.h GroundLook): the
    // authored values are archetypes.json's ground_look.
    const char* GL = "Terrain";
    engine::GroundLook& gl = R.ground_look();
    range("ground_variation", GL, "Ground: macro variation", 0.0f, 2.0f, 0.05f, "x",
          [&gl] { return gl.variation; }, [&gl](float v) { gl.variation = v; });
    range("ground_fade_from", GL, "Ground: scan fades from", 0.0f, 400.0f, 5.0f, "m",
          [&gl] { return gl.fade[0]; }, [&gl](float v) { gl.fade[0] = v; });
    range("ground_fade_to", GL, "Ground: scan is its mean by", 10.0f, 1000.0f, 5.0f, "m",
          [&gl] { return gl.fade[1]; }, [&gl](float v) { gl.fade[1] = v; });
    toggle("ground_hex", GL, "Ground: hex-tiling", [&gl] { return gl.hex; }, [&gl](bool b) { gl.hex = b; });
    range("ground_far_detail", GL, "Ground: far detail", 0.0f, 1.5f, 0.05f, "x",
          [&gl] { return gl.far_detail; }, [&gl](float v) { gl.far_detail = v; });
    range("ground_far_scale", GL, "Ground: far detail scale", 2.0f, 20.0f, 0.1f, "x tile",
          [&gl] { return gl.far_scale; }, [&gl](float v) { gl.far_scale = v; });

    const char* F = "Fog";
    toggle("mist", F, "Mist (near, patchy)", [&] { return R.fog(); }, [&](bool b) { R.set_fog(b); });
    range("mist_density", F, "Mist density", 0.0f, 5.0f, 0.05f, "x", [&] { return R.fog_density(); },
          [&](float v) { R.set_fog_density(v); });
    range("mist_top", F, "Mist thins above", 0.0f, 400.0f, 5.0f, "m",
          [&] { return R.fog_settings().mist_top[0]; }, [&](float v) { R.fog_settings().mist_top[0] = v; });
    toggle("height_fog", F, "Height fog (exploration)", [&] { return R.height_fog(); },
           [&](bool b) { R.set_height_fog(b); });
    range("height_fog_scale", F, "Height fog density", 0.0f, 5.0f, 0.05f, "x",
          [&] { return R.height_fog_scale(); }, [&](float v) { R.set_height_fog_scale(v); });
    auto layer = [&](const char* id, const char* label, float* lp, float vmin, float vmax, float vstep,
                     float hmax, float hstep) {
        range(std::string(id) + "_vis", F, std::string(label) + ": visibility at sea level", vmin, vmax, vstep,
              "m", [lp] { return lp[0] > 0.0f ? 3.912f / lp[0] : 1e6f; },
              [lp](float v) { lp[0] = 3.912f / (v > 1.0f ? v : 1.0f); });
        range(std::string(id) + "_height", F, std::string(label) + ": thickness", 5.0f, hmax, hstep, "m",
              [lp] { return lp[1]; }, [lp](float v) { lp[1] = v; });
    };
    engine::FogSettings& fs = R.fog_settings();
    layer("ground", "Ground layer", fs.ground, 100.0f, 20000.0f, 50.0f, 500.0f, 5.0f);
    layer("valley", "Valley layer (dawn peak)", fs.valley, 50.0f, 5000.0f, 10.0f, 300.0f, 5.0f);
    layer("deep", "Deep layer", fs.deep, 2000.0f, 200000.0f, 500.0f, 5000.0f, 50.0f);
    range("valley_noon", F, "Valley layer left at noon", 0.0f, 1.0f, 0.01f, "",
          [&] { return R.fog_settings().valley_noon; }, [&](float v) { R.fog_settings().valley_noon = v; });
    range("valley_wind", F, "Wind that clears the valleys", 1.0f, 20.0f, 0.5f, "m/s",
          [&] { return R.fog_settings().valley_wind; }, [&](float v) { R.fog_settings().valley_wind = v; });

    // Exposure.
    // The near field's probes (near_field.h Schedule): how often the box round
    // the camera is traced, and how long probes remember, in Hz and seconds.
    {
        const char* NF = "Near field";
        static engine::NearField::Schedule& S = R.near_field_schedule();
        range("nf_near_memory", NF, "Probes' memory", 0.05f, 3.0f, 0.05f, "s",
              [] { return S.near_memory; }, [](float v) { S.near_memory = v; });
        range("nf_coarse_memory", NF, "Coarse level's memory", 0.1f, 5.0f, 0.1f, "s",
              [] { return S.coarse_memory; }, [](float v) { S.coarse_memory = v; });
        range("nf_light_memory", NF, "By a moving light", 0.02f, 1.0f, 0.02f, "s",
              [] { return S.light_memory; }, [](float v) { S.light_memory = v; });
        // How fast daylight dies down a cave (sky_access.h): archetypes.json
        // `underground` sky_falloff_m; for cave designs to come.
        range("nf_sky_falloff", NF, "Daylight's falloff down a cave", 2.0f, 60.0f, 1.0f, "m",
              [] { return S.sky_falloff; }, [](float v) { S.sky_falloff = v; });
    }
    const char* E = "Exposure";
    range("ev_comp", E, "Exposure compensation", -4.0f, 4.0f, 0.25f, "EV", [&] { return R.ev_comp(); },
          [&](float v) { R.set_ev_comp(v); });
    choice("tonemap", E, "Tonemap", {"Raw", "AgX", "ACES"}, [&] { return float(R.tonemap()); },
           [&](float v) { R.set_tonemap(unsigned(v + 0.5f)); });
    range("agx_saturation", E, "AgX saturation", 0.5f, 2.0f, 0.05f, "", [&] { return R.agx_saturation(); },
          [&](float v) { R.set_agx_saturation(v); });
    range("agx_power", E, "AgX power (contrast)", 0.5f, 2.5f, 0.05f, "", [&] { return R.agx_power(); },
          [&](float v) { R.set_agx_power(v); });
    // Dark vision (renderer.h DarkVision): the eye's floor by its sky, how it
    // adapts, and the display's dark floor.
    {
        engine::Renderer::DarkVision& dv = R.dark_vision();
        range("ev_floor_open", E, "Darkest the eye adapts to, open sky", -3.0f, 4.0f, 0.25f, "EV100",
              [&dv] { return dv.floor_open; }, [&dv](float v) { dv.floor_open = v; });
        range("ev_floor_under", E, "Darkest the eye adapts to, underground", -3.0f, 6.0f, 0.25f, "EV100",
              [&dv] { return dv.floor_under; }, [&dv](float v) { dv.floor_under = v; });
        range("ev_floor_lit", E, "Darkest the eye adapts to, a light in view", -4.0f, 4.0f, 0.25f, "EV100",
              [&dv] { return dv.floor_lit; }, [&dv](float v) { dv.floor_lit = v; });
        range("lit_key", E, "A light in the dark: its lit part over grey", 0.0f, 4.0f, 0.25f, "stops",
              [&dv] { return dv.lit_key; }, [&dv](float v) { dv.lit_key = v; });
        range("adapt_dark", E, "Into the dark", 0.1f, 10.0f, 0.1f, "s",
              [&dv] { return dv.dark; }, [&dv](float v) { dv.dark = v; });
        range("adapt_light", E, "Into the light", 0.1f, 5.0f, 0.05f, "s",
              [&dv] { return dv.light; }, [&dv](float v) { dv.light = v; });
        toggle("dark_display", E, "Dark vision display floor", [&dv] { return dv.on; }, [&dv](bool b) { dv.on = b; });
        range("dark_lift", E, "Dark floor lift", 0.0f, 0.06f, 0.0025f, "",
              [&dv] { return dv.lift; }, [&dv](float v) { dv.lift = v; });
        range("dark_threshold", E, "Colour fades below", 0.01f, 3.0f, 0.01f, "nits",
              [&dv] { return dv.threshold; }, [&dv](float v) { dv.threshold = v; });
    }

    // Digging (right mouse; , . step it). The HUD reads this too.
    {
        std::vector<std::string> names;
        for (int i = 0; i < kDigSizes; ++i) {
            char b[32];
            std::snprintf(b, sizeof b, "%.2f m", kDigRadii[i]);
            names.push_back(b);
        }
        choice("dig_radius", "Dig", "Dig radius", names, [] { return float(g_dig_size); },
               [](float v) {
                   const int i = int(v + 0.5f);
                   g_dig_size = i < 0 ? 0 : (i >= kDigSizes ? kDigSizes - 1 : i);
               });
    }

    // Lights.
    toggle("torch", "Lights", "Carry a torch", [&] { return R.carried_torch(); },
           [&](bool b) { R.set_carried_torch(b); });

    // Trees: the depth prepass, live, for A/B (--no-tree-prepass).
    toggle("tree_prepass", "Trees", "Depth prepass", [&] { return R.tree_prepass(); },
           [&](bool b) { R.set_tree_prepass(b); });
    // Terrain: its depth prepass, live, for A/B (--no-terrain-prepass).
    toggle("terrain_prepass", "Terrain", "Depth prepass", [&] { return R.terrain_prepass(); },
           [&](bool b) { R.set_terrain_prepass(b); });
}

// Brings the UI up over the window loop. Failure leaves the engine as it was
// before the UI existed (Esc releases the mouse, then quits); the reason is
// in build/ui_log.txt.
void setup_ui(HWND hwnd) {
    register_tunables();
    g_host.quit = [] { PostQuitMessage(0); };
    g_host.set_paused = [](bool p) { g_paused = p; };
    g_host.paused = [] { return g_paused; };
    g_host.capture_mouse = [hwnd](bool on) { set_capture(hwnd, on); };
    g_host.display = [hwnd] { return current_display(hwnd); };
    g_host.display_modes = [hwnd] { return display_modes(hwnd); };
    g_host.apply_display = [hwnd](const engine::ui::DisplayState& d) { apply_display(hwnd, d); };
    std::string err;
    if (!g_ui.init(hwnd, g_renderer.device(), g_renderer.width(), g_renderer.height(), &g_host, err)) {
        if (FILE* ef = std::fopen("build/engine_error.txt", "w")) {
            std::fprintf(ef, "ui init failed (running without it):\n%s\n", err.c_str());
            std::fclose(ef);
        }
        return;
    }
    g_renderer.set_overlay([](ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h) {
        g_ui.draw(cmd, rtv, w, h);
    });
}

}  // namespace

// --sea-report: what the weather's sea actually produces, measured on the C++
// side of the SAME function the shader draws. The thing that matters is not
// that waves exist but that the wind maps onto a usable range of weather, and
// that depth tames them -- and the spectrum in weather.cpp is not evidence of
// either, only the numbers it produces are.
//
// The assertion worth making is the one you would never catch by looking: the
// MEAN surface height must stay at sea level whatever the weather. A Gerstner
// sum has zero mean by construction, so a non-zero mean here means an amplitude
// or a steepness term has acquired a bias -- the sea would silently rise or
// fall with the weather, flooding or draining coastlines, and no screenshot of
// a rough sea would look wrong.
namespace {
struct SeaStats { double hi, lo, mean, slope, miss, naive; };

SeaStats measure_sea(const WwSea& sea, float depth, bool invert) {
    const int N = 160;          // sample grid a side
    const float span = 420.0f;  // metres, several of the longest wavelength
    SeaStats r = {-1e30, 1e30, 0.0, 0.0, 0.0, 0.0};
    double sum = 0.0;
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
                const ww_float3 p = ww_surface(x, z, t, sea, depth, 1.0f, 0.0f, nrm);
                if (p.y < r.lo) r.lo = p.y;
                if (p.y > r.hi) r.hi = p.y;
                sum += p.y;
                ++n;
                // ny is cos of the angle from vertical; smaller means steeper.
                const double sl = nrm.y > 1e-6f ? (1.0 / nrm.y) : 1e6;
                if (sl > r.slope) r.slope = sl;
                // The waterline physics asks for: the surface ABOVE (x, z),
                // not the water that started there. Every fourth sample is
                // plenty to find the worst case.
                if (invert && (i & 3) == 0 && (j & 3) == 0) {
                    const ww_float2 rp = ww_rest_point(x, z, t, sea, depth, 0.0f);
                    ww_float3 n2;
                    const ww_float3 q = ww_surface(rp.x, rp.y, t, sea, depth, 1.0f, 0.0f, n2);
                    const double mx = double(q.x) - x, mz = double(q.z) - z;
                    const double miss = std::sqrt(mx * mx + mz * mz);
                    if (miss > r.miss) r.miss = miss;
                    const double naive = std::fabs(double(p.y) - double(q.y));
                    if (naive > r.naive) r.naive = naive;
                }
            }
        }
    }
    r.mean = n ? sum / n : 0.0;
    return r;
}
}  // namespace

static int run_sea_report(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return 1;
    engine::Weather weather;
    std::string err;
    if (!weather.load("worldgen/data/archetypes.json", err)) {
        std::fprintf(f, "%s\n", err.c_str());
        std::fclose(f);
        return 1;
    }
    weather.set_seed(1);
    weather.set_heading(0.6f);

    std::fprintf(f, "The sea surface, measured from engine/render/water_wave.h -- the same\n");
    std::fprintf(f, "text the shader compiles -- raised by engine/render/weather.cpp.\n");
    std::fprintf(f, "Heights are crest to trough over a 420 m patch at two times.\n\n");

    bool ok = true;
    double worst_mean = 0.0;
    auto check_mean = [&](const SeaStats& st) {
        const double am = std::fabs(st.mean);
        if (am > worst_mean) worst_mean = am;
        if (am > 0.05) ok = false;
    };

    // 1. The wind sea alone, deep water, steady wind (the sea fully answered).
    std::fprintf(f, "1. wind sea alone (no swell), deep water:\n");
    std::fprintf(f, "%-7s %8s %8s %9s %9s %9s %9s %9s\n", "wind", "storm", "Hs-m", "crest-m",
                 "trough-m", "height-m", "max-slope", "mean-m");
    const float winds[] = {0.5f, 2.0f, 3.5f, 6.0f, 10.0f, 15.0f, 20.0f, 24.0f};
    double inv_miss = 0.0;
    for (float u : winds) {
        weather.set_wind(u);
        weather.set_swell(0.0f);
        const engine::WeatherState w = weather.at(1000.0);
        const WwSea sea = weather.sea(w);
        const SeaStats st = measure_sea(sea, 1e4f, true);
        check_mean(st);
        if (st.miss > inv_miss) inv_miss = st.miss;
        double m0 = 0.0;
        for (int i = 0; i < WW_WAVES; ++i) m0 += 0.5 * double(sea.amp[i]) * sea.amp[i];
        std::fprintf(f, "%-7.1f %8.2f %8.2f %9.2f %9.2f %9.2f %9.2f %9.4f   (naive waterline error %.3f m)\n",
                     u, w.storm, 4.0 * std::sqrt(m0), st.hi, st.lo, st.hi - st.lo, st.slope, st.mean, st.naive);
    }

    // 2. The swell alone.
    std::fprintf(f, "\n2. swell alone (a calm wind), deep water:\n");
    std::fprintf(f, "%-7s %9s %9s\n", "Hs-m", "height-m", "max-slope");
    const float swells[] = {0.25f, 1.0f, 2.5f};
    for (float hs : swells) {
        weather.set_wind(0.5f);
        weather.set_swell(hs);
        const SeaStats st = measure_sea(weather.sea(weather.at(1000.0)), 1e4f, false);
        check_mean(st);
        std::fprintf(f, "%-7.2f %9.2f %9.2f\n", hs, st.hi - st.lo, st.slope);
    }

    // 3. Depth, at a near gale with its swell: the waves die towards the
    // shore, the long ones first, and nothing stands much taller than the
    // water is deep. The cap is on the significant height (WW_BREAK_HS x
    // depth); the biggest single wave of a random sea runs ~1.6-1.9 times
    // that, so the check is the tallest wave in the patch against 1.1 x
    // depth -- a wave that size is breaking, which is the point.
    std::fprintf(f, "\n3. depth, at 15 m/s wind with a 2 m swell:\n");
    std::fprintf(f, "%-8s %9s %9s %10s\n", "depth-m", "height-m", "limit-m", "verdict");
    weather.set_wind(15.0f);
    weather.set_swell(2.0f);
    const WwSea gale = weather.sea(weather.at(1000.0));
    const float depths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 1e4f};
    double prev = 0.0;
    bool grows = true, held = true;
    for (float d : depths) {
        const SeaStats st = measure_sea(gale, d, d < 100.0f);
        check_mean(st);
        if (st.miss > inv_miss) inv_miss = st.miss;
        const double h = st.hi - st.lo;
        const double limit = 1.1 * d;
        const bool deep = d > 1000.0f;
        const bool in = deep || h <= limit + 0.01;
        if (!in) held = false;
        if (h + 0.01 < prev) grows = false;
        prev = h;
        std::fprintf(f, "%-8.2f %9.2f %9.2f %10s\n", deep ? 9999.0f : d, h,
                     deep ? 0.0 : limit, deep ? "deep" : (in ? "ok" : "TOO TALL"));
    }
    std::fprintf(f, "waves grow with depth up to deep water: %s\n", grows ? "PASS" : "FAIL");
    std::fprintf(f, "no wave taller than its water can hold: %s\n", held ? "PASS" : "FAIL");
    if (!grows || !held) ok = false;

    // 3b. Whitecaps: the trains' folding measure (ww_surface_with's Jacobian,
    // 1 on still water, 0 where a crest folds) against the share of the sea
    // that is whitecap at each wind, Monahan's fit W = 3.84e-6 U^3.41. The
    // J at that quantile is the threshold water.hlsl's foam is fitted to.
    std::fprintf(f, "\n3b. whitecaps: the trains' Jacobian J at the Monahan coverage, deep water, steady wind:\n");
    std::fprintf(f, "%-7s %10s %8s %8s %8s %12s\n", "wind", "Monahan-%", "J 1%", "J 5%", "J 20%", "J@Monahan");
    const float cap_winds[] = {4.0f, 6.0f, 8.0f, 10.0f, 12.0f, 15.0f, 18.0f, 20.0f, 24.0f};
    for (float u : cap_winds) {
        weather.set_wind(u);
        weather.set_swell(0.0f);
        const WwSea sea = weather.sea(weather.at(1000.0));
        const ww_float3 loc = ww_local(sea, 1e4f, 1.0f, 0.0f);
        std::vector<float> js;
        js.reserve(160 * 160 * 2);
        for (int ti = 0; ti < 2; ++ti) {
            const float t = ti == 0 ? 0.0f : 7.3f;
            for (int j = 0; j < 160; ++j) {
                for (int i = 0; i < 160; ++i) {
                    const float x = -210.0f + 420.0f * float(i) / 159.0f;
                    const float z = -210.0f + 420.0f * float(j) / 159.0f;
                    ww_float3 nrm;
                    float jac = 1.0f;
                    ww_surface_with(x, z, t, sea, 1e4f, 1.0f, 0.0f, loc.x, loc.y, nrm, jac);
                    js.push_back(jac);
                }
            }
        }
        std::sort(js.begin(), js.end());
        auto q = [&](double share) { return js[size_t((share < 0.999 ? share : 0.999) * double(js.size() - 1))]; };
        const double w0 = 3.84e-6 * std::pow(double(u), 3.41), w = w0 < 0.3 ? w0 : 0.3;
        std::fprintf(f, "%-7.1f %10.2f %8.3f %8.3f %8.3f %12.3f\n", u, 100.0 * w, q(0.01), q(0.05), q(0.20), q(w));
    }

    // 4. The automatic weather over six hours of four seeds: how often each wind.
    std::fprintf(f, "\n4. automatic weather, seeds 1-4, six hours each sampled every 10 s:\n");
    weather.set_auto();
    int bins[6] = {0, 0, 0, 0, 0, 0};
    const char* names[6] = {"calm <2", "light 2-5", "moderate 5-8", "fresh 8-14", "strong 14-18", "gale 18+"};
    double worst_swell = 0.0;
    const int per_seed = 2160, samples = 4 * per_seed;
    for (int k = 0; k < samples; ++k) {
        if (k % per_seed == 0) weather.set_seed(uint32_t(1 + k / per_seed));
        const engine::WeatherState w = weather.at(double(k % per_seed) * 10.0);
        const float u = w.wind_speed;
        bins[u < 2 ? 0 : u < 5 ? 1 : u < 8 ? 2 : u < 14 ? 3 : u < 18 ? 4 : 5]++;
        if (w.swell_hs > worst_swell) worst_swell = w.swell_hs;
    }
    for (int b = 0; b < 6; ++b) std::fprintf(f, "  %-13s %5.1f%%\n", names[b], 100.0 * bins[b] / samples);
    std::fprintf(f, "  biggest swell %.2f m\n", worst_swell);

    std::fprintf(f, "\nmean surface height must stay at sea level: worst |mean| %.4f m -> %s\n",
                 worst_mean, worst_mean <= 0.05 ? "PASS" : "FAIL (the sea rises with the weather)");
    const bool inv_ok = inv_miss <= 0.001;
    std::fprintf(f, "waterline inversion (%d Newton steps) lands within 1 mm of the asked point: "
                    "worst %.6f m -> %s\n", WW_INVERT_STEPS, inv_miss, inv_ok ? "PASS" : "FAIL (raise WW_INVERT_STEPS)");
    if (!inv_ok) ok = false;
    std::fprintf(f, "\nmax-slope is 1/n.y, so 1.00 is flat and the Gerstner limit is\n");
    std::fprintf(f, "where crests come to a point. Steepness rises with the storm\n");
    std::fprintf(f, "on purpose -- sharpening crests is most of what makes a\n");
    std::fprintf(f, "storm read as one rather than as calm water scaled up.\n");
    std::fclose(f);
    return ok ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    // --snapshot <dir> (F8 writes them): the snapshot's own flags -- camera,
    // time, weather, exposure, torch -- appended to this command line, so
    // every mode reads them as if typed, and anything typed first wins.
    static std::string full_cmdline;
    full_cmdline = cmdline ? cmdline : "";
    {
        const size_t at = full_cmdline.find("--snapshot ");
        if (at != std::string::npos) {
            size_t p = full_cmdline.find_first_not_of(" \t", at + 11);
            size_t e;
            if (p != std::string::npos && full_cmdline[p] == '"') {
                e = full_cmdline.find('"', p + 1);
                ++p;
            } else {
                e = full_cmdline.find_first_of(" \t", p);
            }
            g_snapshot_dir = full_cmdline.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (FILE* f = std::fopen((g_snapshot_dir + "/snapshot.txt").c_str(), "r")) {
                char line[2048];
                while (std::fgets(line, sizeof line, f)) {
                    if (std::strncmp(line, "args:", 5) != 0) continue;
                    std::string a = line + 5;
                    while (!a.empty() && (a.back() == '\n' || a.back() == '\r')) a.pop_back();
                    full_cmdline += " " + a;
                }
                std::fclose(f);
            }
        }
    }
    cmdline = &full_cmdline[0];
    // --sky-report (ADR-004 L2): the atmosphere model against published
    // clear-sky references. CPU only -- no device, no window, no world.
    if (std::string(cmdline ? cmdline : "").find("--sky-report") != std::string::npos) {
        std::string err;
        const bool ok = engine::write_sky_report("build/sky_report.txt", err);
        if (!ok) {
            if (FILE* ef = std::fopen("build/engine_error.txt", "w")) { std::fputs(err.c_str(), ef); std::fclose(ef); }
        }
        return ok ? 0 : 1;
    }
    // --shot <path> renders one frame at a fixed camera and writes it out
    // without showing the window, with residency filled first so the image is
    // the finished world rather than eight chunks of it.
    std::string shot_path;
    bool start_overlay = false, debug_layer = false;
    // The weather (weather.h) is automatic unless one of these fixes it:
    // --sea-state the storm dial 0..1, --wind a speed in m/s, --wind-dir a
    // heading in degrees (the way the air moves, from +x towards +z),
    // --swell a significant height in metres.
    float sea_state = -1.0f, wind_mps = -1.0f, wind_dir_deg = -1000.0f, swell_hs = -1.0f;
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
        // 14 s against the natural roof, then 8 s against a cavity dug for
        // the purpose (phase B in the --overhang block).
        if (cl.find("--overhang") != std::string::npos) overhang_seconds = 22.0;
        if (cl.find("--dig-save") != std::string::npos) mode_dig_save = true;
        else if (cl.find("--dig-verify") != std::string::npos) mode_dig_verify = true;
        else if (cl.find("--dig-stress") != std::string::npos) mode_dig_stress = true;
        else {
            // --dig alone: --dig-row and --dig-settle contain it too.
            for (size_t at = cl.find("--dig"); at != std::string::npos; at = cl.find("--dig", at + 5)) {
                const char nx = at + 5 < cl.size() ? cl[at + 5] : ' ';
                if (nx == ' ' || nx == '\t' || nx == '"') { dig_seconds = 20.0; break; }
            }
        }
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
        auto flag_num = [&](const char* name, float& out) {
            const size_t at = cl.find(name);
            if (at == std::string::npos) return;
            const size_t q = cl.find_first_not_of(" \t", at + std::string(name).size());
            if (q != std::string::npos) out = float(std::atof(cl.c_str() + q));
        };
        flag_num("--wind-dir", wind_dir_deg);
        {
            // "--wind" alone, not the prefix of --wind-dir.
            size_t at = 0;
            while ((at = cl.find("--wind", at)) != std::string::npos) {
                if (cl.compare(at, 10, "--wind-dir") != 0) {
                    const size_t q = cl.find_first_not_of(" \t", at + 6);
                    if (q != std::string::npos) wind_mps = float(std::atof(cl.c_str() + q));
                    break;
                }
                at += 6;
            }
        }
        flag_num("--swell", swell_hs);
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
        // --seed <n>: which world. 0, the default, is the one every measured
        // view in CLAUDE.md was taken on; the world viewer passes others.
        {
            const size_t sd = cl.find("--seed ");
            if (sd != std::string::npos)
                engine::WorldGpu::world_seed = std::strtoull(cl.c_str() + sd + 7, nullptr, 10);
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

    if (std::string(cmdline ? cmdline : "").find("--no-shader-cache") != std::string::npos) {
        engine::shader_cache_enabled() = false;
    }

    if (std::string(cmdline ? cmdline : "").find("--check-dialect") != std::string::npos) {
        // Always a real compile: a cache hit would answer "did it compile
        // last time", which is not the question.
        engine::shader_cache_enabled() = false;
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

    {
        const std::string rs = cmdline ? cmdline : "";
        const size_t r = rs.find("--res ");
        int rw = 0, rh = 0;
        if (r != std::string::npos && std::sscanf(rs.c_str() + r + 6, "%d %d", &rw, &rh) == 2 && rw >= 64 && rh >= 64) {
            g_res_w = float(rw);
            g_res_h = float(rh);
        }
    }
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "biomes2_engine";
    RegisterClassA(&wc);

    // With --res the window is borderless: a framed window is shrunk to fit
    // the screen, and 2560 x 1440 on a 1440p screen came out 1421 tall.
    const bool res_given = cmdline && std::strstr(cmdline, "--res ") != nullptr;
    const DWORD wstyle = res_given ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = {0, 0, LONG(g_res_w), LONG(g_res_h)};
    AdjustWindowRect(&rc, wstyle, FALSE);
    HWND hwnd = CreateWindowA("biomes2_engine", "biomes2 engine",
                              wstyle, res_given ? 0 : CW_USEDEFAULT, res_given ? 0 : CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g_display.width = int(g_res_w);
    g_display.height = int(g_res_h);
    g_display.borderless = res_given;
    g_display.forced = res_given;

    // Every batch mode, not just --shot. The narrow version of this check
    // covered --shot only and --dig-save hung on a message box the very next
    // time a shader failed to compile -- ten minutes of nothing, with the real
    // error already written to build/engine_error.txt. A mode that was asked
    // to run without a human is headless whatever it is called.
    // The report modes too. --band-report was missing from this list, so a
    // shader that failed to compile put a modal box behind every window and
    // the run hung until it was killed -- the trap the comment above names,
    // found again by a [unroll] experiment that did not compile.
    const std::string hl = cmdline ? cmdline : "";
    const bool report_mode = hl.find("--band-report") != std::string::npos ||
                             hl.find("--probe-field") != std::string::npos ||
                             hl.find("--normal-check") != std::string::npos ||
                             hl.find("--blend-report") != std::string::npos;
    const bool fell_mode = hl.find("--fell") != std::string::npos;
    const bool dig_tree_mode = hl.find("--dig-settle") != std::string::npos;
    const bool lights_mode = hl.find("--lights-report") != std::string::npos;
    const bool leak_mode = hl.find("--leak-test") != std::string::npos;
    const bool seabed_mode = hl.find("--seabed-tunnel") != std::string::npos;
    const bool headless = report_mode || fell_mode || dig_tree_mode || lights_mode || leak_mode ||
                          seabed_mode ||
                          !shot_path.empty() || flight_seconds > 0.0 ||
                          walk_seconds > 0.0 || swim_seconds > 0.0 ||
                          overhang_seconds > 0.0 ||
                          dig_seconds > 0.0 || mode_dig_save || mode_dig_verify ||
                          mode_dig_stress || sea_shot ||
                          std::string(cmdline ? cmdline : "").find("--tab-test") != std::string::npos ||
                          std::string(cmdline ? cmdline : "").find("--pose-sheet") != std::string::npos;

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
    {
        // --character male|female: which packed character (tools/models).
        const std::string cl = cmdline ? cmdline : "";
        const size_t ch = cl.find("--character ");
        if (ch != std::string::npos) {
            std::string name = cl.substr(ch + 12);
            name = name.substr(0, name.find(' '));
            for (char c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) name.clear();
            if (!name.empty()) g_renderer.set_character_pack("assets/models/packed/" + name + ".chr");
        }
    }
    const double init_t0 = engine::startup_now_ms();
    const bool init_ok = g_renderer.init(hwnd, error, debug_layer);
    // Dark vision's rays (the ground under the eye, whether a light is in
    // clear view of it): through the physics world -- terrain near the
    // player, edits included, trunks, rocks.
    g_renderer.set_ray_query([](const float from[3], const float dir[3], float len) {
        int prop = 0;
        return g_physics.probe_ray(from, dir, len, prop);
    });
    if (init_ok && g_renderer.character().ready()) {
        // The clips, retargeted onto this character; without them the
        // procedural gait.
        std::string aerr;
        const double ta = engine::startup_now_ms();
        const bool clips = g_anim_lib.load("assets/models/packed/ual.anim", g_renderer.character().model(), aerr);
        if (FILE* sf = std::fopen("build/startup.txt", "a")) {
            std::fprintf(sf, "%9.0f ms  animation clips -- %s\n", engine::startup_now_ms() - ta,
                         clips ? g_anim_lib.note.c_str() : aerr.c_str());
            std::fclose(sf);
        }
        g_anim.bind(&g_renderer.character().model(), clips ? &g_anim_lib : nullptr);
    }
    {
        // Where startup went. The 15-20 s before the first frame was always
        // described as "shader compilation plus the world build" and never
        // split; this splits it.
        const double init_ms = engine::startup_now_ms() - init_t0;
        if (FILE* sf = std::fopen("build/startup.txt", "w")) {
            double compile_ms = 0.0;
            const std::vector<std::pair<std::string, double> >& lg = engine::startup_log();
            for (size_t i = 0; i < lg.size(); ++i) {
                std::fprintf(sf, "%9.0f ms  %s\n", lg[i].second, lg[i].first.c_str());
                if (lg[i].first.compare(0, 8, "compile ") == 0) compile_ms += lg[i].second;
            }
            std::fprintf(sf, "renderer init: %.0f ms, of which shader compiles %.0f ms "
                             "(shader cache %s)\n",
                         init_ms, compile_ms, engine::shader_cache_enabled() ? "on" : "OFF");
            std::fclose(sf);
        }
    }
    if (!init_ok) {
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
    g_felling.init(&g_renderer, &g_physics);
    {
        const double ts = engine::startup_now_ms();
        const engine::WorldGpu& wg = g_renderer.world();
        g_spawn = worldgen::find_spawn(wg.world(), wg.params(), wg.table());
        // The window opens over the start, 40 m up, looking along +z at it.
        g_camera.x = g_spawn.x;
        g_camera.z = g_spawn.z - 60.0f;
        g_camera.y = g_spawn.ground + 40.0f;
        if (FILE* sf = std::fopen("build/startup.txt", "a")) {
            std::fprintf(sf, "%9.0f ms  seed %llu: spawn at (%.0f, %.1f, %.0f), %.0f m from the centre, "
                             "calmest biome %d at weight %.2f%s\n",
                         engine::startup_now_ms() - ts,
                         static_cast<unsigned long long>(engine::WorldGpu::world_seed),
                         g_spawn.x, g_spawn.ground, g_spawn.z,
                         std::sqrt(g_spawn.x * g_spawn.x + g_spawn.z * g_spawn.z),
                         g_spawn.biome, g_spawn.weight,
                         g_spawn.strict ? "" : " (FALLBACK: no point passed every test)");
            // Layer 5b: how many level sites the seed has, and the nearest.
            const std::vector<worldgen::SeedWorld::FlatSite>& fl = wg.world().flats;
            size_t best = fl.size();
            float bd2 = 3.4e38f;
            for (size_t i = 0; i < fl.size(); ++i) {
                const float dx = fl[i].x - g_spawn.x, dz = fl[i].z - g_spawn.z;
                if (dx * dx + dz * dz < bd2) { bd2 = dx * dx + dz * dz; best = i; }
            }
            if (best < fl.size()) {
                std::fprintf(sf, "           flat sites: %zu; nearest to spawn at (%.0f, %.1f, %.0f), "
                                 "radius %.0f m, shoulder %.0f m, %.0f m away\n",
                             fl.size(), fl[best].x, fl[best].level, fl[best].z,
                             fl[best].radius, fl[best].shoulder, std::sqrt(bd2));
            } else {
                std::fprintf(sf, "           flat sites: none\n");
            }
            // Placed caves: the nearest few to spawn, each as a --cam that
            // stands 25 m behind its mouth looking in. Insert in the window
            // jumps from cave to cave.
            const std::vector<worldgen::SeedWorld::Cave>& cv = wg.world().caves;
            std::vector<std::pair<float, size_t> > cnear;
            for (size_t i = 0; i < cv.size(); ++i) {
                const float dx = cv[i].x - g_spawn.x, dz = cv[i].z - g_spawn.z;
                cnear.push_back(std::make_pair(dx * dx + dz * dz, i));
            }
            std::sort(cnear.begin(), cnear.end());
            std::fprintf(sf, "           caves: %zu (Insert jumps to the next, Shift+Insert back)\n", cv.size());
            for (size_t k = 0; k < cnear.size() && k < 8; ++k) {
                const worldgen::SeedWorld::Cave& c = cv[cnear[k].second];
                std::fprintf(sf, "             %-9s %5.0f m away  --cam %.1f %.1f %.1f %.2f -0.35\n",
                             wg.table()[size_t(c.biome)].name.c_str(), std::sqrt(cnear[k].first),
                             c.x - c.hx * 25.0f, c.y + 12.0f, c.z - c.hz * 25.0f, std::atan2(c.hx, c.hz));
            }
            std::fclose(sf);
        }
    }
    // A snapshot: its camera (the window opens there; --shot re-applies it),
    // its edits and felled trees, and -- in the window -- the character back
    // at its feet once the ground round them has streamed in.
    if (have_cam) {
        g_camera.x = cam_arg[0]; g_camera.y = cam_arg[1]; g_camera.z = cam_arg[2];
        g_camera.yaw = cam_arg[3]; g_camera.pitch = cam_arg[4];
    }
    if (!g_snapshot_dir.empty()) {
        std::string err;
        bool mismatch = false;
        g_renderer.load_edits((g_snapshot_dir + "/edits.bin").c_str(), err, mismatch);
        g_felling.load((g_snapshot_dir + "/trees.bin").c_str(), g_renderer.world().field_id(), err, mismatch);
        const std::string cl = cmdline;
        const size_t sw = cl.find("--snap-walk ");
        if (sw != std::string::npos &&
            std::sscanf(cl.c_str() + sw + 12, "%f %f %f %f", &g_snap_feet[0], &g_snap_feet[1], &g_snap_feet[2],
                        &g_snap_cam_dist) == 4)
            g_snap_walk_frames = 90;
        if (mismatch) {
            if (FILE* sf = std::fopen("build/startup.txt", "a")) {
                std::fprintf(sf, "           snapshot %s was made in ANOTHER world (seed or generator version)\n",
                             g_snapshot_dir.c_str());
                std::fclose(sf);
            }
        }
    }
    if (start_overlay) g_renderer.set_mode(engine::Mode::Wireframe);
    if (sea_state >= 0.0f) g_renderer.set_sea_state(sea_state);
    if (wind_mps >= 0.0f) g_renderer.weather().set_wind(wind_mps);
    if (wind_dir_deg > -999.0f) {
        float rad = wind_dir_deg * 0.017453293f;
        while (rad < 0.0f) rad += 6.2831853f;
        g_renderer.weather().set_heading(rad);
    }
    if (swell_hs >= 0.0f) g_renderer.weather().set_swell(swell_hs);
    g_renderer.set_water_debug(water_debug);
    if (no_water) g_renderer.set_water(false);
    if (cmdline && std::string(cmdline).find("--no-ssr") != std::string::npos) g_renderer.set_ssr(false);
    if (cmdline && std::string(cmdline).find("--no-wave-detail") != std::string::npos) g_renderer.set_wave_detail(false);
    if (cmdline && std::string(cmdline).find("--no-clouds") != std::string::npos) g_renderer.set_clouds(false);
    if (cmdline && std::string(cmdline).find("--no-rain") != std::string::npos) g_renderer.set_rain_on(false);
    if (cmdline && std::string(cmdline).find("--no-lightning") != std::string::npos) g_renderer.set_lightning(false);
    if (cmdline && std::string(cmdline).find("--no-height-fog") != std::string::npos) g_renderer.set_height_fog(false);
    if (cmdline) {
        const std::string c4 = cmdline;
        const size_t hk = c4.find("--height-fog ");
        if (hk != std::string::npos) g_renderer.set_height_fog_scale(float(std::atof(c4.c_str() + hk + 13)));
    }
    if (cmdline) {
        const std::string c3 = cmdline;
        const size_t rn = c3.find("--rain ");
        if (rn != std::string::npos) g_renderer.set_rain(float(std::atof(c3.c_str() + rn + 7)));
        // --lightning <dist m> <bearing deg>: a ground strike for a still, the
        // bearing from the camera's forward, positive to the right.
        const size_t lt = c3.find("--lightning ");
        if (lt != std::string::npos) {
            float d = 2000.0f, b = 0.0f;
            int in_cloud = 0;
            std::sscanf(c3.c_str() + lt + 12, "%f %f %d", &d, &b, &in_cloud);
            g_renderer.lightning().force(d, b * 0.017453293f, in_cloud != 0);
        }
        const size_t wt = c3.find("--wet ");
        if (wt != std::string::npos) g_renderer.set_wet(float(std::atof(c3.c_str() + wt + 6)));
    }
    if (no_lod_hold) g_renderer.set_lod_hold(false);
    {
        const std::string c2 = cmdline ? cmdline : "";
        if (c2.find("--no-prioritise") != std::string::npos) g_renderer.set_prioritise(false);
        if (c2.find("--no-lod-align") != std::string::npos) g_renderer.set_lod_align(false);
        const size_t md = c2.find("--mesh-debug");
        if (md != std::string::npos) g_renderer.set_mesh_debug(float(std::atof(c2.c_str() + md + 12)));
        const size_t tx = c2.find("--texture");
        if (tx != std::string::npos) g_renderer.set_texture_mode(float(std::atof(c2.c_str() + tx + 9)));
        if (c2.find("--adaptive-budget") != std::string::npos) g_renderer.set_adaptive_budget(true);
        // Trees: --tree-distance <m> is the draw-distance slider's starting
        // value; --tree-debug 1 tints by level, 2/3/4 draw only LOD 0, LOD 1
        // or impostors; --no-trees for an A/B against bare ground.
        const size_t td = c2.find("--tree-distance");
        if (td != std::string::npos) {
            const float d = float(std::atof(c2.c_str() + td + 15));
            if (d > 10.0f) g_renderer.tree_settings().max_dist = d;
        }
        const size_t tg = c2.find("--tree-debug");
        if (tg != std::string::npos) g_renderer.tree_settings().debug = float(std::atof(c2.c_str() + tg + 12));
        if (c2.find("--no-trees") != std::string::npos) g_renderer.set_trees(false);
        if (c2.find("--no-tree-prepass") != std::string::npos) g_renderer.set_tree_prepass(false);
        if (c2.find("--no-terrain-prepass") != std::string::npos) g_renderer.set_terrain_prepass(false);
        if (c2.find("--no-shadows") != std::string::npos) g_renderer.set_shadows(false);
        if (c2.find("--no-foliage") != std::string::npos) g_renderer.set_foliage(false);
        if (c2.find("--no-taa") != std::string::npos) g_renderer.set_taa(false);
        if (c2.find("--no-ao") != std::string::npos) g_renderer.set_ao(false);
        if (c2.find("--no-contact") != std::string::npos) g_renderer.set_contact(false);
        if (c2.find("--no-post") != std::string::npos) {
            g_renderer.set_taa(false); g_renderer.set_ao(false); g_renderer.set_contact(false);
        }
        // Local lights (ADR-004 L3): carry a torch; local shadows off for A/B.
        if (c2.find("--torch") != std::string::npos) g_renderer.set_carried_torch(true);
        if (c2.find("--no-light-shadows") != std::string::npos) g_renderer.set_light_shadows(false);
        // The mist (ADR-004 L4): off, or thicker or thinner than authored.
        if (c2.find("--no-fog") != std::string::npos) g_renderer.set_fog(false);
        if (c2.find("--no-canopy") != std::string::npos) g_renderer.set_canopy(false);
        if ((" " + c2 + " ").find(" --ssgi ") != std::string::npos) g_renderer.set_ssgi(true);
        if (c2.find("--no-ssgi") != std::string::npos) g_renderer.set_ssgi(false);
        if (c2.find("--no-hfgi") != std::string::npos) g_renderer.set_hfgi(false);
        if (c2.find("--no-near-field") != std::string::npos) g_renderer.set_near_field(false);
        // --fire x y z (any number): a campfire there -- what a snapshot
        // writes for each one dropped with V.
        for (size_t fp = c2.find("--fire "); fp != std::string::npos; fp = c2.find("--fire ", fp + 7)) {
            engine::LocalLight l = engine::make_campfire();
            if (std::sscanf(c2.c_str() + fp + 7, "%f %f %f", &l.pos[0], &l.pos[1], &l.pos[2]) == 3)
                g_renderer.add_light(l);
        }
        // --fire-ground x z (any number): a campfire on the edited ground
        // under (x, z) -- the floor of a pit, for a test.
        for (size_t fp = c2.find("--fire-ground "); fp != std::string::npos; fp = c2.find("--fire-ground ", fp + 14)) {
            engine::LocalLight l = engine::make_campfire();
            if (std::sscanf(c2.c_str() + fp + 14, "%f %f", &l.pos[0], &l.pos[2]) == 2) {
                const float gen = g_renderer.world().height(l.pos[0], l.pos[2]);
                l.pos[1] = g_renderer.ground_below(l.pos[0], l.pos[2], gen + 1.0f, true) + 0.5f;
                g_renderer.add_light(l);
            }
        }
        // --nf-schedule <fine s> <coarse s> <light s>: the probes' memories.
        {
            const size_t ns = c2.find("--nf-schedule ");
            engine::NearField::Schedule& s = g_renderer.near_field_schedule();
            if (ns != std::string::npos)
                std::sscanf(c2.c_str() + ns + 14, "%f %f %f", &s.near_memory, &s.coarse_memory, &s.light_memory);
            // --sky-falloff <m>: how fast daylight dies down a cave (sky_access.h).
            const size_t sf = c2.find("--sky-falloff ");
            if (sf != std::string::npos) s.sky_falloff = float(std::atof(c2.c_str() + sf + 14));
        }
        // Dark vision for A/B: --ev-floor <open> <under> (EV100), the display
        // floor off, the player's brightness.
        {
            engine::Renderer::DarkVision& dv = g_renderer.dark_vision();
            const size_t ef = c2.find("--ev-floor ");
            if (ef != std::string::npos) std::sscanf(c2.c_str() + ef + 11, "%f %f", &dv.floor_open, &dv.floor_under);
            if (c2.find("--no-dark-display") != std::string::npos) dv.on = false;
            const size_t br = c2.find("--brightness ");
            if (br != std::string::npos) dv.brightness = float(std::atof(c2.c_str() + br + 13));
        }
        if (c2.find("--no-far-shadow") != std::string::npos) g_renderer.set_far_shadow(false);
        if (c2.find("--no-pom") != std::string::npos) g_renderer.set_pom(false);
        {
            // The scans' anisotropic filtering (1 trilinear, 2, 4, 8, 16).
            const size_t an = c2.find("--aniso ");
            if (an != std::string::npos) g_renderer.set_scan_aniso(float(std::atof(c2.c_str() + an + 8)));
        }
        if (c2.find("--no-rocks") != std::string::npos) g_renderer.set_rocks(false);
        if (c2.find("--no-mushrooms") != std::string::npos) g_renderer.set_mushrooms(false);
        // Bare ground (GroundLook): --no-ground-look is the scans as they
        // were; the others set one part. Not "--ground-...": --ground is
        // taken (the camera at eye height), and a find() for it matches.
        {
            engine::GroundLook& gl = g_renderer.ground_look();
            if (c2.find("--no-ground-look") != std::string::npos) {
                gl.variation = 0.0f;
                gl.fade[0] = 1e6f;
                gl.fade[1] = 2e6f;
                gl.hex = false;
                gl.far_detail = 0.0f;
            }
            if (c2.find("--no-hex") != std::string::npos) gl.hex = false;
            const size_t fd = c2.find("--far-detail ");
            if (fd != std::string::npos) gl.far_detail = float(std::atof(c2.c_str() + fd + 13));
            const size_t gv = c2.find("--macro-var ");
            if (gv != std::string::npos) gl.variation = float(std::atof(c2.c_str() + gv + 12));
            const size_t gf = c2.find("--detail-fade ");
            if (gf != std::string::npos) {
                char* e = nullptr;
                gl.fade[0] = std::strtof(c2.c_str() + gf + 14, &e);
                gl.fade[1] = std::strtof(e, nullptr);
            }
        }
        if (c2.find("--no-forest-shell") != std::string::npos) g_renderer.set_forest_shell(false);
        const size_t fdn = c2.find("--fog-density");
        if (fdn != std::string::npos) g_renderer.set_fog_density(float(std::atof(c2.c_str() + fdn + 13)));
        // Time of day (ADR-004 L2), hours: --time 6.5 is half past six.
        const size_t tod = c2.find("--time");
        if (tod != std::string::npos) g_renderer.set_time_of_day(float(std::atof(c2.c_str() + tod + 6)));
        // Exposure and tonemap (ADR-004 L1).
        const size_t evc = c2.find("--ev-comp");
        if (evc != std::string::npos) g_renderer.set_ev_comp(float(std::atof(c2.c_str() + evc + 9)));
        const size_t evf = c2.find("--ev100");
        if (evf != std::string::npos) g_renderer.set_ev100_fixed(float(std::atof(c2.c_str() + evf + 7)));
        const size_t tmo = c2.find("--tonemap");
        if (tmo != std::string::npos) g_renderer.set_tonemap(unsigned(std::atoi(c2.c_str() + tmo + 9)));
        const size_t asat = c2.find("--agx-sat");
        if (asat != std::string::npos) g_renderer.set_agx_saturation(float(std::atof(c2.c_str() + asat + 9)));
        const size_t apow = c2.find("--agx-power");
        if (apow != std::string::npos) g_renderer.set_agx_power(float(std::atof(c2.c_str() + apow + 11)));
        const size_t pdb = c2.find("--post-debug");
        if (pdb != std::string::npos) g_renderer.set_post_debug(float(std::atof(c2.c_str() + pdb + 12)));
        const size_t fd = c2.find("--foliage-debug");
        if (fd != std::string::npos) g_renderer.foliage_settings().debug = float(std::atof(c2.c_str() + fd + 15));
        {
            const size_t gw = c2.find("--grass-width ");
            if (gw != std::string::npos) g_renderer.foliage_settings().blade_width = float(std::atof(c2.c_str() + gw + 14));
            const size_t gt = c2.find("--grass-tuft ");
            if (gt != std::string::npos) g_renderer.foliage_settings().tuft = float(std::atof(c2.c_str() + gt + 13));
            const size_t gd = c2.find("--grass-density ");
            if (gd != std::string::npos) g_renderer.foliage_settings().max_blades = float(std::atof(c2.c_str() + gd + 16));
        }
        // Where LOD 0 hands over to LOD 1, in metres: 0 shows LOD 1 up close.
        const size_t tl = c2.find("--tree-lod0");
        if (tl != std::string::npos) g_renderer.tree_settings().lod0_dist = float(std::atof(c2.c_str() + tl + 11));
    }
    if (lod_fade >= 0) g_renderer.set_lod_fade_frames(lod_fade);
    g_physics.set_column_height([](float x, float z) {
        return g_renderer.world().height(x, z);
    });
    // No sea under a roof (ADR-006 stage 1). BIOMES_SEA_COLUMN_ONLY=1 leaves
    // the column rule alone: the old behaviour, --seabed-tunnel's mutant.
    if (!std::getenv("BIOMES_SEA_COLUMN_ONLY")) {
        g_physics.set_sea_cover([](float x, float z, float lo, float hi) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            const bool solid = g_renderer.solid_between(x, z, lo, hi);
            QueryPerformanceCounter(&b);
            ++g_cover_calls;
            g_cover_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
            return solid;
        });
    }

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
            // The fixed vantage, relative to the start: 70 m behind it
            // and 22 m up, looking at it.
            g_camera.x = g_spawn.x + 10.0f; g_camera.z = g_spawn.z - 70.0f;
            g_camera.y = g_renderer.world().height(g_camera.x, g_camera.z) + 22.0f;
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
        {
            const std::string cl = cmdline ? cmdline : "";
            // --ground: the camera's x and z, at eye height on the ground.
            if (cl.find("--ground") != std::string::npos)
                g_camera.y = g_renderer.world().height(g_camera.x, g_camera.z) + 1.7f;
            const size_t ln = cl.find("--lights ");
            if (ln != std::string::npos) scatter_lights(std::atoi(cl.c_str() + ln + 9));
        }
        engine::ViewConstants v = {};
        fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);

        // --ui "<lua>": bring the UI up, run the code against it (say,
        // menu.open()) and draw it into the still -- the one way to check a
        // document without a person at the window. Without it a shot holds
        // no UI at all.
        {
            const std::string cl = cmdline ? cmdline : "";
            const size_t ui = cl.find("--ui ");
            if (ui != std::string::npos) {
                std::string code;
                size_t p = cl.find_first_not_of(" 	", ui + 5);
                if (p != std::string::npos && cl[p] == '"') {
                    const size_t e = cl.find('"', p + 1);
                    code = cl.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
                } else if (p != std::string::npos) {
                    code = cl.substr(p, cl.find_first_of(" 	", p) - p);
                }
                g_display.forced = true;   // the still's size is the command line's
                setup_ui(hwnd);
                if (g_ui.ready()) {
                    g_ui.run(code);
                    for (int k = 0; k < 4; ++k) g_ui.update(0.25f);   // past the panel's refresh
                    g_renderer.set_capture_overlay(true);
                }
            }
        }

        // --dig-row: before the still, one aimed dig of every size, left to
        // right across the view at 60% of its height (smallest on the left),
        // through the same Renderer::dig_aimed the right mouse uses.
        // <shot>.dig.txt says where each landed and what it wrote.
        if (std::string(cmdline ? cmdline : "").find("--dig-row") != std::string::npos) {
            const std::string dp = shot_path + ".dig.txt";
            FILE* df = std::fopen(dp.c_str(), "w");
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
            if (df) std::fprintf(df, "aimed digs from (%.2f, %.2f, %.2f) yaw %.3f pitch %.3f\n",
                                 eye[0], eye[1], eye[2], g_camera.yaw, g_camera.pitch);
            for (int i = 0; i < kDigSizes; ++i) {
                const float px = g_res_w * (0.08f + 0.84f * float(i) / float(kDigSizes - 1));
                const float py = g_res_h * 0.6f;
                const float nx = (px / g_res_w) * 2.0f - 1.0f, ny = 1.0f - (py / g_res_h) * 2.0f;
                float d[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = fwd[k] + right[k] * nx * v.tan_half_fov * v.aspect + up[k] * ny * v.tan_half_fov;
                const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int k = 0; k < 3; ++k) d[k] /= dl;
                const engine::Renderer::AimedDig r = g_renderer.dig_aimed(eye, d, kDigReach, kDigRadii[i]);
                // What the ray sees now: a dig must have moved the surface
                // back along it, by about the radius.
                float after[3], dist_after = -1.0f;
                g_renderer.aim_terrain(eye, d, kDigReach, after, dist_after);
                if (df) {
                    if (!r.hit) std::fprintf(df, "r %.2f m: pixel (%.0f, %.0f) no ground within %.0f m\n",
                                             kDigRadii[i], px, py, kDigReach);
                    else std::fprintf(df, "r %.2f m: pixel (%.0f, %.0f) hit at %.2f m (%.2f, %.2f, %.2f), "
                                          "centre (%.2f, %.2f, %.2f), %zu voxels, %.2f ms; "
                                          "the ray now meets ground at %.2f m (+%.2f)\n",
                                      kDigRadii[i], px, py, r.dist, r.hit_pos[0], r.hit_pos[1], r.hit_pos[2],
                                      r.centre[0], r.centre[1], r.centre[2], r.voxels, r.ms,
                                      dist_after, dist_after - r.dist);
                }
            }
            if (df) std::fclose(df);
        }

        // --dig-tunnel: a shaft 6 m down at the camera's x, z (2.5 m radius),
        // then a tunnel 22 m along the camera's heading from its floor (1.4 m
        // radius), and the camera moved into the shaft, at eye height off its
        // floor, looking down the tunnel. The scene for light under the
        // ground: sky down the shaft, dark along the tunnel. With --tunnel-up
        // it looks up the shaft instead.
        //
        // --tunnel-len <m> and --tunnel-r <m> its length (22) and radius
        // (1.4); --tunnel-ramp a ramp in from the surface instead of the
        // shaft (walking in from outside: the mouth's light is the open
        // horizon, not a well); --tunnel-inward looks further in, not back;
        // --tunnel-depth <m> how far under the ground its floor is (6).
        if (std::string(cmdline ? cmdline : "").find("--dig-tunnel") != std::string::npos) {
            const std::string tcl = cmdline;
            auto tnum = [&](const char* name, float def) {
                const size_t a = tcl.find(name);
                return a == std::string::npos ? def : float(std::atof(tcl.c_str() + a + std::strlen(name)));
            };
            const float len = tnum("--tunnel-len ", 22.0f), rad = tnum("--tunnel-r ", 1.4f);
            const bool ramp = tcl.find("--tunnel-ramp") != std::string::npos;
            const float x0 = g_camera.x, z0 = g_camera.z;
            const float h0 = g_renderer.world().height(x0, z0);
            const float fx = std::sin(g_camera.yaw), fz = std::cos(g_camera.yaw);
            const float floor_y = h0 - tnum("--tunnel-depth ", 6.0f);
            g_tunnel.dug = true;
            g_tunnel.x0 = x0; g_tunnel.z0 = z0; g_tunnel.fx = fx; g_tunnel.fz = fz;
            g_tunnel.floor_y = floor_y; g_tunnel.h0 = h0;
            if (ramp) {
                // 20 m down from the surface, its floor meeting the tunnel's.
                const float slope = (h0 - floor_y) / 20.0f;
                for (int k = -24; k <= 0; ++k)
                    g_renderer.dig(x0 + fx * float(k), floor_y + rad * 0.65f - slope * float(k), z0 + fz * float(k),
                                   rad, -4.0f, 1);
            } else {
                for (int k = 0; k <= 4; ++k) g_renderer.dig(x0, h0 - 1.5f * float(k), z0, 2.5f, -4.0f, 1);
            }
            for (int k = 0; k <= int(len); ++k)
                g_renderer.dig(x0 + fx * float(k), floor_y + rad * 0.65f, z0 + fz * float(k), rad, -4.0f, 1);
            g_camera.x = x0 - fx * 1.0f;
            g_camera.z = z0 - fz * 1.0f;
            g_camera.y = floor_y + 1.6f;
            g_camera.pitch = std::string(cmdline).find("--tunnel-up") != std::string::npos ? 1.2f : -0.05f;
            // --tunnel-in <m>: stand that far down the tunnel, looking back
            // at its mouth and the lit shaft.
            const size_t ti = std::string(cmdline).find("--tunnel-in ");
            if (ti != std::string::npos) {
                const float m = float(std::atof(cmdline + ti + 12));
                g_camera.x = x0 + fx * m;
                g_camera.z = z0 + fz * m;
                g_camera.y = floor_y + 1.4f;
                if (tcl.find("--tunnel-inward") == std::string::npos) g_camera.yaw += 3.14159265f;
                g_camera.pitch = 0.0f;
            }
            fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);
            if (FILE* tf = std::fopen((shot_path + ".tunnel.txt").c_str(), "w")) {
                std::fprintf(tf, "%s at (%.1f, %.1f, %.1f), 6 m deep; tunnel heading (%.2f, %.2f), %.0f m, radius %.1f m; "
                                 "camera (%.2f, %.2f, %.2f) yaw %.3f pitch %.3f\n",
                             ramp ? "ramp" : "shaft", x0, h0, z0, fx, fz, len, rad, g_camera.x, g_camera.y, g_camera.z,
                             g_camera.yaw, g_camera.pitch);
                std::fclose(tf);
            }
        }

        // --dig-burst <n>: a player hammering one spot with small digs --
        // n aimed digs at radii 0.5, 0.75, 1 and 1.5 m in turn, scattered
        // over +-90 px round 60% down the middle of the view by a fixed
        // sequence, so a still of what repeated small edits leave is
        // repeatable. Pair with --mesh-debug 1 to see the skirt curtain.
        {
            const std::string cl = cmdline ? cmdline : "";
            const size_t at = cl.find("--dig-burst ");
            if (at != std::string::npos) {
                const int n = std::atoi(cl.c_str() + at + 12);
                float fwd[3], right[3], up[3];
                g_camera.basis(fwd, right, up);
                const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
                const float radii[4] = {0.5f, 0.75f, 1.0f, 1.5f};
                uint32_t h = 0x2545F491u;
                int hits = 0;
                for (int i = 0; i < n; ++i) {
                    h = h * 1664525u + 1013904223u;
                    const float jx = (float(h >> 8) / 16777216.0f - 0.5f) * 180.0f;
                    h = h * 1664525u + 1013904223u;
                    const float jy = (float(h >> 8) / 16777216.0f - 0.5f) * 180.0f;
                    const float px = 0.5f * g_res_w + jx, py = 0.6f * g_res_h + jy;
                    const float nx = (px / g_res_w) * 2.0f - 1.0f, ny = 1.0f - (py / g_res_h) * 2.0f;
                    float d[3];
                    for (int k = 0; k < 3; ++k)
                        d[k] = fwd[k] + right[k] * nx * v.tan_half_fov * v.aspect + up[k] * ny * v.tan_half_fov;
                    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    for (int k = 0; k < 3; ++k) d[k] /= dl;
                    if (g_renderer.dig_aimed(eye, d, kDigReach, radii[i & 3]).dug) ++hits;
                }
                if (FILE* bf = std::fopen((shot_path + ".dig.txt").c_str(), "a")) {
                    std::fprintf(bf, "burst: %d of %d small digs landed, %zu edited voxels in all\n", hits, n,
                                 g_renderer.edits().edited_voxels());
                    std::fclose(bf);
                }
            }
        }

        bool ok = g_renderer.capture(v, shot_path.c_str(), error);
        // --dig-play <n>: after the still has settled, n frames of play (a
        // simulated 1/60 s each) with a small dig at the view's middle every
        // sixth, each frame timed on the wall clock with its dig: what digging
        // costs while the world runs -- the sky-access flood forgetting and
        // searching again (sky_access.h). <shot>.digplay.txt.
        {
            const std::string cl = cmdline ? cmdline : "";
            const size_t at = cl.find("--dig-play ");
            if (ok && at != std::string::npos) {
                const int n = std::atoi(cl.c_str() + at + 11);
                float fwd[3], right[3], up[3];
                g_camera.basis(fwd, right, up);
                const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
                const engine::SkyAccess& sa = g_renderer.near_field().sky_access();
                const unsigned long long s0 = sa.searches(), c0 = sa.cells_searched(), r0 = sa.cells_reset();
                const double m0 = sa.ms();
                std::vector<double> ms;
                uint32_t h = 0x9E3779B9u;
                int hits = 0;
                float t = 0.0f;
                LARGE_INTEGER qf;
                QueryPerformanceFrequency(&qf);
                for (int i = 0; i < n; ++i) {
                    LARGE_INTEGER q0, q1;
                    QueryPerformanceCounter(&q0);
                    if (i % 6 == 0) {
                        h = h * 1664525u + 1013904223u;
                        const float jx = (float(h >> 8) / 16777216.0f - 0.5f) * 120.0f;
                        h = h * 1664525u + 1013904223u;
                        const float jy = (float(h >> 8) / 16777216.0f - 0.5f) * 120.0f;
                        const float nx = jx / (0.5f * g_res_w), ny = -jy / (0.5f * g_res_h);
                        float d[3];
                        for (int k = 0; k < 3; ++k)
                            d[k] = fwd[k] + right[k] * nx * v.tan_half_fov * v.aspect + up[k] * ny * v.tan_half_fov;
                        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        for (int k = 0; k < 3; ++k) d[k] /= dl;
                        if (g_renderer.dig_aimed(eye, d, kDigReach, 1.0f).dug) ++hits;
                    }
                    t += 1.0f / 60.0f;
                    fill_view(g_camera, g_res_w, g_res_h, t, v);
                    g_renderer.step_offscreen(v);
                    QueryPerformanceCounter(&q1);
                    ms.push_back(double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(qf.QuadPart));
                }
                g_renderer.flush();
                std::vector<double> sorted = ms;
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (double x : ms) sum += x;
                if (FILE* df = std::fopen((shot_path + ".digplay.txt").c_str(), "w")) {
                    std::fprintf(df, "dig play: %d frames, %d of %d digs landed\n", n, hits, (n + 5) / 6);
                    if (!sorted.empty())
                        std::fprintf(df, "frame (wall clock, dig included): mean %.2f ms, 99th %.2f ms, worst %.2f ms\n",
                                     sum / double(ms.size()), sorted[(sorted.size() * 99) / 100], sorted.back());
                    std::fprintf(df, "sky access over it: %llu searches, %llu cells searched, %llu cells forgotten, %.0f ms "
                                     "on its thread; %zu requests still queued at the end\n",
                                 sa.searches() - s0, sa.cells_searched() - c0, sa.cells_reset() - r0, sa.ms() - m0,
                                 sa.queued());
                    std::fclose(df);
                }
            }
        }
        // --adapt-test (with --dig-tunnel): the eye's adaptation, timed. From
        // 10 m outside the tunnel's entrance in daylight (3 s), down it and
        // 20 m in at walking pace, holding there for 75 s, then back out --
        // every frame a simulated 1/30 s, so the curve does not depend on how
        // fast this renders. build/adapt_test.txt: time, where, the meter's
        // target and the adapted EV100, every 0.1 s.
        if (ok && g_tunnel.dug && std::string(cmdline).find("--adapt-test") != std::string::npos) {
            const float step = 1.0f / 30.0f;
            g_renderer.set_post_dt_override(step);
            FILE* af = std::fopen("build/adapt_test.txt", "w");
            if (af) std::fprintf(af, "t_s  phase  metres_in  target_EV100  adapted_EV100  light_presence  p10_nits  p90_nits\n");
            const TestTunnel& tt = g_tunnel;
            // Where the camera is, metres along the tunnel's axis from its
            // entrance (negative: back up the ramp and out on the surface).
            auto place = [&](float m) {
                g_camera.x = tt.x0 + tt.fx * m;
                g_camera.z = tt.z0 + tt.fz * m;
                const float ground = g_renderer.world().height(g_camera.x, g_camera.z);
                const float in_tunnel = tt.floor_y + 1.4f;
                // On the ramp the floor rises 1/20 of the depth a metre.
                const float ramp = tt.floor_y + 1.4f + (tt.h0 - tt.floor_y) * (-m / 20.0f);
                g_camera.y = m >= 0.0f ? in_tunnel : (m > -20.0f ? ramp : ground + 1.6f);
                g_camera.yaw = std::atan2(tt.fx, tt.fz);
                g_camera.pitch = -0.05f;
            };
            float t = 0.0f;
            int frame = 0;
            auto run = [&](const char* phase, float seconds, float from, float to) {
                const int n = int(seconds / step + 0.5f);
                for (int i = 0; i < n; ++i) {
                    const float m = from + (to - from) * (float(i + 1) / float(n));
                    place(m);
                    t += step;
                    fill_view(g_camera, g_res_w, g_res_h, t, v);
                    g_renderer.step_offscreen(v);
                    const float* ex = g_renderer.live_exposure();
                    if (af)
                        std::fprintf(af, "%6.2f  %-5s  %6.1f  %6.2f  %6.2f  %4.2f  %8.4f %8.4f\n", t, phase, m,
                                     ex[3] + 3.0f, ex[2], g_renderer.light_presence(), ex[4], ex[5]);
                    ++frame;
                }
            };
            run("out", 3.0f, -30.0f, -30.0f);     // on the surface, adapted to the day
            run("in", 6.0f, -30.0f, 20.0f);       // down the ramp and in, ~8 m/s
            run("hold", 75.0f, 20.0f, 20.0f);     // standing 20 m in
            run("back", 6.0f, 20.0f, -30.0f);     // out again
            run("out", 4.0f, -30.0f, -30.0f);
            if (af) std::fclose(af);
            g_renderer.set_post_dt_override(-1.0f);
        }

        // --mushroom-test: the cluster of glowing mushrooms nearest the
        // still's camera, seen from a metre over it (build/mushroom_before.bmp),
        // then picked one by one by aiming the camera's ray at each cap as E
        // would, and seen again (build/mushroom_after.bmp). The light that
        // cluster gave goes with them. build/mushroom_test.txt says what.
        {
            const std::string cl = cmdline ? cmdline : "";
            if (ok && cl.find("--mushroom-test") != std::string::npos) {
                engine::GlowMushrooms& gm = g_renderer.mushrooms();
                FILE* mf = std::fopen("build/mushroom_test.txt", "w");
                const float p[3] = {g_camera.x, g_camera.y, g_camera.z};
                float c[3];
                if (!gm.nearest(p, c)) {
                    if (mf) std::fprintf(mf, "no cluster populated near (%.1f, %.1f, %.1f)\n", p[0], p[1], p[2]);
                } else {
                    g_camera.x = c[0] - 0.9f; g_camera.y = c[1] + 1.0f; g_camera.z = c[2];
                    g_camera.yaw = 1.5708f; g_camera.pitch = -0.85f;   // +x, towards it
                    fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);
                    ok = g_renderer.capture(v, "build/mushroom_before.bmp", error);
                    std::vector<engine::LocalLight> lit;
                    gm.lights(v.cam_pos, lit);
                    std::vector<float> caps;
                    gm.targets_near(c, 1.5f, caps);
                    const size_t before = gm.picked();
                    const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
                    // A ray at nothing first: straight up, into the roof.
                    const float upd[3] = {0.0f, 1.0f, 0.0f};
                    const bool miss = g_renderer.pick_mushroom(eye, upd, eye, kPickReach);
                    int aimed = 0, got = 0;
                    for (size_t k = 0; k + 2 < caps.size(); k += 3) {
                        float d[3] = {caps[k] - eye[0], caps[k + 1] - eye[1], caps[k + 2] - eye[2]};
                        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        for (int q = 0; q < 3; ++q) d[q] /= l;
                        ++aimed;
                        if (g_renderer.pick_mushroom(eye, d, eye, kPickReach)) ++got;
                    }
                    std::vector<engine::LocalLight> lit_after;
                    gm.lights(v.cam_pos, lit_after);
                    float lm0 = 0.0f, lm1 = 0.0f;
                    for (const engine::LocalLight& l : lit) lm0 += l.lumens;
                    for (const engine::LocalLight& l : lit_after) lm1 += l.lumens;
                    if (ok) ok = g_renderer.capture(v, "build/mushroom_after.bmp", error);
                    if (mf)
                        std::fprintf(mf, "cluster at (%.1f, %.1f, %.1f), camera (%.2f, %.2f, %.2f)\n"
                                         "a ray at the roof picked %s; aimed at %d caps within 1.5 m, picked %d "
                                         "(total picked %zu -> %zu)\n"
                                         "cluster lights within reach: %zu (%.1f lm) before, %zu (%.1f lm) after\n",
                                     c[0], c[1], c[2], eye[0], eye[1], eye[2], miss ? "ONE (wrong)" : "nothing",
                                     aimed, got, before, gm.picked(), lit.size(), lm0, lit_after.size(), lm1);
                }
                if (mf) std::fclose(mf);
            }
        }

        // --cave-walk <k>: the k-th placed cave nearest the still's camera,
        // walked into as play would -- never settled. From 50 m outside the
        // mouth, along the capsules' axis to the chamber's middle at 3 m/s,
        // every frame a simulated 1/30 s. build/cave_walk.txt logs where,
        // the meter's 10th/90th percentiles in nits and the adapted EV100
        // every frame; build/cavewalk_NN.bmp is a raw frame every 1.5 s.
        {
            const std::string cl = cmdline ? cmdline : "";
            const size_t cw = cl.find("--cave-walk ");
            const worldgen::SeedWorld& sw = g_renderer.world().world();
            if (ok && cw != std::string::npos && !sw.caves.empty()) {
                const int want = std::atoi(cl.c_str() + cw + 12);
                std::vector<std::pair<float, size_t> > order;
                for (size_t i = 0; i < sw.caves.size(); ++i) {
                    const float dx = sw.caves[i].x - g_camera.x, dz = sw.caves[i].z - g_camera.z;
                    order.push_back(std::make_pair(dx * dx + dz * dz, i));
                }
                std::sort(order.begin(), order.end());
                const worldgen::SeedWorld::Cave& c = sw.caves[order[size_t(want) % order.size()].second];
                // The path: outside, the mouth at eye height, then every
                // tunnel capsule's far end (the last three capsules are the
                // chamber's own), then the chamber's middle.
                std::vector<float> px, py, pz;
                auto add = [&](float x, float y, float z) { px.push_back(x); py.push_back(y); pz.push_back(z); };
                const float ox = c.x - c.hx * 50.0f, oz = c.z - c.hz * 50.0f;
                add(ox, g_renderer.world().height(ox, oz) + 1.6f, oz);
                add(c.x, c.y + 1.6f, c.z);
                for (int k = 1; k < c.seg_count - 3; ++k) {
                    const worldgen::SeedWorld::EntranceSeg& e = sw.entrances[size_t(c.seg_begin + k)];
                    add(e.bx, e.by, e.bz);
                }
                add(c.cx, c.floor_y + 1.6f, c.cz);
                const float step = 1.0f / 30.0f, speed = 3.0f;
                g_renderer.set_post_dt_override(step);
                g_renderer.set_capture_raw(true);
                FILE* wf = std::fopen("build/cave_walk.txt", "w");
                if (wf) std::fprintf(wf, "cave %s, mouth (%.1f, %.1f, %.1f), floor %.1f m down, %zu path points\n"
                                         "t_s  metres  x y z  target_EV100  adapted_EV100  p10_nits  p90_nits  frame\n",
                                     g_renderer.world().table()[size_t(c.biome)].name.c_str(), c.x, c.y, c.z,
                                     c.y - c.floor_y, px.size());
                float t = 0.0f, walked = 0.0f, next_shot = 0.0f;
                int shot = 0;
                for (size_t k = 0; k + 1 < px.size() && ok; ++k) {
                    const float dx = px[k + 1] - px[k], dy = py[k + 1] - py[k], dz = pz[k + 1] - pz[k];
                    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const int n = (std::max)(1, int(len / (speed * step) + 0.5f));
                    const float hl = std::sqrt(dx * dx + dz * dz);
                    for (int i = 0; i < n && ok; ++i) {
                        const float u = float(i + 1) / float(n);
                        g_camera.x = px[k] + dx * u; g_camera.y = py[k] + dy * u; g_camera.z = pz[k] + dz * u;
                        if (hl > 0.1f) g_camera.yaw = std::atan2(dx, dz);
                        g_camera.pitch = hl > 0.1f ? 0.6f * std::atan2(dy, hl) : 0.0f;
                        t += step;
                        walked += len / float(n);
                        fill_view(g_camera, g_res_w, g_res_h, t, v);
                        int saved = -1;
                        if (t >= next_shot) {
                            char p[64];
                            std::snprintf(p, sizeof p, "build/cavewalk_%02d.bmp", shot);
                            ok = g_renderer.capture(v, p, error);
                            saved = shot++;
                            next_shot += 1.5f;
                        } else {
                            g_renderer.step_offscreen(v);
                        }
                        const float* ex = g_renderer.live_exposure();
                        if (wf)
                            std::fprintf(wf, "%6.2f %6.1f  %.1f %.1f %.1f  %6.2f %6.2f  %9.5f %9.5f  %d\n", t, walked,
                                         g_camera.x, g_camera.y, g_camera.z, ex[3] + 3.0f, ex[2], ex[4], ex[5], saved);
                    }
                }
                if (wf) std::fclose(wf);
                g_renderer.set_capture_raw(false);
                g_renderer.set_post_dt_override(-1.0f);
            }
        }

        // --clock-jump <hours> [seconds] [ramp]: how fast the light under the
        // ground follows the clock. After the still, 2 s at its hour, then the
        // clock jumps to <hours> -- or with [ramp] seconds, runs there
        // evenly over them, as H does -- and holds for [seconds] (10) after
        // it. Every frame a
        // simulated 1/30 s, the camera still. build/time_jump.txt: time, hour,
        // the meter's target and adapted EV100, its 10th and 90th percentiles
        // in nits (exposure-independent), every frame.
        {
            const std::string cl = cmdline ? cmdline : "";
            const size_t tj = cl.find("--clock-jump ");
            if (ok && tj != std::string::npos) {
                float to = 23.0f, secs = 10.0f, ramp = 0.0f;
                std::sscanf(cl.c_str() + tj + 13, "%f %f %f", &to, &secs, &ramp);
                if (secs <= 0.0f || secs > 300.0f) secs = 10.0f;
                const float step = 1.0f / 30.0f;
                g_renderer.set_post_dt_override(step);
                FILE* jf = std::fopen("build/time_jump.txt", "w");
                if (jf) std::fprintf(jf, "t_s  hour  target_EV100  adapted_EV100  p10_nits  p90_nits\n");
                float t = 0.0f;
                auto run = [&](float seconds) {
                    const int n = int(seconds / step + 0.5f);
                    for (int i = 0; i < n; ++i) {
                        t += step;
                        fill_view(g_camera, g_res_w, g_res_h, t, v);
                        g_renderer.step_offscreen(v);
                        const float* ex = g_renderer.live_exposure();
                        if (jf)
                            std::fprintf(jf, "%6.3f  %5.2f  %6.2f  %6.2f  %9.5f %9.5f\n", t, g_renderer.time_of_day(),
                                         ex[3] + 3.0f, ex[2], ex[4], ex[5]);
                    }
                };
                run(2.0f);
                if (ramp > 0.0f) {
                    // Forward through midnight if need be, as the clock runs.
                    const float from = g_renderer.time_of_day();
                    const float span = std::fmod(to - from + 24.0f, 24.0f);
                    const int n = int(ramp / step + 0.5f);
                    for (int i = 0; i < n; ++i) {
                        g_renderer.set_time_of_day(from + span * float(i + 1) / float(n));
                        run(step);
                    }
                }
                g_renderer.set_time_of_day(to);
                run(secs);
                if (jf) std::fclose(jf);
                g_renderer.set_post_dt_override(-1.0f);
            }
        }

        // --write-snapshot: the still's scene as an F8 snapshot (the window's
        // key, headless), its folder named in build/last_snapshot.txt.
        if (ok && std::string(cmdline).find("--write-snapshot") != std::string::npos) {
            const std::string dir = write_snapshot(v);
            if (FILE* lf = std::fopen("build/last_snapshot.txt", "w")) {
                std::fprintf(lf, "%s\n", dir.c_str());
                std::fclose(lf);
            }
        }
        {
            // --shake <frames>: a still camera through a session's worth of
            // frames with the clock running, then eight consecutive raw
            // frames, build/shake_0..7.bmp, to measure whether the image
            // moves while the camera does not.
            const std::string cl = cmdline ? cmdline : "";
            const size_t sk = cl.find("--shake ");
            if (ok && sk != std::string::npos) {
                const int n = std::atoi(cl.c_str() + sk + 8);
                float t = 0.0f;
                // Per frame: what the streamer did with the camera still.
                FILE* sl = std::fopen("build/shake.txt", "w");
                if (sl) std::fprintf(sl, "frame meshed pending held fading_in resident free\n");
                auto log_frame = [&](int i) {
                    if (!sl) return;
                    const engine::FrameStats s = g_renderer.stats();
                    const engine::Streamer& sm = g_renderer.streamer();
                    std::fprintf(sl, "%d %u %zu %zu %zu %zu %zu\n", i, s.meshed_this_frame, s.pending,
                                 sm.held_count(), sm.fading_in_count(), s.resident, s.free_slots);
                };
                for (int i = 0; i < n; ++i) {
                    t += 1.0f / 60.0f;
                    fill_view(g_camera, g_res_w, g_res_h, t, v);
                    g_renderer.step_offscreen(v);
                    log_frame(i);
                }
                g_renderer.set_capture_raw(true);
                for (int i = 0; i < 8 && ok; ++i) {
                    t += 1.0f / 60.0f;
                    fill_view(g_camera, g_res_w, g_res_h, t, v);
                    char p[64];
                    std::snprintf(p, sizeof p, "build/shake_%d.bmp", i);
                    ok = g_renderer.capture(v, p, error);
                    log_frame(n + i);
                }
                g_renderer.set_capture_raw(false);
                if (sl) std::fclose(sl);
            }
        }
        {
            // --gpu-profile [frames]: where a still's GPU time goes, pass by
            // pass, and how much the tree pass overdraws. Writes
            // <shot>.gpu.txt and <shot>.overdraw.bmp. The camera stays still
            // and the clock runs, as in --shake.
            const std::string cl = cmdline ? cmdline : "";
            const size_t gp = cl.find("--gpu-profile");
            if (ok && gp != std::string::npos) {
                int n = 120;
                {
                    const size_t q = cl.find_first_not_of(" \t", gp + 13);
                    if (q != std::string::npos && cl[q] != '-') n = std::atoi(cl.c_str() + q);
                    if (n < 8) n = 120;
                }
                engine::GpuProfiler& prof = g_renderer.profiler();
                float t = 0.0f;
                auto run = [&](int frames) {
                    for (int i = 0; i < frames; ++i) {
                        t += 1.0f / 60.0f;
                        fill_view(g_camera, g_res_w, g_res_h, t, v);
                        g_renderer.step_offscreen(v);
                    }
                };
                run(30);   // settle the streamers, the history, the exposure
                // The frame as it runs: first and last marks only.
                g_renderer.set_profiling(true);
                prof.set_detail(false);
                prof.reset(2);
                LARGE_INTEGER w0, w1, wf;
                QueryPerformanceFrequency(&wf);
                QueryPerformanceCounter(&w0);
                run(n);
                g_renderer.flush();
                QueryPerformanceCounter(&w1);
                const double wall_ms = double(w1.QuadPart - w0.QuadPart) * 1000.0 / double(wf.QuadPart) / n;
                prof.harvest_all();
                const unsigned whole_frames = prof.frames();
                const double whole_ms = whole_frames ? prof.total_ms() / whole_frames : 0.0;
                // Pass by pass, serialised.
                prof.set_detail(true);
                prof.reset(2);
                const unsigned long long nf_probes0 = g_renderer.near_field().probes_traced();
                run(n);
                const double nf_probes_frame = double(g_renderer.near_field().probes_traced() - nf_probes0) / n;
                g_renderer.flush();
                prof.harvest_all();
                g_renderer.set_profiling(false);
                const unsigned pf = prof.frames() ? prof.frames() : 1u;
                const double W = g_res_w, H = g_res_h;
                const std::string gpath = shot_path + ".gpu.txt";
                FILE* gf = std::fopen(gpath.c_str(), "w");
                if (gf) {
                    std::fprintf(gf, "gpu profile: %.0f x %.0f, camera (%.1f, %.1f, %.1f) yaw %.2f pitch %.2f, %d frames each\n",
                                 W, H, g_camera.x, g_camera.y, g_camera.z, g_camera.yaw, g_camera.pitch, n);
                    std::fprintf(gf, "frame, as it runs (first to last mark, passes overlapping): %.3f ms over %u frames\n",
                                 whole_ms, whole_frames);
                    std::fprintf(gf, "frame, wall clock (CPU and GPU, a still's frame waits for the GPU): %.3f ms\n",
                                 wall_ms);
                    std::fprintf(gf, "frame, every pass serialised by a barrier:                  %.3f ms over %u frames\n\n",
                                 prof.total_ms() / pf, prof.frames());
                    std::fprintf(gf, "near field: %.0f probes listed by the CPU a frame%s; live, traced every frame: %u fine, %u coarse\n\n",
                                 nf_probes_frame, std::getenv("BIOMES_NF_PLAY") ? " (BIOMES_NF_PLAY: play's schedule)" : "",
                                 g_renderer.near_field().live_fine(), g_renderer.near_field().live_coarse());
                    {
                        const engine::SkyAccess& sa = g_renderer.near_field().sky_access();
                        std::fprintf(gf, "sky access: %zu bricks, %llu searches, %llu cells searched (%llu answered "
                                         "no sky), %llu evaluated, %.0f ms of CPU in all; the camera %d m from the "
                                         "sky by path (255 none), darkness %.3f; %d blocks not answered yet\n\n",
                                     sa.bricks(), sa.searches(), sa.cells_searched(), sa.cells_no_sky(),
                                     sa.cells_evaluated(), sa.ms(), g_renderer.near_field().camera_sky_dist(),
                                     g_renderer.near_field().camera_dark(), g_renderer.near_field().sky_unlabelled());
                        std::fprintf(gf, "sky access, camera cell: state %d, its column's sky floor %d\n\n",
                                     sa.dbg_cam_st_.load(), sa.dbg_cam_floor_.load());
                    }
                    std::fprintf(gf, "%-58s %9s %9s %7s\n", "pass (named by the mark that ends it)", "ms/frame", "max ms", "frames");
                    double sum = 0.0;
                    const std::vector<std::pair<std::string, engine::GpuProfiler::Segment> > segs = prof.ordered_segments();
                    for (size_t i = 0; i < segs.size(); ++i) {
                        const double ms = segs[i].second.ms / pf;
                        sum += ms;
                        std::fprintf(gf, "%-58s %9.3f %9.3f %7u\n", segs[i].first.c_str(), ms, segs[i].second.ms_max,
                                     segs[i].second.frames);
                    }
                    // Groups, for the headline.
                    double g_trees = 0.0, g_shadow_trees = 0.0, g_shadow_terrain = 0.0, g_foliage = 0.0, g_terrain = 0.0;
                    for (size_t i = 0; i < segs.size(); ++i) {
                        const std::string& nm = segs[i].first;
                        const double ms = segs[i].second.ms / pf;
                        if (nm.compare(0, 6, "trees ") == 0) g_trees += ms;
                        if (nm.compare(0, 8, "shadow c") == 0 && nm.find("trees") != std::string::npos) g_shadow_trees += ms;
                        if (nm.compare(0, 8, "shadow c") == 0 && nm.find("terrain") != std::string::npos) g_shadow_terrain += ms;
                        if (nm == "foliage") g_foliage += ms;
                        if (nm == "terrain") g_terrain += ms;
                    }
                    std::fprintf(gf, "\nsum of passes %.3f ms. Groups: trees (camera) %.3f ms; tree shadow casters %.3f ms; "
                                     "terrain + rocks shadow casters %.3f ms; foliage %.3f ms; terrain %.3f ms\n",
                                 sum, g_trees, g_shadow_trees, g_shadow_terrain, g_foliage, g_terrain);
                    std::fprintf(gf, "\npipeline statistics, per frame it ran (PS invocations: the overdraw PAID, after early depth)\n");
                    std::fprintf(gf, "%-28s %12s %12s %12s %14s %10s %7s\n", "draws", "VS inv", "prims in", "prims raster",
                                 "PS invocations", "per pixel", "frames");
                    const std::vector<std::pair<std::string, engine::GpuProfiler::Stat> > sts = prof.ordered_stats();
                    for (size_t i = 0; i < sts.size(); ++i) {
                        const engine::GpuProfiler::Stat& st2 = sts[i].second;
                        const double f = st2.frames ? double(st2.frames) : 1.0;
                        std::fprintf(gf, "%-28s %12.0f %12.0f %12.0f %14.0f %10.3f %7u\n", sts[i].first.c_str(), st2.vs / f,
                                     st2.ia_prims / f, st2.c_prims / f, st2.ps / f, st2.ps / f / (W * H), st2.frames);
                    }
                }
                // One frame through the counting shaders.
                std::vector<uint32_t> od;
                std::string oerr;
                if (g_renderer.tree_overdraw_begin(oerr)) {
                    run(1);
                    g_renderer.tree_overdraw_read(od, oerr);
                }
                const size_t px = size_t(W) * size_t(H);
                if (gf && od.size() == px * 2) {
                    double inv = 0, surv = 0, leaf = 0, imp = 0;
                    size_t touched = 0, covered = 0;
                    unsigned max_inv = 0, max_surv = 0;
                    size_t hist[7] = {0, 0, 0, 0, 0, 0, 0};   // survived: 1, 2, 3-4, 5-8, 9-16, 17-32, 33+
                    std::vector<unsigned char> bgr(px * 3, 0);
                    for (size_t i = 0; i < px; ++i) {
                        const unsigned a = od[i * 2] & 0xFFFFu, b = od[i * 2] >> 16;
                        const unsigned c = od[i * 2 + 1] & 0xFFFFu, d = od[i * 2 + 1] >> 16;
                        inv += a; surv += b; leaf += c; imp += d;
                        if (a) ++touched;
                        if (b) ++covered;
                        max_inv = (std::max)(max_inv, a);
                        max_surv = (std::max)(max_surv, b);
                        if (b) {
                            const int k = b <= 1 ? 0 : b <= 2 ? 1 : b <= 4 ? 2 : b <= 8 ? 3 : b <= 16 ? 4 : b <= 32 ? 5 : 6;
                            ++hist[k];
                        }
                        // Heat: the rasterised layers, black 0, blue 1, green 4,
                        // yellow 8, red 16, white 32+.
                        const float l = a == 0 ? 0.0f : std::log2(float(a)) / 5.0f;
                        float r = 0, g = 0, bl = 0;
                        if (a) {
                            if (l < 0.4f) { bl = 1.0f - l / 0.4f; g = l / 0.4f; }
                            else if (l < 0.6f) { g = 1.0f; r = (l - 0.4f) / 0.2f; }
                            else if (l < 0.8f) { r = 1.0f; g = 1.0f - (l - 0.6f) / 0.2f; }
                            else { r = 1.0f; g = bl = (std::min)(1.0f, (l - 0.8f) / 0.2f); }
                            r = 0.15f + 0.85f * r; g = 0.15f + 0.85f * g; bl = 0.15f + 0.85f * bl;
                        }
                        const size_t x = i % size_t(W), y = i / size_t(W);
                        const size_t o = ((size_t(H) - 1 - y) * size_t(W) + x) * 3;   // BMP rows run bottom-up
                        bgr[o + 0] = (unsigned char)(bl * 255.0f);
                        bgr[o + 1] = (unsigned char)(g * 255.0f);
                        bgr[o + 2] = (unsigned char)(r * 255.0f);
                    }
                    const double cov = covered ? double(covered) : 1.0;
                    std::fprintf(gf, "\ntree overdraw, camera pass, one frame (counting shaders: late depth, so every "
                                     "rasterised fragment runs)\n");
                    std::fprintf(gf, "  fragments rasterised %.0f (%.2f per screen pixel, %.2f per touched pixel, max %u); "
                                     "touched %.1f%% of the screen\n",
                                 inv, inv / double(px), touched ? inv / double(touched) : 0.0, max_inv,
                                 100.0 * double(touched) / double(px));
                    std::fprintf(gf, "  survived coverage + dither %.0f (%.1f%% of rasterised): leaf cards %.0f, impostors %.0f, "
                                     "bark %.0f\n", surv, inv > 0 ? 100.0 * surv / inv : 0.0, leaf, imp, surv - leaf - imp);
                    std::fprintf(gf, "  covered pixels %.1f%% of the screen; survivors per covered pixel %.2f (max %u) -- "
                                     "a depth prepass would shade ~1\n",
                                 100.0 * double(covered) / double(px), surv / cov, max_surv);
                    std::fprintf(gf, "  survivors per covered pixel: 1: %.1f%%, 2: %.1f%%, 3-4: %.1f%%, 5-8: %.1f%%, 9-16: %.1f%%, "
                                     "17-32: %.1f%%, 33+: %.1f%%\n",
                                 100.0 * hist[0] / cov, 100.0 * hist[1] / cov, 100.0 * hist[2] / cov, 100.0 * hist[3] / cov,
                                 100.0 * hist[4] / cov, 100.0 * hist[5] / cov, 100.0 * hist[6] / cov);
                    const std::string hp = shot_path + ".overdraw.bmp";
                    FILE* bf = std::fopen(hp.c_str(), "wb");
                    if (bf) {
                        const int w = int(W), h = int(H);
                        const int row = (w * 3 + 3) & ~3;
                        const unsigned size = 54u + unsigned(row) * unsigned(h);
                        unsigned char hd[54] = {'B', 'M'};
                        auto put32 = [&](int at, unsigned val) {
                            for (int k = 0; k < 4; ++k) hd[at + k] = (unsigned char)(val >> (8 * k));
                        };
                        put32(2, size); put32(10, 54); put32(14, 40); put32(18, unsigned(w)); put32(22, unsigned(h));
                        hd[26] = 1; hd[28] = 24;
                        std::fwrite(hd, 1, 54, bf);
                        const unsigned char pad[3] = {0, 0, 0};
                        for (int y = 0; y < h; ++y) {
                            std::fwrite(&bgr[size_t(y) * size_t(w) * 3], 1, size_t(w) * 3, bf);
                            std::fwrite(pad, 1, size_t(row - w * 3), bf);
                        }
                        std::fclose(bf);
                        std::fprintf(gf, "  heat map of rasterised layers: %s (black 0, blue 1, green 4, yellow 8, red 16, "
                                         "white 32+)\n", hp.c_str());
                    }
                } else if (gf) {
                    std::fprintf(gf, "\ntree overdraw: not measured (%s)\n", oerr.empty() ? "no trees" : oerr.c_str());
                }
                if (gf) std::fclose(gf);
            }
        }
        const engine::FrameStats st = g_renderer.stats();
        float mx = 0.0f, mn = 0.0f;
        unsigned checked = 0;
        g_renderer.check_against_field(mx, mn, checked);
        if (debug_layer) {
            g_renderer.drain_debug_messages("build/d3d12_validation.txt");
            g_renderer.dump_draw_args("build/draw_args.txt");
        }

        if (std::string(cmdline ? cmdline : "").find("--slot-report") != std::string::npos) {
            g_renderer.slot_report((shot_path + ".slots.txt").c_str());
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
                const float nx = ((px + 0.5f) / g_res_w) * 2.0f - 1.0f;
                const float ny = 1.0f - ((py + 0.5f) / g_res_h) * 2.0f;
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
                // What is actually resident around the hit, per LOD: the hit's
                // column and its four neighbours, at the hit's y-level and one
                // either side. G = holds geometry, e = resident but meshed
                // empty, . = not resident.
                for (int L = 0; L < engine::kLodCount; ++L) {
                    const float S = engine::lod_chunk_size(L);
                    const int cx = int(std::floor(hx / S)), cy = int(std::floor(hy / S));
                    const int cz = int(std::floor(hz / S));
                    const int off[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    bool any = false;
                    std::string row;
                    for (int c = 0; c < 5; ++c) {
                        row += c == 0 ? " here[" : (c == 1 ? " -x[" : c == 2 ? " +x[" : c == 3 ? " -z[" : " +z[");
                        for (int dy = -1; dy <= 1; ++dy) {
                            engine::ChunkKey k;
                            k.x = cx + off[c][0]; k.y = cy + dy; k.z = cz + off[c][1]; k.lod = L;
                            UINT sl = 0;
                            const bool res = g_renderer.streamer().slot_of(k, sl);
                            row += !res ? '.' : (sl == engine::kNoSlot ? 'e' : 'G');
                            if (res) any = true;
                        }
                        row += "]";
                    }
                    if (any) std::fprintf(pf, "    LOD %d chunk (%d,%d,%d), y-levels below/at/above:%s\n",
                                          L, cx, cy, cz, row.c_str());
                }

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

        if (g_ui.ready()) {
            g_renderer.flush();
            g_renderer.set_overlay(nullptr);
            g_ui.shutdown();
        }
        g_renderer.shutdown();
        g_physics.shutdown();

        const std::string sp = shot_path + ".txt";
        if (FILE* f = std::fopen(sp.c_str(), "w")) {
            std::fprintf(f, "resident chunks %zu\npending %zu\nfree slots %zu\n",
                         st.resident, st.pending, st.free_slots);
            std::fprintf(f, "vertices %u\nindices %u\ntriangles %u\nslots overflowed %u\n",
                         st.vertices, st.indices, st.indices / 3, st.overflowed);
            write_rock_report(f, g_camera.x, g_camera.z, false);
            write_mushroom_report(f, g_camera.x, g_camera.y, g_camera.z);
            {
                const engine::Renderer::CrackCount& cc = g_renderer.last_cracks();
                std::fprintf(f, "cracks below the horizon: %zu undrawn (far depth, scene targets clear); "
                                "far depth alone %zu; of %zu pixels below it\n",
                             cc.undrawn, cc.by_depth, cc.below_horizon);
                if (!cc.where.empty()) {
                    std::fprintf(f, "undrawn pixels (x y):");
                    for (size_t q = 0; q < cc.where.size(); ++q)
                        std::fprintf(f, " %d %d", cc.where[q].first, cc.where[q].second);
                    std::fprintf(f, "\n");
                }
            }
            {
                // The atmosphere (ADR-004 L2): the hour, the light, and the
                // GPU's sky table against the CPU's march of it.
                const engine::SkyFrame& sf = g_renderer.sky_frame();
                const float kl = 0.2126f * sf.key_lux[0] + 0.7152f * sf.key_lux[1] + 0.0722f * sf.key_lux[2];
                const float sm = 3.14159265f * (0.2126f * sf.sky_mean[0] + 0.7152f * sf.sky_mean[1] + 0.0722f * sf.sky_mean[2]);
                std::fprintf(f, "sky: %02d:%02d, sun %.1f deg up; key light the %s, %.4g lux; sky on level ground %.4g lux\n",
                             int(sf.hours), int((sf.hours - float(int(sf.hours))) * 60.0f), sf.sun_elevation_deg,
                             sf.key_is_moon ? "moon" : "sun", kl, sm);
                {
                    // The weather (weather.h) and the sea under the camera.
                    const engine::WeatherState& w = g_renderer.weather_state();
                    const WwSea& sea = g_renderer.sea();
                    double m0 = 0.0;
                    for (int i = 0; i < WW_WAVES; ++i) m0 += 0.5 * double(sea.amp[i]) * sea.amp[i];
                    const float sl = g_renderer.world().params().sea_level;
                    const float depth = sl - g_renderer.world().height(g_camera.x, g_camera.z);
                    std::fprintf(f, "weather: %s, storm %.2f, wind %.1f m/s towards (%.2f, %.2f), gusts %.2f; "
                                    "sea developed for %.1f m/s, swell Hs %.2f m towards (%.2f, %.2f); "
                                    "deep-water Hs %.2f m; camera (%.1f, %.1f, %.1f) over %.1f m of water, significant height there %.2f m\n",
                                 g_renderer.weather().automatic() ? "automatic" : "fixed", w.storm, w.wind_speed,
                                 w.wind_dir[0], w.wind_dir[1], w.gust, w.sea_wind, w.swell_hs, w.swell_dir[0],
                                 w.swell_dir[1], 4.0 * std::sqrt(m0), g_camera.x, g_camera.y, g_camera.z, depth,
                                 depth > 0.0f ? ww_significant_height_lod(sea, depth, 0.0f) : 0.0f);
                    // ADR-006 stage 2: which side of the water the still was
                    // taken from decides everything in it.
                    if (g_renderer.camera_underwater())
                        std::fprintf(f, "underwater: the eye is %.2f m under the surface (post.hlsl cs_underwater)\n",
                                     g_renderer.camera_water_depth());
                    else
                        std::fprintf(f, "underwater: no\n");
                    {
                        const engine::Strike& st = g_renderer.strike();
                        if (st.active)
                            std::fprintf(f, "  lightning: %s strike %.0f m away at (%.0f, %.0f), %.0f ms in, %.3g cd now, "
                                            "channel %.2f; thunder %.1f s after the flash\n",
                                         st.ground ? "ground" : "in-cloud", st.distance, st.ground_pt[0], st.ground_pt[2],
                                         st.age * 1000.0f, st.candela, st.channel, st.distance / 343.0f);
                        else
                            std::fprintf(f, "  lightning: none this frame\n");
                    }
                    {
                        // The exploration fog's visibility (haze.hlsli), from
                        // its own layers alone (the froxel mist's patches add
                        // to it near the ground): the 2% contrast distance,
                        // 3.912 / sigma, along a level line at a few heights.
                        const float sl = g_renderer.world().params().sea_level;
                        auto vis = [&](float y) {
                            const float s = g_renderer.height_fog_sigma(y);
                            return s > 0.0f ? 3.912f / s : 0.0f;
                        };
                        std::fprintf(f, "  height fog: valley %.2f; level visibility at the eye (%.0f m) %.0f m, at 20 m "
                                        "%.0f m, at 300 m %.0f m, at 600 m %.0f m\n",
                                     g_renderer.height_fog_valley(), g_camera.y - sl, vis(g_camera.y), vis(sl + 20.0f),
                                     vis(sl + 300.0f), vis(sl + 600.0f));
                    }
                    float wet_now = 0.0f, puddle_now = 0.0f;
                    g_renderer.wetness(wet_now, puddle_now);
                    std::fprintf(f, "  rain: %.2f (%s), cloud cover %.2f; ground wetness %.2f, puddles %.2f\n",
                                 g_renderer.rain_intensity(),
                                 !g_renderer.rain_on() ? "off" : "from the clouds unless --rain",
                                 g_renderer.cloud_layer().coverage(), wet_now, puddle_now);
                    {
                        const engine::OceanDetail& od = g_renderer.ocean();
                        std::fprintf(f, "  sea slope (mean square, both axes): Cox-Munk %.4f; trains %.4f, ripples %.4f "
                                        "(31 m tile %.4f, 7.7 m tile %.4f)%s\n",
                                     od_cox_munk(w.sea_wind > 1.0f ? w.sea_wind : 1.0f),
                                     od.gerstner_variance(), od.variance(0) + od.variance(1), od.variance(0), od.variance(1),
                                     g_renderer.wave_detail() ? "" : " -- ripples OFF");
                    }
                    // Along the view, flat: what the water ahead can carry.
                    const float fx = std::sin(g_camera.yaw), fz = std::cos(g_camera.yaw);
                    std::fprintf(f, "  ahead (depth m / Hs m):");
                    const float ahead[5] = {25.0f, 50.0f, 100.0f, 200.0f, 400.0f};
                    for (float a : ahead) {
                        const float px = g_camera.x + fx * a, pz = g_camera.z + fz * a;
                        const float d = sl - g_renderer.world().height(px, pz);
                        std::fprintf(f, " %.0f m: %.1f / %.2f;", a, d,
                                     d > 0.0f ? ww_significant_height_lod(sea, d, 0.0f) : 0.0f);
                    }
                    std::fprintf(f, "\n");
                }
                const engine::Renderer::PostTimes& pt = g_renderer.post_times();
                std::fprintf(f, "gpu: atmosphere %.3f ms, mist inject %.3f ms, mist integrate %.3f ms, "
                                "post chain in all %.3f ms (smoothed over the still's frames)\n",
                             pt.atmosphere, pt.mist_inject, pt.mist_integrate, pt.post_total);
                const engine::Renderer::CloudShadowReport& csr = g_renderer.last_cloud_shadow();
                if (csr.valid)
                    std::fprintf(f, "cloud shadow: %.1f%% of its 12 x 12 km sunlit (beam T > 0.5), mean T %.3f; "
                                    "percentiles 10/25/50/75/90: %.3f %.3f %.3f %.3f %.3f\n",
                                 csr.sunlit * 100.0f, csr.mean_t, csr.pct[0], csr.pct[1], csr.pct[2], csr.pct[3], csr.pct[4]);
                const engine::Renderer::SkyCheck& sk = g_renderer.last_sky_check();
                if (sk.valid)
                    std::fprintf(f, "sky table GPU vs CPU: %d texels, mean %.3f%%, worst %.3f%% at (%.0f, %.0f)\n",
                                 sk.texels, sk.mean_rel * 100.0f, sk.worst_rel * 100.0f, sk.worst_at[0], sk.worst_at[1]);
            }
            {
                // The exposure meter (ADR-004 L1): what it saw and what it
                // chose. The histogram is of log2 luminance in nits, every
                // second pixel each way, of what TAA produced.
                const engine::Renderer::ExposureReport& er = g_renderer.last_exposure();
                if (er.valid) {
                    std::fprintf(f, "exposure: EV100 %.2f (metered mean %.0f nits, this frame's target %.0f), "
                                    "x%.3g, compensation %+.1f stops, tonemap %s\n",
                                 er.ev100, std::exp2(er.adapted_log2), std::exp2(er.target_log2), er.exposure,
                                 g_renderer.ev_comp(),
                                 g_renderer.tonemap() == 1u ? "AgX" : g_renderer.tonemap() == 2u ? "ACES" : "raw");
                    std::fprintf(f, "dark vision: light presence %.2f (%.3g lux at the eye from visible local lights)\n",
                                 g_renderer.light_presence(), g_renderer.light_lux_at_eye());
                    std::fprintf(f, "exposure: %.0f pixels metered; 10th percentile %.3g nits, 90th %.3g nits "
                                    "(%.1f stops apart)\n",
                                 er.metered, er.lo_nits, er.hi_nits,
                                 er.lo_nits > 0.0f ? std::log2(er.hi_nits / er.lo_nits) : 0.0f);
                    // One row per stop: the share of metered pixels, as a bar.
                    const float step = (er.log_max - er.log_min) / 255.0f;
                    double tot = 0.0;
                    for (size_t k = 0; k < er.bins.size(); ++k) tot += er.bins[k];
                    for (int stop = int(er.log_min); stop < int(er.log_max); ++stop) {
                        double c = 0.0;
                        for (size_t k = 0; k < er.bins.size(); ++k) {
                            const float l = er.log_min + float(k) * step;
                            if (l >= float(stop) - 0.5f * step && l < float(stop + 1) - 0.5f * step) c += er.bins[k];
                        }
                        if (c <= 0.0) continue;
                        const double share = tot > 0.0 ? c / tot : 0.0;
                        char bar[61];
                        const int nb = int(share * 120.0 + 0.5) > 60 ? 60 : int(share * 120.0 + 0.5);
                        for (int q = 0; q < nb; ++q) bar[q] = '#';
                        bar[nb] = 0;
                        std::fprintf(f, "  %7.1f-%-8.1f nits %5.1f%% %s\n", std::exp2(float(stop)),
                                     std::exp2(float(stop + 1)), share * 100.0, bar);
                    }
                }
            }
            {
                // What the ground cover drew in this still.
                uint32_t fc[5] = {0, 0, 0, 0, 0};
                std::string ferr;
                if (g_renderer.foliage_counts(fc, ferr)) {
                    std::fprintf(f, "foliage: %u near blades, %u far blades, %u clump cards, %u bushes, "
                                    "%u overflowed; %zu tiles resident\n",
                                 fc[0], fc[1], fc[2], fc[3], fc[4], g_renderer.foliage().tiles_resident());
                }
            }
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
        // A chunk straddling the surface at the spawn. It was the origin while
        // the centre floor guaranteed land there; since v18 the origin can be
        // sea (on seed 0 the default chunk came back all air).
        float origin[3] = {g_spawn.x - 8.0f, g_spawn.ground - 8.0f, g_spawn.z - 8.0f};
        // --probe-at x z: the chunk centred on the ground there instead.
        {
            const std::string pc = cmdline ? cmdline : "";
            const size_t pa = pc.find("--probe-at");
            float px = 0.0f, pz = 0.0f;
            if (pa != std::string::npos && std::sscanf(pc.c_str() + pa + 10, "%f %f", &px, &pz) == 2) {
                const float gy = g_renderer.world().height(px, pz);
                origin[0] = px - 8.0f; origin[1] = gy - 8.0f; origin[2] = pz - 8.0f;
            }
        }
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
    // field shaders (unless cached -- see shader_compile.h) and builds the
    // whole seed before the first frame: ~1 s warm, ~8 s cold, with
    // build/startup.txt saying where it went. During a cold start the process
    // burns a core with no window up and looks like a hang. It is not one.
    // No SetForegroundWindow: grabbing focus seconds after launch would
    // interrupt whatever the user moved on to.
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
            fill_view(c, g_res_w, g_res_h, 0.0f, cv);
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
        float ov_slope = 0.0f;                // underside rise per metre, worst way
        bool  ov_found = false;
        bool  ov_fallback = false;
        float best_fallback_span = -1.0f;
        float fb[8] = {0, 0, 0, 0, 0, 0, 0, 0};
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
                    //
                    // And how level the underside is. A capsule launched at an
                    // inclined roof slides along it -- collide-and-slide keeps
                    // the tangential part of the velocity -- and off the edge of
                    // a narrow one. The v9 world's first real ceiling was solid
                    // 1 m out and inclined; the capsule hit it, slid 2.5 m
                    // sideways in ten frames, cleared the edge and rose past,
                    // and the first version of this test called that "passed
                    // straight THROUGH". Physics was right; the fixture was not.
                    float slope = 0.0f;
                    {
                        const float off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (int d = 0; d < 4; ++d) {
                            const float qx = x + off[d][0], qz = z + off[d][1];
                            float under = roof_bottom;
                            bool got = false;
                            for (float y = roof_bottom + 3.0f; y > roof_bottom - 3.0f; y -= 0.05f) {
                                if (wg.density(qx, y, qz) > 0.0f && wg.density(qx, y - 0.05f, qz) <= 0.0f) {
                                    under = y; got = true; break;
                                }
                            }
                            const float dy = got ? std::fabs(under - roof_bottom) : 3.0f;
                            if (dy > slope) slope = dy;   // metres per metre
                        }
                    }
                    const bool good = span >= 3.0f && slope <= 0.4f;
                    if (!good) {
                        // Kept as a fallback: the widest roof seen, so a world
                        // without a good one still gets the best test it has,
                        // and the report says which it was.
                        if (span > best_fallback_span) {
                            best_fallback_span = span;
                            ov_fallback = true;
                            fb[0] = x; fb[1] = z; fb[2] = floor_y; fb[3] = roof_bottom;
                            fb[4] = gap; fb[5] = span; fb[6] = roof_top - roof_bottom; fb[7] = slope;
                        }
                        continue;
                    }
                    ov_span = span;
                    ov_thick = roof_top - roof_bottom;
                    ov_slope = slope;

                    ov_x = x; ov_z = z;
                    ov_ground = floor_y;
                    ov_under = roof_bottom;
                    ov_gap = gap;
                    ov_found = true;
                }
            }
        }
        bool ov_is_fallback = false;
        if (!ov_found && ov_fallback) {
            ov_found = true;
            ov_is_fallback = true;
            ov_x = fb[0]; ov_z = fb[1]; ov_ground = fb[2]; ov_under = fb[3];
            ov_gap = fb[4]; ov_span = fb[5]; ov_thick = fb[6]; ov_slope = fb[7];
        }
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
        // What physics holds above the capsule at the moment of launch, and
        // how the capsule's feet moved over the first frames after it.
        engine::PhysicsWorld::UpProbe launch_probe;
        int slab_shapes[4] = {-1, -1, -1, -1};     // LOD 0 chunks floor..slab top
        int slab_keys_y[4] = {0, 0, 0, 0};
        int slab_nkeys = 0;
        std::vector<float> after_launch;
        std::vector<float> after_side;
        bool crossed = false;
        float side_at_cross = 0.0f;

        // Phase B: a ceiling the world cannot fail to provide. A sphere of
        // air dug a few metres under flat ground is a dome over the capsule,
        // and a dome funnels any sideways slide back to its apex -- so if
        // collision works the capsule MUST stop, whatever the terrain grew.
        // Phase A depends on the world having a wide, level roof near spawn,
        // and the v9 world does not; this does not depend on anything.
        const float kCavR = 3.5f, kCavDepth = 7.0f;
        bool cav_dug = false, cav_placed = false, cav_launched = false, cav_site = false;
        float cav_x = 0.0f, cav_y = 0.0f, cav_z = 0.0f, cav_grade = 0.0f;
        double cav_head_peak = -1e9;
        engine::PhysicsWorld::UpProbe cav_probe;
        int cav_shape = -1;
        // How long an overhang chunk is a solid-filled heightfield: until its
        // mesh shape lands, which waits in a 2-a-frame queue.
        size_t peak_mesh_backlog = 0;
        double backlog_clear_at = -1.0;

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
                const float head = g_feet[1] + 2.0f * (kCapsuleHalf + kCapsuleRadius);
                launch_probe = g_physics.probe_up(g_feet[0], head, g_feet[2], 60.0f);
                // Every LOD 0 chunk the column passes through from the floor to
                // the slab's top: which kind of body stands for each.
                const float S0 = engine::lod_chunk_size(0);
                const int kx = int(std::floor(ov_x / S0)), kz = int(std::floor(ov_z / S0));
                const int y0 = int(std::floor(ov_ground / S0));
                const int y1 = int(std::floor((ov_under + ov_thick) / S0));
                for (int ky = y0; ky <= y1 && slab_nkeys < 4; ++ky) {
                    engine::ChunkKey k; k.x = kx; k.y = ky; k.z = kz; k.lod = 0;
                    slab_keys_y[slab_nkeys] = ky;
                    slab_shapes[slab_nkeys++] = g_physics.chunk_shape(k);
                }
                g_physics.launch_character(45.0f);
            }
            if (placed_under) speed = 0.0f;

            if (total >= 14.0 && !cav_dug) {
                cav_dug = true;
                // Flat land near spawn, well above the sea.
                const engine::WorldGpu& wg = g_renderer.world();
                const float sea = wg.params().sea_level;
                for (int i = 0; i < 400 && !cav_site; ++i) {
                    const float cx = 40.0f + 8.0f * float(i % 20), cz = 40.0f + 8.0f * float(i / 20);
                    const float h = wg.height(cx, cz);
                    if (h < sea + 8.0f) continue;
                    float worst = 0.0f;
                    const float o[4][2] = {{4, 0}, {-4, 0}, {0, 4}, {0, -4}};
                    for (int d = 0; d < 4; ++d) {
                        const float dh = std::fabs(wg.height(cx + o[d][0], cz + o[d][1]) - h);
                        if (dh > worst) worst = dh;
                    }
                    if (worst > 1.5f) continue;
                    cav_site = true;
                    cav_x = cx; cav_z = cz; cav_grade = h;
                    cav_y = h - kCavDepth;
                }
                if (cav_site) {
                    g_renderer.dig(cav_x, cav_y, cav_z, kCavR, -4.0f, 1);
                    // Stand on the surface above it while it remeshes, so
                    // the chunks around it are the finest ones.
                    g_physics.place_character(cav_x, cav_grade + 1.0f, cav_z);
                }
            }
            if (total >= 17.0 && cav_site && !cav_placed) {
                cav_placed = true;
                // A metre up from the cavity's floor, where it is 4.9 m wide.
                g_physics.place_character(cav_x, cav_y - kCavR + 1.0f, cav_z);
            }
            if (total >= 17.5 && cav_placed && !cav_launched) {
                cav_launched = true;
                const float head = g_feet[1] + 2.0f * (kCapsuleHalf + kCapsuleRadius);
                cav_probe = g_physics.probe_up(g_feet[0], head, g_feet[2], 20.0f);
                const float S0 = engine::lod_chunk_size(0);
                engine::ChunkKey k;
                k.x = int(std::floor(cav_x / S0));
                k.y = int(std::floor((cav_y + kCavR) / S0));
                k.z = int(std::floor(cav_z / S0));
                k.lod = 0;
                cav_shape = g_physics.chunk_shape(k);
                g_physics.launch_character(45.0f);
            }
            if (total >= 14.0) speed = 0.0f;
            {
                const size_t bl = g_renderer.stats().mesh_backlog;
                if (bl > peak_mesh_backlog) peak_mesh_backlog = bl;
                if (bl == 0 && backlog_clear_at < 0.0 && total > 1.0) backlog_clear_at = total;
                if (bl > 0) backlog_clear_at = -1.0;
            }

            sync_sea(float(total));
            g_physics.update(float(dt), 0.0f, speed, false);
            g_physics.character_position(g_feet);
            if (launched && total < 14.0 && g_feet[1] > launch_peak) launch_peak = g_feet[1];
            if (cav_launched) {
                const double hd = double(g_feet[1]) + 2.0 * double(kCapsuleHalf + kCapsuleRadius);
                if (hd > cav_head_peak) cav_head_peak = hd;
            }
            if (launched && total < 14.0 && !crossed &&
                g_feet[1] + 2.0f * (kCapsuleHalf + kCapsuleRadius) > ov_under + 0.5f) {
                crossed = true;
                const float cx = g_feet[0] - ov_x, cz = g_feet[2] - ov_z;
                side_at_cross = std::sqrt(cx * cx + cz * cz);
            }
            if (launched && total < 14.0 && after_launch.size() < 24) {
                after_launch.push_back(g_feet[1]);
                // How far it has moved sideways from where it was launched:
                // a capsule that slides along an inclined underside and off
                // its edge is not one that went through it.
                const float sx = g_feet[0] - ov_x, sz = g_feet[2] - ov_z;
                after_side.push_back(std::sqrt(sx * sx + sz * sz));
            }

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

            if (total > 2.0 && total < 14.0) {
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
                        fill_view(c, g_res_w, g_res_h, 0.0f, cv);
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
            std::fprintf(f, "%s roof: underside rises at most %.2f m per metre (good: >= 3 m wide, <= 0.40)\n",
                         ov_is_fallback ? "FALLBACK (no wide, level roof found)" : "a wide, near-level",
                         ov_slope);
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
                // The verdict is about WHERE the head crossed the underside,
                // not only whether it got higher than it.
                const char* verdict =
                    !crossed ? "it was STOPPED by the overhang"
                    : side_at_cross >= ov_span
                        ? "it SLID OFF the roof's edge and rose past it -- not a collision failure"
                        : "it went THROUGH the overhang inside its footprint -- a COLLISION FAILURE";
                std::fprintf(f, "  the underside is at %.2f m; ", ov_under);
                if (crossed)
                    std::fprintf(f, "the head crossed it %.2f m sideways, against a roof %.1f m wide; ",
                                 side_at_cross, ov_span);
                std::fprintf(f, "so %s\n", verdict);
            }
            if (launched) {
                const char* kind[3] = {"NO BODY", "heightfield", "mesh shape"};
                std::fprintf(f, "  at the launch, a ray up from the head: front faces %s, any face %s",
                             launch_probe.hit_front ? "hit" : "missed",
                             launch_probe.hit_any ? "hit" : "missed");
                if (launch_probe.hit_any)
                    std::fprintf(f, " at %.2f m (a %s, LOD %d)", launch_probe.dist_any,
                                 launch_probe.is_mesh ? "mesh shape" : "heightfield",
                                 launch_probe.lod);
                std::fprintf(f, "\n  LOD 0 chunks in that column, floor to slab top:");
                for (int i = 0; i < slab_nkeys; ++i)
                    std::fprintf(f, " [y %d: %s]", slab_keys_y[i],
                                 slab_shapes[i] >= 0 ? kind[slab_shapes[i]] : "?");
                std::fprintf(f, "\n  feet over the first frames after launch (y m / metres sideways):");
                for (size_t i = 0; i < after_launch.size(); ++i)
                    std::fprintf(f, " %.2f/%.2f", after_launch[i], after_side[i]);
                std::fprintf(f, "\n");
            }
            // Phase B's verdict. The apex of the dome is the roof; a skin of
            // half a metre allows for Jolt's character padding and for the
            // mesh sitting off the field at the voxel.
            std::fprintf(f, "\nphase B, a cavity dug for the purpose (radius %.1f m, %.1f m under flat ground):\n",
                         kCavR, kCavDepth);
            if (!cav_site) {
                std::fprintf(f, "  NO SITE: no flat land above the sea near spawn, so nothing was dug\n");
            } else if (!cav_launched) {
                std::fprintf(f, "  NOT RUN: the run ended before the launch\n");
            } else {
                const double apex = double(cav_y) + double(kCavR);
                const char* kind[3] = {"NO BODY", "heightfield", "mesh shape"};
                std::fprintf(f, "  at (%.0f, %.0f): dome apex at %.2f m, %.2f m of rock above it\n",
                             cav_x, cav_z, apex, double(cav_grade) - apex);
                std::fprintf(f, "  the chunk holding the apex is a %s; a ray up from the head %s",
                             cav_shape >= 0 ? kind[cav_shape] : "?",
                             cav_probe.hit_any ? "hit" : "MISSED");
                if (cav_probe.hit_any) std::fprintf(f, " at %.2f m", cav_probe.dist_any);
                std::fprintf(f, "\n  launched at 45 m/s, the head peaked at %.2f m: %s\n", cav_head_peak,
                             cav_head_peak <= apex + 0.5 ? "STOPPED by the roof -- collision holds"
                                                         : "went THROUGH the roof -- a COLLISION FAILURE");
            }
            std::fprintf(f, "chunks classified: %llu, needing a mesh: %llu (%.1f%%)\n",
                         st.chunks_classified, st.chunks_needing_mesh,
                         st.chunks_classified ? 100.0 * double(st.chunks_needing_mesh) /
                                                     double(st.chunks_classified) : 0.0);
            std::fprintf(f, "mesh shapes delivered: %llu, still waiting: %zu\n",
                         st.mesh_shapes_delivered, st.mesh_backlog);
            std::fprintf(f, "mesh-shape backlog: peak %zu chunks, last cleared at %.1f s\n",
                         peak_mesh_backlog, backlog_clear_at);
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

    if (leak_mode) {
        // --leak-test (ADR-004 L6): the failure every GI is judged by.
        // A sphere of air dug 10 m under flat ground -- sealed, its roof six
        // metres of rock -- and the camera in it: with no light inside, its
        // luminance must be ~0, and a torch must fill it. Measured as the
        // exposure meter's mean (nits, 10-90% band) after 240 frames, each
        // config against the same chamber.
        std::remove("build/leak_report.txt");
        FILE* f = std::fopen("build/leak_report.txt", "w");
        if (!f) return 1;
        const engine::WorldGpu& wg = g_renderer.world();
        const float sea = wg.params().sea_level;
        bool site = false;
        float cx = 0.0f, cz = 0.0f, grade = 0.0f;
        // From the spawn, as every other mode starts: from (40, 40), the
        // pre-v16 centre, v18's archipelago put the search in the sea on
        // every seed tried, and the test reported nothing.
        for (int i = 0; i < 400 && !site; ++i) {
            const float x = g_spawn.x + 8.0f * float(i % 20) - 76.0f, z = g_spawn.z + 8.0f * float(i / 20) - 76.0f;
            const float h = wg.height(x, z);
            if (h < sea + 12.0f) continue;
            float worst = 0.0f;
            const float o[4][2] = {{6, 0}, {-6, 0}, {0, 6}, {0, -6}};
            for (int d = 0; d < 4; ++d) worst = (std::max)(worst, std::fabs(wg.height(x + o[d][0], z + o[d][1]) - h));
            if (worst > 1.5f) continue;
            site = true;
            cx = x; cz = z; grade = h;
        }
        if (!site) {
            std::fprintf(f, "no flat site near spawn\n");
            std::fclose(f);
            g_renderer.shutdown();
            g_physics.shutdown();
            return 1;
        }
        const float kR = 4.0f, kDepth = 10.0f;
        const float cy = grade - kDepth;
        int leak_frames = 240;
        {
            const size_t k = hl.find("--leak-frames ");
            if (k != std::string::npos) leak_frames = std::atoi(hl.c_str() + k + 14);
        }
        g_renderer.dig(cx, cy, cz, kR, -4.0f, 1);
        std::fprintf(f, "leak test (ADR-004 L6): a sphere of air r %.1f m, centre %.1f m under flat ground at (%.0f, %.1f, %.0f); "
                        "roof %.1f m of rock\n",
                     kR, kDepth, cx, cy, cz, kDepth - kR);
        std::fprintf(f, "\n");
        // `hf`: the heightfield GI; without it the bare two-hemisphere
        // fallback is what leaked 566 nits. (DDGI's probe configurations went
        // with hardware RT: git tag rt-last.)
        struct Cfg { const char* name; bool inside, torch, fog, hf; };
        const Cfg cfgs[] = {
            {"sealed, heightfield GI", true, false, true, true},
            {"sealed, heightfield GI, no mist", true, false, false, true},
            {"sealed, bare fallback (--no-hfgi)", true, false, true, false},
            {"sealed, heightfield GI, torch", true, true, true, true},
            {"outside, standing on the ground above", false, false, true, true},
        };
        double dark = -1.0, dark_clear = -1.0, fallback = -1.0, torch = -1.0;
        // Settle inside first: the chamber is re-meshed over the first frames.
        {
            g_camera.x = cx; g_camera.y = cy; g_camera.z = cz;
            g_camera.yaw = 0.0f; g_camera.pitch = -0.35f;
            engine::ViewConstants v = {};
            fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);
            std::string err;
            g_renderer.capture(v, "build/leak.bmp", err);
            for (int i = 0; i < 300; ++i) g_renderer.step_offscreen(v);
        }
        for (const Cfg& c : cfgs) {
            g_renderer.set_carried_torch(c.torch);
            g_renderer.set_fog(c.fog);
            g_renderer.set_hfgi(c.hf);
            if (c.inside) {
                g_camera.x = cx; g_camera.y = cy; g_camera.z = cz;
                g_camera.yaw = 0.0f; g_camera.pitch = -0.35f;
            } else {
                g_camera.x = cx; g_camera.y = grade + 1.7f; g_camera.z = cz + 6.0f;
                g_camera.yaw = 0.0f; g_camera.pitch = -0.3f;
            }
            engine::ViewConstants v = {};
            fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);
            std::string err;
            g_renderer.capture(v, "build/leak.bmp", err);
            for (int i = 0; i < leak_frames; ++i) g_renderer.step_offscreen(v);
            char path[64];
            std::snprintf(path, sizeof path, "build/leak_%d.bmp", int(&c - cfgs));
            const bool ok = g_renderer.capture(v, path, err);
            const engine::Renderer::ExposureReport& ex = g_renderer.last_exposure();
            // The meter's own target stops at EV100 -2 (0.03 nits), so read
            // its histogram instead: the mean of the 10-90% band, in log2,
            // the way the meter weighs it -- down to 0.001 nits.
            double nits = -1.0;
            if (ex.valid && !ex.bins.empty()) {
                double total = 0.0;
                for (unsigned b : ex.bins) total += b;
                const double step = (ex.log_max - ex.log_min) / double(ex.bins.size());
                double seen = 0.0, acc = 0.0, w = 0.0;
                for (size_t b = 0; b < ex.bins.size(); ++b) {
                    const double lo = seen / total, hi = (seen + ex.bins[b]) / total;
                    seen += ex.bins[b];
                    const double in = (std::min)(hi, 0.9) - (std::max)(lo, 0.1);
                    if (in <= 0.0) continue;
                    acc += in * (ex.log_min + (double(b) + 0.5) * step);
                    w += in;
                }
                if (w > 0.0) nits = std::exp2(acc / w);
            }
            std::fprintf(f, "  %-40s %s mean %.4g nits (EV100 %.2f) -> %s\n", c.name, ok ? "" : "CAPTURE FAILED", nits,
                         ex.ev100, path);
            if (c.inside && !c.torch && c.fog && c.hf) dark = nits;
            if (c.inside && !c.torch && !c.fog && c.hf) dark_clear = nits;
            if (c.inside && !c.torch && !c.hf) fallback = nits;
            if (c.inside && c.torch) torch = nits;
        }
        g_renderer.set_carried_torch(false);
        g_renderer.set_fog(true);
        g_renderer.set_hfgi(true);
        // A full moon puts ~0.01 nits on a meadow; sealed and unlit must be
        // well under that.
        std::fprintf(f, "\nverdict: sealed and unlit, the heightfield GI %.3g nits with mist, %.3g without (%s) -- under "
                        "0.01, a moonlit meadow; the bare fallback leaks %.4g; a carried torch %.3g\n",
                     dark, dark_clear, dark >= 0.0 && dark < 0.01 && dark_clear < 0.01 ? "PASS" : "LEAKS", fallback, torch);
        std::fclose(f);
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (lights_mode) {
        // --lights-report (ADR-004 L3): local lights at night, at ground
        // level, measured -- the frame with none, with the torch, with 64
        // campfires with and without their shadows, and with 255 -- and what
        // the clusters and the shadow budget did.
        const std::string cl = cmdline ? cmdline : "";
        if (cl.find("--time") == std::string::npos) g_renderer.set_time_of_day(23.0f);
        g_renderer.set_vsync(false);
        const engine::WorldGpu& wg = g_renderer.world();
        g_camera.x = g_spawn.x; g_camera.z = g_spawn.z;
        g_camera.y = wg.height(g_camera.x, g_camera.z) + 1.7f;
        g_camera.yaw = 0.15f; g_camera.pitch = -0.12f;
        std::remove("build/lights_report.txt");
        // A running clock: flames flicker and the carried torch wanders, so
        // its shadow is redrawn every frame as it is in play. A frozen clock
        // cached it and reported its shadows as free.
        float clock = 0.0f;
        auto frame = [&]() -> bool {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) return false;
            clock += 1.0f / 60.0f;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, clock, v);
            g_renderer.render(v);
            return true;
        };
        for (int i = 0; i < 900; ++i) {
            if (!frame()) break;
            if (i > 60 && g_renderer.stats().pending == 0 && g_renderer.tree_field().settled()) break;
        }
        FILE* f = std::fopen("build/lights_report.txt", "w");
        if (!f) return 1;
        const engine::SkyFrame& sf = g_renderer.sky_frame();
        std::fprintf(f, "lights report (ADR-004 L3): %02d:%02d, key light the %s; camera at ground level, (0, %.1f, 0)\n\n",
                     int(sf.hours), int((sf.hours - float(int(sf.hours))) * 60.0f), sf.key_is_moon ? "moon" : "sun", g_camera.y);
        // `walk`: the camera walks at 3 m/s, so the carried torch moves every
        // frame and all six of its faces are redrawn every frame -- a torch
        // held still is redrawn only once it has wandered a centimetre.
        struct Config { const char* name; bool torch; int lights; bool shadows; bool walk; };
        const Config configs[] = {
            {"no local lights", false, 0, true, false},
            {"the torch, held still", true, 0, true, false},
            {"the torch, walking at 3 m/s", true, 0, true, true},
            {"torch + 64 campfires, no local shadows", true, 64, false, false},
            {"torch + 64 campfires", true, 64, true, false},
            {"torch + 255 campfires", true, 255, true, false},
        };
        const float home_x = g_camera.x;
        LARGE_INTEGER qf, q0, q1;
        QueryPerformanceFrequency(&qf);
        for (const Config& c : configs) {
            // Home first: the campfires are scattered round the camera, and a
            // walking config leaves it 20 m away.
            g_camera.x = home_x;
            g_camera.y = wg.height(g_camera.x, g_camera.z) + 1.7f;
            g_renderer.clear_lights();
            g_renderer.set_carried_torch(c.torch);
            g_renderer.set_light_shadows(c.shadows);
            scatter_lights(c.lights);
            for (int i = 0; i < 90; ++i) frame();
            std::vector<double> ms;
            unsigned faces = 0, draws = 0;
            for (int i = 0; i < 400; ++i) {
                if (c.walk) {
                    g_camera.x += 0.05f;
                    g_camera.y = wg.height(g_camera.x, g_camera.z) + 1.7f;
                }
                QueryPerformanceCounter(&q0);
                if (!frame()) break;
                QueryPerformanceCounter(&q1);
                ms.push_back(1000.0 * double(q1.QuadPart - q0.QuadPart) / double(qf.QuadPart));
                faces += g_renderer.light_stats().faces_drawn;
                draws += g_renderer.light_stats().chunk_draws;
            }
            std::sort(ms.begin(), ms.end());
            double mean = 0.0;
            for (double x : ms) mean += x;
            mean = ms.empty() ? 0.0 : mean / double(ms.size());
            const double p99 = ms.empty() ? 0.0 : ms[size_t(double(ms.size() - 1) * 0.99)];
            const engine::Renderer::LightStats& ls = g_renderer.light_stats();
            std::fprintf(f, "%-40s frame mean %.2f ms, 99th %.2f ms\n", c.name, mean, p99);
            std::fprintf(f, "    lights %u, with a shadow ready %u; shadow faces redrawn %.1f a frame (%.0f chunk draws)\n",
                         ls.lights, ls.shadowed, ms.empty() ? 0.0 : double(faces) / ms.size(),
                         ms.empty() ? 0.0 : double(draws) / ms.size());
            std::fprintf(f, "    clusters: %u of %u lit, %u lights listed in all (%.1f per lit cluster), most in one %u, overflowed %u\n",
                         ls.clusters_lit, engine::kClusterCount, ls.cluster_items,
                         ls.clusters_lit ? double(ls.cluster_items) / ls.clusters_lit : 0.0,
                         ls.max_in_cluster, ls.overflowed);
        }
        std::fclose(f);
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (dig_tree_mode) {
        // --dig-settle: a tree, then a boulder, dug out from under. Picks a fellable tree near
        // spawn, carves a 3 m sphere a metre under its base, then with the
        // clock running:
        //   1. watches it drop -- straight down, under gravity, to the new
        //      ground under its trunk (Renderer::ground_below), upright;
        //   2. probes its trunk collider half a metre over the NEW base,
        //      where the old capsule (built at the old base) would be absent;
        //   3. stands beside it at the bottom of the hole and chops it down.
        // A still of it in the hole first: build/dig_tree.bmp. Then a boulder
        // near spawn: a hole under its whole footprint, the drop, and a ray
        // down its axis that must hit its collider at its NEW top
        // (build/dig_rock.bmp).
        const engine::WorldGpu& wg = g_renderer.world();
        g_renderer.set_vsync(true);
        g_walk = true;
        std::remove("build/dig_settle.txt");
        LARGE_INTEGER tf, tp;
        QueryPerformanceFrequency(&tf);
        auto frame = [&](float tt) -> bool {
            sync_physics_terrain();
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) return false;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, tt, v);
            g_renderer.render(v);
            return true;
        };
        auto standing_y = [&](uint32_t id, float& y) -> bool {
            const std::vector<worldgen::TreeInstance>& st = g_renderer.tree_field().standing();
            for (size_t i = 0; i < st.size(); ++i) if (st[i].id == id) { y = st[i].y; return true; }
            return false;
        };
        g_camera.x = g_spawn.x; g_camera.z = g_spawn.z;
        g_camera.y = wg.height(g_camera.x, g_camera.z) + 2.0f;
        for (int i = 0; i < 600 && !g_renderer.tree_field().settled(); ++i) { frame(0.0f); Sleep(5); }
        for (int i = 0; i < 5; ++i) frame(0.0f);

        FILE* f = std::fopen("build/dig_settle.txt", "w");
        if (!f) return 1;
        const std::vector<worldgen::TreeInstance> st = g_renderer.tree_field().standing();
        const worldgen::TreeInstance* pick = nullptr;
        float best = 1e30f;
        for (size_t i = 0; i < st.size(); ++i) {
            float b[3], ht, tr;
            int hits;
            if (!g_felling.tree_info(st[i].id, b, ht, tr, hits) || tr < 0.12f || ht < 8.0f) continue;
            const float dx = st[i].x - g_spawn.x, dz = st[i].z - g_spawn.z;
            if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; pick = &st[i]; }
        }
        if (!pick) {
            std::fprintf(f, "VOID RUN: no fellable tree near spawn (%zu standing)\n", st.size());
            std::fclose(f);
            return 1;
        }
        const worldgen::TreeInstance tree = *pick;
        float b0[3], height, trunk_r;
        int need = 0;
        g_felling.tree_info(tree.id, b0, height, trunk_r, need);
        std::fprintf(f, "tree %08x, %.1f m tall, trunk radius %.2f m, base (%.2f, %.2f, %.2f)\n",
                     tree.id, height, trunk_r, tree.x, tree.y, tree.z);

        const float y0 = tree.y;
        g_renderer.dig(tree.x, y0 - 1.0f, tree.z, 3.0f, -4.0f, 1);
        // How long the hole takes to reach the screen: frames until every
        // chunk the dig threw away has its remesh drawn (Streamer's edit
        // swap: no cross-fade; it was the remesh plus 16 faded frames).
        int swap_frames = -1;
        {
            const size_t dug = g_renderer.stats().chunks_invalidated;
            for (int i = 1; i <= 60; ++i) {
                frame(0.0f);
                if (g_renderer.streamer().edit_swaps_pending() == 0) { swap_frames = i; break; }
            }
            std::fprintf(f, "remesh: %zu chunks thrown away, all redrawn after %d frame(s), "
                            "swapped without a fade\n", dug, swap_frames);
        }
        const float expect = g_renderer.ground_below(tree.x, tree.z, y0);
        std::fprintf(f, "dug a 3 m sphere 1 m under the base; ground under the trunk now %.2f (%.2f m down)\n",
                     expect, y0 - expect);

        // 1. The drop, on the real clock.
        float t = 0.0f, y_prev = y0, y = y0, max_step = 0.0f, landed_at = -1.0f;
        bool rose = false;
        QueryPerformanceCounter(&tp);
        unsigned long long frames = 0;
        while (t < 4.0f) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = float(double(now.QuadPart - tp.QuadPart) / double(tf.QuadPart));
            tp = now;
            if (dt > 0.1f) dt = 0.1f;
            t += dt;
            g_felling.update(tree.x + 6.0f, tree.z);
            frame(t);
            ++frames;
            if (!standing_y(tree.id, y)) break;
            if (y > y_prev + 1e-4f) rose = true;
            if (y_prev - y > max_step) max_step = y_prev - y;
            if (landed_at < 0.0f && std::fabs(y - expect) < 0.01f) landed_at = t;
            y_prev = y;
        }
        const float fall = y0 - expect;
        std::fprintf(f, "drop: %.2f m in %.2f s over %llu frames (free fall from rest: %.2f s); "
                        "now at %.3f, %.3f off the ground; largest step %.2f m; moved up: %s\n",
                     y0 - y, landed_at, frames, std::sqrt(2.0f * fall / 9.81f), y, y - expect, max_step,
                     rose ? "YES" : "no");

        // The still, from the rim, looking down at the base.
        {
            engine::Camera c = g_camera;
            c.x = tree.x + 7.0f; c.z = tree.z - 7.0f;
            c.y = wg.height(c.x, c.z) + 5.0f;
            const float dx = tree.x - c.x, dz = tree.z - c.z, dy = (y + 1.5f) - c.y;
            c.yaw = std::atan2(dx, dz);
            c.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
            engine::ViewConstants cv = {};
            fill_view(c, g_res_w, g_res_h, t, cv);
            std::string e3;
            g_renderer.capture(cv, "build/dig_tree.bmp", e3);
        }

        // 2. The trunk collider at the new base.
        for (int i = 0; i < 3; ++i) { g_felling.update(tree.x + 1.0f, tree.z); frame(t); }
        int prop = 0;
        const float from[3] = {tree.x + 1.5f, y + 0.5f, tree.z};
        const float dir[3] = {-1.0f, 0.0f, 0.0f};
        const float hit = g_physics.probe_ray(from, dir, 3.0f, prop);
        std::fprintf(f, "collider: a ray 0.5 m over the new base hits %s at %.2f m (bark at %.2f)\n",
                     hit < 0.0f ? "NOTHING" : (prop ? "a prop" : "terrain"), hit, 1.5f - trunk_r);

        // 3. Chop it from the bottom of the hole.
        const float eye[3] = {tree.x + trunk_r + 1.0f, y + kEyeAboveFeet, tree.z};
        int swings = 0, r = 0;
        while (swings < 40 && r != 2) {
            r = g_felling.chop(eye, dir, t, nullptr, nullptr, nullptr);
            ++swings;
            t += 0.2f;
            frame(t);
            if (r == 0) break;
        }
        float lp[3] = {0, 0, 0}, lr[4];
        bool active = false;
        const bool have_log = g_felling.log_pose(tree.id, lp, lr, active);
        std::fprintf(f, "chop: %s after %d swings (needs %d); log starts at y %.2f (new base %.2f)\n",
                     r == 2 ? "FELLED" : (r == 0 ? "NOTHING IN REACH" : "still standing"), swings, need,
                     have_log ? lp[1] : 0.0f, y);

        const bool tree_pass = landed_at >= 0.0f && landed_at < 3.0f && !rose && std::fabs(y - expect) < 0.02f &&
                               fall > 1.0f && hit >= 0.0f && prop && r == 2;
        std::fprintf(f, "tree: %s\n\n", tree_pass ? "pass" : "FAIL");

        // A boulder: the nearest to spawn over 1.5 m across, clear of the tree.
        bool rock_pass = false;
        {
            for (int i = 0; i < 600 && !g_renderer.rock_field().settled(); ++i) { frame(t); Sleep(5); }
            const worldgen::RockInstance* bp = nullptr;
            float bd = 1e30f;
            for (const worldgen::RockInstance& rk : g_renderer.rock_field().rocks()) {
                if (rk.kind != 0 || rk.size < 1.5f) continue;
                const float tx = rk.x - tree.x, tz = rk.z - tree.z;
                if (tx * tx + tz * tz < 15.0f * 15.0f) continue;
                const float dx = rk.x - g_spawn.x, dz = rk.z - g_spawn.z;
                if (dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; bp = &rk; }
            }
            if (!bp) {
                std::fprintf(f, "rock: VOID, no boulder over 1.5 m resident near spawn\n");
            } else {
                const worldgen::RockInstance rock = *bp;
                const engine::RockModel& rm = g_renderer.rocks().model(engine::rock_model(rock.kind, rock.variant));
                std::fprintf(f, "boulder %08x, %.2f m across, origin y %.2f, ground %.2f, %.0f m from spawn\n",
                             rock.id, rock.size, rock.y, rock.ground, std::sqrt(bd));
                const float R = 0.6f * rock.size + 1.5f;
                g_renderer.dig(rock.x, rock.ground - 0.5f, rock.z, R, -4.0f, 1);
                auto rock_y = [&](float& ry) -> bool {
                    for (const worldgen::RockInstance& rk : g_renderer.rock_field().rocks())
                        if (rk.id == rock.id) { ry = rk.y; return true; }
                    return false;
                };
                float ry = rock.y, rprev = rock.y, rlanded = -1.0f, last_move = 0.0f;
                bool rrose = false;
                const float t_start = t;
                QueryPerformanceCounter(&tp);
                while (t - t_start < 4.0f) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    float dt = float(double(now.QuadPart - tp.QuadPart) / double(tf.QuadPart));
                    tp = now;
                    if (dt > 0.1f) dt = 0.1f;
                    t += dt;
                    g_felling.update(rock.x, rock.z);
                    frame(t);
                    if (!rock_y(ry)) break;
                    if (ry > rprev + 1e-4f) rrose = true;
                    if (ry < rprev - 1e-4f) last_move = t - t_start;
                    rprev = ry;
                }
                rlanded = last_move;
                const float rdrop = rock.y - ry;
                std::fprintf(f, "dug a %.1f m sphere 0.5 m under its middle: dropped %.2f m, still after %.2f s "
                                "(free fall: %.2f s); moved up: %s\n",
                             R, rdrop, rlanded, std::sqrt(2.0f * (rdrop > 0.0f ? rdrop : 0.0f) / 9.81f),
                             rrose ? "YES" : "no");
                {
                    engine::Camera c = g_camera;
                    c.x = rock.x + 3.0f + 0.5f * rock.size; c.z = rock.z - 3.0f - 0.5f * rock.size;
                    c.y = wg.height(c.x, c.z) + 6.0f + rock.size;
                    const float dx = rock.x - c.x, dz = rock.z - c.z, dy = ry - c.y;
                    c.yaw = std::atan2(dx, dz);
                    c.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
                    engine::ViewConstants cv = {};
                    fill_view(c, g_res_w, g_res_h, t, cv);
                    std::string e3;
                    g_renderer.capture(cv, "build/dig_rock.bmp", e3);
                }
                for (int i = 0; i < 3; ++i) { g_felling.update(rock.x, rock.z); frame(t); }
                const float top = ry + rm.height * rock.size;
                const float rf[3] = {rock.x, top + 30.0f, rock.z};
                const float down[3] = {0.0f, -1.0f, 0.0f};
                int rprop = 0;
                const float rh = g_physics.probe_ray(rf, down, 80.0f, rprop);
                const float hy = rh < 0.0f ? 0.0f : rf[1] - rh;
                std::fprintf(f, "collider: a ray down its axis hits %s at y %.2f; its new top %.2f\n",
                             rh < 0.0f ? "NOTHING" : (rprop ? "a prop" : "terrain"), hy, top);
                rock_pass = rdrop > 0.5f && !rrose && rlanded < 3.0f && rh >= 0.0f && rprop &&
                            std::fabs(hy - top) < 0.5f + 0.15f * rock.size;
                std::fprintf(f, "rock: %s\n", rock_pass ? "pass" : "FAIL");
            }
        }
        const bool pass = tree_pass && rock_pass;
        std::fprintf(f, "\n%s\n", pass ? "PASS" : "FAIL");
        std::fclose(f);
        g_renderer.shutdown();
        g_physics.shutdown();
        return pass ? 0 : 1;
    }

    if (fell_mode) {
        // --fell: the whole life of one tree, measured. Picks a real tree
        // near spawn, stands the character 1.6 m off its trunk, and:
        //   1. walks INTO the trunk for 1.5 s -- the capsule must stop at
        //      the bark, which is the trunk collider working;
        //   2. swings at it until it falls, counting swings;
        //   3. watches the log for 12 s: how far it tipped, whether it came
        //      to rest, and whether any part of it sank into the ground.
        // --fell-save then writes the felled tree out; --fell-verify, in a
        // SEPARATE process, loads it and checks the log is where it rested,
        // the tree no longer stands, and the stump does.
        const std::string cl = cmdline ? cmdline : "";
        const bool do_save = cl.find("--fell-save") != std::string::npos;
        const bool do_verify = cl.find("--fell-verify") != std::string::npos;
        const engine::WorldGpu& wg = g_renderer.world();
        g_renderer.set_vsync(true);
        g_walk = true;
        std::remove("build/fell.txt");

        LARGE_INTEGER tf, t0, tp;
        QueryPerformanceFrequency(&tf);
        // Every frame hands its terrain to physics -- the gotcha CLAUDE.md
        // names, met again: the first version's settle frames rendered
        // without syncing, threw away every chunk that landed, and the
        // character fell through the world beneath the trunk it was meant to
        // walk into. Its contact list was empty, ground included.
        auto frame = [&](float tt) -> bool {
            sync_physics_terrain();
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) return false;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, tt, v);
            g_renderer.render(v);
            return true;
        };

        // Let the tree field fill round spawn before choosing.
        g_camera.x = g_spawn.x; g_camera.z = g_spawn.z;
        g_camera.y = wg.height(g_camera.x, g_camera.z) + 2.0f;
        for (int i = 0; i < 600 && !g_renderer.tree_field().settled(); ++i) { frame(0.0f); Sleep(5); }
        for (int i = 0; i < 5; ++i) frame(0.0f);

        FILE* f = std::fopen("build/fell.txt", "w");
        if (!f) return 1;
        int verdict_fail = 0;

        if (do_verify) {
            std::string err;
            bool mismatch = false;
            float want[7] = {0, 0, 0, 0, 0, 0, 1};
            uint32_t want_id = 0;
            if (FILE* pf = std::fopen("build/fell_pose.txt", "r")) {
                std::fscanf(pf, "%u %f %f %f %f %f %f %f", &want_id, &want[0], &want[1], &want[2],
                            &want[3], &want[4], &want[5], &want[6]);
                std::fclose(pf);
            }
            const bool ok = g_felling.load("build/trees.bin", wg.field_id(), err, mismatch);
            for (int i = 0; i < 5; ++i) { g_felling.update(g_camera.x, g_camera.z); frame(0.0f); }
            float pos[3], rot[4];
            bool active = false;
            const bool have = g_felling.log_pose(want_id, pos, rot, active);
            bool standing = false;
            const std::vector<worldgen::TreeInstance>& st = g_renderer.tree_field().standing();
            for (size_t i = 0; i < st.size(); ++i) if (st[i].id == want_id) standing = true;
            const float dx = pos[0] - want[0], dy = pos[1] - want[1], dz = pos[2] - want[2];
            const float dpos = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float qd = std::fabs(rot[0] * want[3] + rot[1] * want[4] + rot[2] * want[5] + rot[3] * want[6]);
            std::fprintf(f, "a SEPARATE process: loaded build/trees.bin (%s)\n", ok ? "ok" : err.c_str());
            std::fprintf(f, "tree %08x: log %s, %.4f m from where it rested, rotation agreement %.5f\n",
                         want_id, have ? "present" : "MISSING", dpos, qd);
            std::fprintf(f, "the tree is standing again: %s\n", standing ? "YES -- FAIL" : "no");
            std::fprintf(f, "felled trees loaded: %zu\n", g_felling.felled());
            const bool pass = ok && have && !standing && dpos < 0.01f && qd > 0.9999f;
            std::fprintf(f, "\n%s\n", pass ? "SURVIVED the restart" : "FAILED");
            std::fclose(f);
            g_renderer.shutdown();
            g_physics.shutdown();
            return pass ? 0 : 1;
        }

        // A tree: nearest to spawn with a trunk worth felling and a clear
        // place to stand beside it.
        const std::vector<worldgen::TreeInstance> st = g_renderer.tree_field().standing();
        const worldgen::TreeInstance* pick = nullptr;
        float stand_x = 0.0f, stand_z = 0.0f, best = 1e30f;
        for (size_t i = 0; i < st.size(); ++i) {
            float base[3], height, tr;
            int hits;
            if (!g_felling.tree_info(st[i].id, base, height, tr, hits)) continue;
            if (tr < 0.12f || height < 8.0f) continue;
            const float d = st[i].x * st[i].x + st[i].z * st[i].z;
            if (d >= best) continue;
            for (int k = 0; k < 8; ++k) {
                const float a = 0.785398f * k;
                const float sx = st[i].x + std::cos(a) * (tr + 1.6f), sz = st[i].z + std::sin(a) * (tr + 1.6f);
                bool clear = std::fabs(wg.height(sx, sz) - st[i].y) < 0.8f;
                for (size_t j = 0; j < st.size() && clear; ++j) {
                    if (j == i) continue;
                    const float ex = st[j].x - sx, ez = st[j].z - sz;
                    if (ex * ex + ez * ez < 2.5f * 2.5f) clear = false;
                }
                if (!clear) continue;
                best = d; pick = &st[i]; stand_x = sx; stand_z = sz;
                break;
            }
        }
        if (!pick) {
            std::fprintf(f, "VOID RUN: no fellable tree with room to stand near spawn (%zu standing)\n", st.size());
            std::fclose(f);
            return 1;
        }
        const worldgen::TreeInstance tree = *pick;
        float base[3], height, trunk_r;
        int need = 0;
        g_felling.tree_info(tree.id, base, height, trunk_r, need);
        std::fprintf(f, "tree %08x, species %u, %.1f m tall, trunk radius %.2f m, at (%.1f, %.1f, %.1f)\n",
                     tree.id, unsigned(tree.species), height, trunk_r, base[0], base[1], base[2]);

        g_physics.place_character(stand_x, wg.height(stand_x, stand_z) + 0.3f, stand_z);
        g_physics.character_position(g_feet);
        // Face the trunk.
        g_camera.yaw = std::atan2(tree.x - stand_x, tree.z - stand_z);
        g_camera.pitch = -0.1f;

        // What physics has between the character and the trunk, before any
        // walking: a ray from the standing point at knee and chest height.
        for (int i = 0; i < 3; ++i) { g_felling.update(g_feet[0], g_feet[2]); frame(0.0f); }
        for (int k = 0; k < 2; ++k) {
            const float hy = k == 0 ? 0.5f : 1.3f;
            const float from[3] = {stand_x, wg.height(stand_x, stand_z) + hy, stand_z};
            float d[3] = {tree.x - from[0], 0.0f, tree.z - from[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            d[0] /= dl; d[2] /= dl;
            int prop = 0;
            const float hit = g_physics.probe_ray(from, d, 4.0f, prop);
            std::fprintf(f, "probe at %.1f m: %s at %.2f m (trunk axis %.2f m away, bark at %.2f)\n", hy,
                         hit < 0.0f ? "NOTHING" : (prop ? "a prop" : "terrain"), hit, dl, dl - trunk_r);
        }

        QueryPerformanceCounter(&t0);
        tp = t0;
        float min_axis = 1e9f;
        int max_prop_contacts = 0, max_contacts = 0;
        unsigned long long grounded_frames = 0;
        bool grounded_at_walk = false;
        int swings = 0;
        bool felled = false;
        double felled_at = -1.0;
        float max_tilt = 0.0f, mid_sink = 0.0f;
        bool rested = false;
        double rested_at = -1.0;
        unsigned long long frames = 0;
        size_t colliders_seen = 0;
        double next_swing = 4.5;
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - t0.QuadPart) / double(tf.QuadPart);
            double dt = double(now.QuadPart - tp.QuadPart) / double(tf.QuadPart);
            tp = now;
            if (dt > 0.1) dt = 0.1;
            if (total >= 19.0 || (felled && total - felled_at > 12.0)) break;

            g_felling.update(g_feet[0], g_feet[2]);
            if (g_felling.colliders() > colliders_seen) colliders_seen = g_felling.colliders();
            float vx = 0.0f, vz = 0.0f;
            if (total >= 2.45 && total < 2.5) grounded_at_walk = g_physics.character_grounded();
            if (total >= 2.5 && total < 4.0) {
                // Into the trunk at walking pace.
                const float ex = tree.x - g_feet[0], ez = tree.z - g_feet[2];
                const float el = std::sqrt(ex * ex + ez * ez);
                if (el > 1e-3f) { vx = ex / el * 3.0f; vz = ez / el * 3.0f; }
            }
            sync_sea(float(total));
            g_physics.update(float(dt), vx, vz, false, 0.0f);
            if (g_physics.character_grounded()) ++grounded_frames;
            g_physics.character_position(g_feet);
            if (total >= 2.5 && total < 4.0) {
                const float ex = tree.x - g_feet[0], ez = tree.z - g_feet[2];
                const float ax = std::sqrt(ex * ex + ez * ez);
                if (ax < min_axis) min_axis = ax;
                int ct = 0, cp = 0;
                g_physics.character_contacts(ct, cp);
                if (cp > max_prop_contacts) max_prop_contacts = cp;
                if (ct > max_contacts) max_contacts = ct;
            }
            g_camera.x = g_feet[0];
            g_camera.y = g_feet[1] + kEyeAboveFeet;
            g_camera.z = g_feet[2];

            if (!felled && total >= next_swing && swings < 40) {
                next_swing = total + 0.2;
                const float eye[3] = {g_feet[0], g_feet[1] + kEyeAboveFeet, g_feet[2]};
                float d[3] = {tree.x - eye[0], tree.y + 1.0f - eye[1], tree.z - eye[2]};
                const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int k = 0; k < 3; ++k) d[k] /= dl;
                uint32_t id = 0;
                const int r = g_felling.chop(eye, d, float(total), &id);
                if (r > 0 && id == tree.id) ++swings;
                if (r == 2 && id == tree.id) { felled = true; felled_at = total; }
            }
            if (felled) {
                float pos[3], rot[4];
                bool active = true;
                if (g_felling.log_pose(tree.id, pos, rot, active)) {
                    // The log's own up axis, and how far it is from vertical.
                    const float up_y = 1.0f - 2.0f * (rot[0] * rot[0] + rot[2] * rot[2]);
                    const float tilt = std::acos(std::fmax(-1.0f, std::fmin(1.0f, up_y))) * 57.29578f;
                    if (tilt > max_tilt) max_tilt = tilt;
                    // Midpoint of the trunk, against the ground under it.
                    const float ux = 2.0f * (rot[0] * rot[1] - rot[3] * rot[2]);
                    const float uz = 2.0f * (rot[1] * rot[2] + rot[3] * rot[0]);
                    const float half = 0.35f * height;
                    const float mx = pos[0] + ux * half, my = pos[1] + up_y * half, mz = pos[2] + uz * half;
                    const float sink = wg.height(mx, mz) - (my + trunk_r * 0.0f);
                    if (total - felled_at > 3.0 && sink > mid_sink) mid_sink = sink;
                    if (!active && !rested) { rested = true; rested_at = total - felled_at; }
                }
            }
            if (frame(float(total))) ++frames;
        }

        float pos[3] = {0, 0, 0}, rot[4] = {0, 0, 0, 1};
        bool active = true;
        g_felling.log_pose(tree.id, pos, rot, active);
        bool standing = false;
        {
            const std::vector<worldgen::TreeInstance>& s2 = g_renderer.tree_field().standing();
            for (size_t i = 0; i < s2.size(); ++i) if (s2[i].id == tree.id) standing = true;
        }
        const float stop_at = min_axis;
        const float bark = trunk_r * 0.9f + 0.35f;
        const bool blocked = stop_at > bark - 0.08f;
        if (frames == 0) std::fprintf(f, "VOID RUN: no frames rendered.\n");
        std::fprintf(f, "\n1. walking into the trunk: closest approach of the feet to its axis %.2f m,\n"
                        "   bark plus capsule %.2f m: %s  (%zu trunk colliders round the player)\n",
                     stop_at, bark, blocked ? "BLOCKED -- collision holds" : "WALKED THROUGH", colliders_seen);
        std::fprintf(f, "   character contacts while walking: up to %d, of them props %d; standing when\n"
                        "   the walk began: %s; frames on the ground %llu\n",
                     max_contacts, max_prop_contacts, grounded_at_walk ? "yes" : "NO -- a void walk",
                     grounded_frames);
        std::fprintf(f, "2. swings to fell: %d of %d needed: %s\n", swings, need, felled ? "felled" : "NOT FELLED");
        std::fprintf(f, "3. the log: tipped to %.0f degrees, %s%s, deepest a trunk midpoint went below the\n"
                        "   ground %.2f m (negative: it lies on it)\n",
                     max_tilt, rested ? "came to rest after " : "STILL MOVING after 12 s",
                     rested ? "" : "", mid_sink);
        if (rested) std::fprintf(f, "   rested %.1f s after the last swing\n", rested_at);
        std::fprintf(f, "   still standing in the tree field: %s\n", standing ? "YES -- FAIL" : "no");
        std::fprintf(f, "frames %llu, felled trees %zu, physics props %zu\n", frames, g_felling.felled(),
                     g_physics.prop_count());
        const bool pass = grounded_at_walk && blocked && felled && max_tilt > 60.0f && rested &&
                          mid_sink < 0.6f && !standing;
        if (!pass) verdict_fail = 1;

        if (do_save) {
            std::string err;
            const bool ok = g_felling.save("build/trees.bin", wg.field_id(), err);
            if (FILE* pf = std::fopen("build/fell_pose.txt", "w")) {
                std::fprintf(pf, "%u %.6f %.6f %.6f %.7f %.7f %.7f %.7f\n", tree.id, pos[0], pos[1], pos[2],
                             rot[0], rot[1], rot[2], rot[3]);
                std::fclose(pf);
            }
            std::fprintf(f, "saved to build/trees.bin: %s\n", ok ? "ok" : err.c_str());
        }

        // A picture of it: from 10 m back, looking at the log's middle.
        {
            const float ux = 2.0f * (rot[0] * rot[1] - rot[3] * rot[2]);
            const float uz = 2.0f * (rot[1] * rot[2] + rot[3] * rot[0]);
            const float mx = pos[0] + ux * 0.4f * height, mz = pos[2] + uz * 0.4f * height;
            engine::Camera c = g_camera;
            c.x = mx - 9.0f; c.z = mz - 9.0f;
            c.y = wg.height(c.x, c.z) + 3.5f;
            c.yaw = std::atan2(mx - c.x, mz - c.z);
            c.pitch = -0.22f;
            RECT cr;
            GetClientRect(hwnd, &cr);
            engine::ViewConstants v = {};
            fill_view(c, float(cr.right - cr.left), float(cr.bottom - cr.top), 20.0f, v);
            std::string err;
            g_renderer.capture(v, "build/fell.bmp", err);
        }
        std::fprintf(f, "\n%s\n", pass ? "PASS" : "FAIL");
        std::fclose(f);
        g_renderer.shutdown();
        g_physics.shutdown();
        return verdict_fail;
    }

    if (std::string(cmdline ? cmdline : "").find("--tab-test") != std::string::npos) {
        // --tab-test: what pressing Tab does in the window. Fly for 4 s with
        // the window loop's physics step running as it does in fly mode,
        // then drop the character in where the camera is -- exactly the Tab
        // handler -- and let it stand for 6 s. A character that ends up
        // under the ground fell through it.
        // Uncapped, so the fly phase's frame time says what the per-frame
        // terrain sync costs while flying.
        g_renderer.set_vsync(false);
        g_walk = false;
        const engine::WorldGpu& wg = g_renderer.world();
        double fly_ms_sum = 0.0;
        unsigned long long fly_frames = 0;
        g_camera.x = g_spawn.x; g_camera.z = g_spawn.z;
        g_camera.y = wg.height(g_camera.x, g_camera.z) + 40.0f;
        g_camera.yaw = 1.0f; g_camera.pitch = -0.3f;

        LARGE_INTEGER tf, t0, tp;
        QueryPerformanceFrequency(&tf);
        QueryPerformanceCounter(&t0);
        tp = t0;
        bool dropped = false;
        float drop_y = 0.0f, ground_at_drop = 0.0f;
        size_t bodies_at_drop = 0;
        float feet_min = 1e9f;
        unsigned long long frames = 0, grounded_frames = 0;
        for (;;) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double total = double(now.QuadPart - t0.QuadPart) / double(tf.QuadPart);
            double dt = double(now.QuadPart - tp.QuadPart) / double(tf.QuadPart);
            tp = now;
            if (dt > 0.1) dt = 0.1;
            if (total >= 10.0) break;

            if (!dropped && total >= 4.0) {
                // The Tab handler, verbatim.
                g_walk = true;
                g_physics.place_character(g_camera.x, g_camera.y, g_camera.z);
                dropped = true;
                drop_y = g_camera.y;
                ground_at_drop = wg.height(g_camera.x, g_camera.z);
                bodies_at_drop = g_physics.body_count();
            }
            if (!g_walk) {
                // Flying: forward at 30 m/s, as a person would before landing.
                g_camera.x += std::sin(g_camera.yaw) * 30.0f * float(dt);
                g_camera.z += std::cos(g_camera.yaw) * 30.0f * float(dt);
                step_window_physics(float(dt), float(total), 0.0f, 0.0f, false, 0.0f);
                if (total > 1.0) { fly_ms_sum += dt * 1000.0; ++fly_frames; }
            } else {
                step_window_physics(float(dt), float(total), 0.0f, 0.0f, false, 0.0f);
                g_camera.x = g_feet[0];
                g_camera.y = g_feet[1] + kEyeAboveFeet;
                g_camera.z = g_feet[2];
                if (g_feet[1] < feet_min) feet_min = g_feet[1];
                if (g_physics.character_grounded()) ++grounded_frames;
            }
            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) continue;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, float(total), v);
            g_renderer.render(v);
            ++frames;
        }
        const float ground_end = wg.height(g_feet[0], g_feet[2]);
        const bool fell = g_feet[1] < ground_end - 2.0f;
        if (FILE* f = std::fopen("build/tab_test.txt", "w")) {
            if (frames == 0) std::fprintf(f, "VOID RUN: no frames rendered.\n\n");
            std::fprintf(f, "fly 4 s at 30 m/s, press Tab, stand 6 s\n");
            std::fprintf(f, "frames %llu; flying, uncapped: mean %.2f ms over %llu frames\n",
                         frames, fly_frames ? fly_ms_sum / double(fly_frames) : 0.0, fly_frames);
            std::fprintf(f, "dropped from y %.2f over ground %.2f, with %zu terrain bodies in physics\n",
                         drop_y, ground_at_drop, bodies_at_drop);
            std::fprintf(f, "feet at the end %.2f, ground there %.2f, lowest feet %.2f\n",
                         g_feet[1], ground_end, feet_min);
            std::fprintf(f, "frames standing on something: %llu\n", grounded_frames);
            std::fprintf(f, "fell through the world: %s\n", fell ? "YES" : "no");
            std::fclose(f);
        }
        g_renderer.shutdown();
        g_physics.shutdown();
        return fell ? 1 : 0;
    }

    if (seabed_mode) {
        // --seabed-tunnel: ADR-006 stage 1. The sea was wherever the
        // GENERATED column is below sea level, so a character in a sealed
        // tunnel under the seabed swam in dry air. Finds the coast nearest
        // spawn, digs a sealed tunnel from under the beach out under the
        // shallows, and a pit in the seabed open to the sea beside it, then
        // stands the character in each place in turn:
        //   A. three points in the tunnel, under sea columns: must stay DRY,
        //      on its feet, not floated up to the roof;
        //   B. open water over the tunnel's far end: must be in the sea;
        //   C. the bottom of the open pit: must be in the sea.
        // BIOMES_SEA_COLUMN_ONLY=1 is the mutant (the old rule): A must FAIL.
        const engine::WorldGpu& wg = g_renderer.world();
        const float sea = wg.params().sea_level;
        g_renderer.set_vsync(true);
        g_walk = true;
        std::remove("build/seabed_tunnel.txt");
        FILE* f = std::fopen("build/seabed_tunnel.txt", "w");
        if (!f) return 1;
        const bool mutant = std::getenv("BIOMES_SEA_COLUMN_ONLY") != nullptr;

        // The coast: the nearest point to spawn along any of 72 spokes where
        // land gives way to sea that stays sea for 30 m and is at least
        // 1.5 m deep 25 m out.
        float best_rc = 1e9f, dx = 0.0f, dz = 1.0f;
        for (int sp = 0; sp < 72; ++sp) {
            const float a = 6.2831853f * float(sp) / 72.0f;
            const float ux = std::sin(a), uz = std::cos(a);
            auto at = [&](float r) { return wg.height(g_spawn.x + ux * r, g_spawn.z + uz * r); };
            float rc = -1.0f;
            for (float r = 10.0f; r < 4000.0f && r < best_rc; r += 2.0f)
                if (at(r) < sea) { rc = r; break; }
            if (rc < 0.0f || at(rc - 10.0f) < sea) continue;
            bool ok = true;
            for (float s = 3.0f; s <= 30.0f && ok; s += 1.0f) ok = at(rc + s) < sea - 0.3f;
            if (!ok || at(rc + 25.0f) > sea - 1.5f) continue;
            best_rc = rc; dx = ux; dz = uz;
        }
        if (best_rc >= 1e9f) {
            std::fprintf(f, "VOID RUN: no coast within 4 km of spawn with 30 m of sea 1.5 m deep\n");
            std::fclose(f);
            g_renderer.shutdown();
            g_physics.shutdown();
            return 1;
        }
        auto pt = [&](float s, float side, float out[2]) {
            out[0] = g_spawn.x + dx * (best_rc + s) - dz * side;
            out[1] = g_spawn.z + dz * (best_rc + s) + dx * side;
        };

        // The tunnel: 4 m across, from 10 m inland to 30 m out, its roof at
        // least 2.5 m under the lowest seabed it passes beneath.
        const float kR = 2.0f, kRoof = 2.5f;
        float hmin = 1e9f;
        for (float s = -10.0f; s <= 30.0f; s += 1.0f) {
            float p[2];
            pt(s, 0.0f, p);
            hmin = (std::min)(hmin, wg.height(p[0], p[1]));
        }
        const float yc = hmin - kRoof - kR;
        // The pit: 8 m to one side, half under the seabed, open to the sea.
        float pit[2];
        pt(15.0f, 8.0f, pit);
        if (wg.height(pit[0], pit[1]) > sea - 0.5f) pt(15.0f, -8.0f, pit);
        const float pit_r = 2.5f, pit_y = wg.height(pit[0], pit[1]);

        float t = 0.0f;
        const float kDt = 1.0f / 60.0f;
        auto frame = [&]() -> bool {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            sync_physics_terrain();
            sync_sea(t);
            g_physics.update(kDt, 0.0f, 0.0f, false);
            g_physics.character_position(g_feet);
            g_camera.x = g_feet[0]; g_camera.y = g_feet[1] + kEyeAboveFeet; g_camera.z = g_feet[2];
            t += kDt;
            RECT cr;
            GetClientRect(hwnd, &cr);
            const float w = float(cr.right - cr.left), h = float(cr.bottom - cr.top);
            if (w < 1.0f || h < 1.0f) return false;
            engine::ViewConstants v = {};
            fill_view(g_camera, w, h, t, v);
            g_renderer.render(v);
            return true;
        };

        // Stream the place in from the beach above the tunnel's land end.
        {
            float p[2];
            pt(-10.0f, 0.0f, p);
            g_physics.place_character(p[0], Spawn::above(p[0], p[1]), p[1]);
        }
        int drawn = 0;
        for (int i = 0; i < 120; ++i) drawn += frame() ? 1 : 0;

        for (float s = -10.0f; s <= 30.0f; s += 1.0f) {
            float p[2];
            pt(s, 0.0f, p);
            g_renderer.dig(p[0], yc, p[1], kR, -4.0f, 1);
        }
        g_renderer.dig(pit[0], pit_y, pit[1], pit_r, -4.0f, 1);

        // Until every tunnel chunk physics holds is a MESH shape: before
        // that a chunk is a heightfield, solid to its top surface, and a
        // character placed in the tunnel would be pushed up onto the seabed.
        const float kS[3] = {5.0f, 15.0f, 25.0f};
        const float S0 = engine::lod_chunk_size(0);
        auto key_at = [&](float x, float y, float z) {
            engine::ChunkKey k;
            k.x = int(std::floor(x / S0)); k.y = int(std::floor(y / S0));
            k.z = int(std::floor(z / S0)); k.lod = 0;
            return k;
        };
        int mesh_frames = -1;
        for (int i = 1; i <= 900 && mesh_frames < 0; ++i) {
            drawn += frame() ? 1 : 0;
            bool all = true;
            for (int k = 0; k < 3 && all; ++k) {
                float p[2];
                pt(kS[k], 0.0f, p);
                all = g_physics.chunk_shape(key_at(p[0], yc, p[1])) == 2;
            }
            if (all && g_physics.chunk_shape(key_at(pit[0], pit_y - pit_r, pit[1])) == 2) mesh_frames = i;
        }

        std::fprintf(f, "seabed tunnel%s\n", mutant ? " -- MUTANT: BIOMES_SEA_COLUMN_ONLY, the old column rule" : "");
        std::fprintf(f, "coast %.0f m from spawn (%.1f, %.1f) along (%.3f, %.3f); sea level %.2f\n",
                     best_rc, g_spawn.x, g_spawn.z, dx, dz, sea);
        std::fprintf(f, "tunnel: radius %.1f m, centre y %.2f, from 10 m inland to 30 m out; "
                        "lowest seabed over it %.2f (roof >= %.1f m under it)\n", kR, yc, hmin, kRoof);
        std::fprintf(f, "pit: radius %.1f m at the seabed (%.1f, %.2f, %.1f)\n", pit_r, pit[0], pit_y, pit[1]);
        std::fprintf(f, "physics mesh shapes in place after %d frame(s)%s\n\n", mesh_frames,
                     mesh_frames < 0 ? " -- NEVER: the cases below may stand on heightfields" : "");

        // One case: place, run 1.5 s, count what physics called it.
        struct Case { int dry = 0, wade = 0, swim = 0, frames = 0, eye_under = 0; float rise = 0.0f;
                      bool grounded = false; unsigned long long calls = 0; double ms = 0.0; };
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);
        auto run = [&](float x, float y, float z) {
            Case c;
            g_physics.place_character(x, y, z);
            const unsigned long long c0 = g_cover_calls, k0 = g_cover_ticks;
            float settled = 1e9f;
            for (int i = 0; i < 90; ++i) {
                drawn += frame() ? 1 : 0;
                if (i < 15) { settled = g_feet[1]; continue; }   // falls the last few cm
                ++c.frames;
                const engine::Medium md = g_physics.medium();
                if (md == engine::Medium::Dry) ++c.dry;
                else if (md == engine::Medium::Wading) ++c.wade;
                else ++c.swim;
                c.rise = (std::max)(c.rise, g_feet[1] - settled);
                // The renderer's own verdict on the eye (ADR-006 stage 2).
                if (g_renderer.camera_underwater()) ++c.eye_under;
            }
            c.grounded = g_physics.character_grounded();
            c.calls = g_cover_calls - c0;
            c.ms = double(g_cover_ticks - k0) * 1000.0 / double(qf.QuadPart);
            return c;
        };
        auto line = [&](const char* name, const Case& c, float x, float z) {
            std::fprintf(f, "%-24s generated column %+6.2f m (%s); dry %d, wading %d, swimming %d of %d; "
                            "rose %.2f m; %s; drawn as underwater %d; cover test %llu calls, %.1f us each\n",
                         name, wg.height(x, z) - sea, wg.height(x, z) < sea ? "sea by the old rule" : "land",
                         c.dry, c.wade, c.swim, c.frames, c.rise, c.grounded ? "on its feet" : "NOT grounded",
                         c.eye_under, c.calls, c.calls ? c.ms * 1000.0 / double(c.calls) : 0.0);
        };

        bool pass_a = mesh_frames > 0, pass_b = false, pass_c = false;
        for (int k = 0; k < 3; ++k) {
            float p[2];
            pt(kS[k], 0.0f, p);
            const Case c = run(p[0], yc - kR + 0.3f, p[1]);
            char name[64];
            std::snprintf(name, sizeof(name), "A. tunnel, %2.0f m out:", kS[k]);
            line(name, c, p[0], p[1]);
            if (c.dry != c.frames || !c.grounded || c.rise > 0.3f || c.eye_under > 0) pass_a = false;
        }
        {
            float p[2];
            pt(25.0f, 0.0f, p);
            const float bed = wg.height(p[0], p[1]);
            const Case c = run(p[0], (std::max)(bed + 0.3f, sea - engine::PhysicsWorld::kFloatDepth), p[1]);
            line("B. open water over it:", c, p[0], p[1]);
            pass_b = c.frames > 0 && c.dry == 0;
        }
        {
            const Case c = run(pit[0], pit_y - pit_r + 0.3f, pit[1]);
            line("C. open pit in the bed:", c, pit[0], pit[1]);
            pass_c = c.frames > 0 && c.dry == 0;
        }

        // D. The eye itself under open water, over the tunnel's far end: the
        // renderer must draw the view as underwater (stage 2), and the still
        // looks back up the slope at the surface from below.
        bool pass_d = false;
        float d_depth = 0.0f;
        {
            float p[2];
            pt(25.0f, 0.0f, p);
            const float bed = wg.height(p[0], p[1]);
            engine::Camera c = g_camera;
            c.x = p[0]; c.z = p[1]; c.y = 0.5f * (sea + bed);
            c.yaw = std::atan2(-dx, -dz);
            c.pitch = 0.25f;
            int under = 0;
            for (int i = 0; i < 8; ++i) {
                sync_sea(t);
                engine::ViewConstants cv = {};
                fill_view(c, g_res_w, g_res_h, t, cv);
                g_renderer.render(cv);
                t += kDt;
                if (g_renderer.camera_underwater()) ++under;
            }
            d_depth = g_renderer.camera_water_depth();
            pass_d = under == 8;
            std::fprintf(f, "D. eye under open water:  camera (%.1f, %.2f, %.1f) over a %.2f m column; drawn as "
                            "underwater %d of 8 frames, %.2f m under the surface\n",
                         c.x, c.y, c.z, sea - bed, under, d_depth);
            engine::ViewConstants cv = {};
            fill_view(c, g_res_w, g_res_h, t, cv);
            std::string e3;
            g_renderer.capture(cv, "build/seabed_under.bmp", e3);
            std::fprintf(f, "  still: build/seabed_under.bmp -- --cam %.2f %.2f %.2f %.4f %.2f\n",
                         c.x, c.y, c.z, c.yaw, c.pitch);
        }

        // A still down the tunnel, seaward, from its 5 m point.
        {
            float p[2];
            pt(5.0f, 0.0f, p);
            engine::Camera c = g_camera;
            c.x = p[0]; c.z = p[1]; c.y = yc + 0.3f;
            c.yaw = std::atan2(dx, dz);
            c.pitch = 0.0f;
            engine::ViewConstants cv = {};
            fill_view(c, g_res_w, g_res_h, t, cv);
            std::string e3;
            g_renderer.capture(cv, "build/seabed_tunnel.bmp", e3);
            std::fprintf(f, "\nstill: build/seabed_tunnel.bmp -- --cam %.2f %.2f %.2f %.4f 0\n", c.x, c.y, c.z, c.yaw);
        }

        const engine::FrameStats st = g_renderer.stats();
        if (drawn == 0 || st.chunks_classified == 0)
            std::fprintf(f, "\nVOID RUN: %d frames drawn, %llu chunks classified\n", drawn, st.chunks_classified);
        std::fprintf(f, "\nA (tunnel dry, standing, not drawn as underwater): %s\nB (open water is sea): %s\n"
                        "C (open pit is sea): %s\nD (an eye under open water is drawn as underwater): %s\n",
                     pass_a ? "PASS" : "FAIL", pass_b ? "PASS" : "FAIL", pass_c ? "PASS" : "FAIL",
                     pass_d ? "PASS" : "FAIL");
        const bool pass = pass_a && pass_b && pass_c && pass_d && drawn > 0;
        std::fprintf(f, "%s\n", pass ? "PASS" : "FAIL");
        std::fclose(f);
        g_renderer.shutdown();
        g_physics.shutdown();
        return pass ? 0 : 1;
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
        g_renderer.update_weather(0.0);
        const float state = g_renderer.sea_state();
        // The weather's sea. Under --sea-state the dial is fixed and the sea
        // with it (only the swell's heading wanders, slowly).
        const WwSea& wsea = g_renderer.sea();
        auto surf_at = [&](float x, float z, float t) {
            return ww_water_at(x, z, t, wsea, g_renderer.swash(), sea - wg.height(x, z));
        };
        // "Deep" is deep enough that no trough at this sea state can put the
        // floating feet on the bottom: float depth, plus the full amplitude
        // the trough can fall, plus a metre. Below this, touching the seabed
        // while swimming is legitimate and is not counted.
        const float deep = engine::PhysicsWorld::kFloatDepth +
                           ww_total_amplitude(wsea) + 1.0f;

        // A beach: the nearest coast to SPAWN along any of 72 spokes where
        // the land side is low enough to walk down (under 6 m, 20 m in) and
        // the sea side stays sea and gets deep within 40 m. From the origin
        // until v19: v18's archipelago put the origin in the sea, and every
        // run since found no beach and swam nothing.
        const float ox = g_spawn.x, oz = g_spawn.z;
        auto hgt = [&](float dx, float dz, float r) { return wg.height(ox + dx * r, oz + dz * r); };
        float best_rc = 1e9f, dir_x = 0.0f, dir_z = 1.0f;
        for (int sp = 0; sp < 72; ++sp) {
            const float a = 6.2831853f * float(sp) / 72.0f;
            const float dx = std::sin(a), dz = std::cos(a);
            float rc = -1.0f;
            for (float r = 20.0f; r < 6000.0f && r < best_rc; r += 5.0f) {
                if (hgt(dx, dz, r) < sea) { rc = r; break; }
            }
            if (rc < 0.0f) continue;
            const float land = hgt(dx, dz, rc - 20.0f);
            if (land < sea || land > sea + 6.0f) continue;
            bool open = true;
            for (float r = rc; r <= rc + 70.0f; r += 5.0f) {
                if (hgt(dx, dz, r) >= sea) { open = false; break; }
            }
            if (!open) continue;
            if (sea - hgt(dx, dz, rc + 40.0f) < deep) continue;
            best_rc = rc; dir_x = dx; dir_z = dz;
        }

        FILE* f = std::fopen("build/swim.txt", "w");
        if (best_rc >= 1e9f) {
            if (f) {
                std::fprintf(f, "no beach found within 6 km of spawn that is walkable on\n"
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
        const float sx = ox + dir_x * (best_rc - 20.0f), sz = oz + dir_z * (best_rc - 20.0f);
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
            // The character swims too (it is drawn only in the second still).
            update_character(float(dt), float(total), sy * spd, cy * spd);

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

            const double along = double(g_feet[0] - ox) * dir_x + double(g_feet[2] - oz) * dir_z;
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
                    surf_at(g_feet[0], g_feet[2], float(total));
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
                surf_at(g_camera.x, g_camera.z, float(total)));
            const bool trough_sea = ww_total_amplitude(wsea) > 1.5f;
            const bool vantage = md == engine::Medium::Swimming && eye_local > 0.2 &&
                                 (!trough_sea || double(g_camera.y) < double(sea) - 1.0);
            if (back && !shot_taken && (vantage || total >= swim_seconds * 0.8)) {
                shot_taken = true;
                engine::ViewConstants cv = {};
                fill_view(g_camera, g_res_w, g_res_h, float(total), cv);
                g_renderer.set_mode(engine::Mode::Mesh);
                std::string err;
                g_renderer.capture(cv, "build/swim.bmp", err);
                shot_eye_local = double(g_camera.y) - (double(sea) +
                    surf_at(g_camera.x, g_camera.z, float(total)));
                shot_eye_sea = double(g_camera.y) - double(sea);
                // And the same moment in third person: the swimmer, through
                // the window's camera, 4.5 m back and a little above.
                {
                    const engine::Camera keep = g_camera;
                    g_walk = true;
                    g_cam_dist = g_cam_dist_now = 4.5f;
                    g_camera.pitch = -0.3f;
                    place_walk_camera(0.0f);
                    publish_character();
                    engine::ViewConstants tv = {};
                    fill_view(g_camera, g_res_w, g_res_h, float(total), tv);
                    g_renderer.capture(tv, "build/swim_char.bmp", err);
                    g_renderer.character().set_pose(g_bones, false);
                    g_camera = keep;
                }
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
            std::fprintf(f, "beach: coast %.0f m from spawn along (%.3f, %.3f), "
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
                         deep, engine::PhysicsWorld::kFloatDepth, ww_total_amplitude(wsea));
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

    if (std::string(cmdline ? cmdline : "").find("--pose-sheet") != std::string::npos) {
        // --pose-sheet: the character in fixed poses from fixed cameras, to
        // judge the procedural animation without playing. Stands it at spawn,
        // lets the world and the capsule settle, then for each pose drives
        // the animator to it and writes build/pose_<name>.bmp, and a line
        // per pose to build/pose_sheet.txt.
        g_walk = true;
        g_cam_dist = g_cam_dist_now = 4.5f;
        const float sx = 0.0f, sz = 0.0f;
        g_physics.place_character(sx, Spawn::above(sx, sz), sz);
        g_physics.character_position(g_feet);
        for (int i = 0; i < 240; ++i) {
            MSG m = {};
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            step_window_physics(1.0f / 60.0f, float(i) / 60.0f, 0.0f, 0.0f, false, 0.0f);
            g_camera.x = g_feet[0] - 4.0f; g_camera.y = g_feet[1] + 1.6f; g_camera.z = g_feet[2];
            engine::ViewConstants v = {};
            fill_view(g_camera, g_res_w, g_res_h, float(i) / 60.0f, v);
            g_renderer.render(v);
        }
        // speed: m/s; air / swim: the medium; action: seconds into the axe's
        // swing, -1 none.
        struct Pose { const char* name; float speed; bool air, swim, torch; float action; float phase;
                      float cam_yaw; float cam_pitch; float dist; float pivot; };
        // Camera yaw: the character faces +z (yaw 0); -pi/2 looks along -x,
        // which is from the character's left side; pi looks back at its front.
        const float side = -1.5707963f, front = 3.1415927f, behind = 0.35f;
        const Pose poses[] = {
            {"idle_front", 0.0f, false, false, false, -1.0f, 0.0f, front, -0.05f, 3.2f, 1.0f},
            {"idle_side", 0.0f, false, false, false, -1.0f, 0.0f, side, -0.05f, 3.2f, 1.0f},
            {"walk_0", 1.4f, false, false, false, -1.0f, 0.0f, side, -0.05f, 3.2f, 1.0f},
            {"walk_50", 1.4f, false, false, false, -1.0f, 0.5f, side, -0.05f, 3.2f, 1.0f},
            {"jog_0", 5.0f, false, false, false, -1.0f, 0.0f, side, -0.05f, 3.4f, 1.0f},
            {"jog_25", 5.0f, false, false, false, -1.0f, 0.25f, side, -0.05f, 3.4f, 1.0f},
            {"sprint_0", 8.0f, false, false, false, -1.0f, 0.0f, side, -0.05f, 3.4f, 1.0f},
            {"sprint_25", 8.0f, false, false, false, -1.0f, 0.25f, side, -0.05f, 3.4f, 1.0f},
            {"jog_front", 5.0f, false, false, false, -1.0f, 0.1f, front, -0.05f, 3.4f, 1.0f},
            {"jump", 0.0f, true, false, false, -1.0f, 0.0f, side, -0.05f, 3.4f, 1.0f},
            {"swim_idle", 0.0f, false, true, false, -1.0f, 0.0f, side, -0.05f, 3.4f, 1.0f},
            {"swim_fwd", 2.0f, false, true, false, -1.0f, 0.3f, side, 0.1f, 3.4f, 1.0f},
            {"torch_idle", 0.0f, false, false, true, -1.0f, 0.0f, front, -0.05f, 3.2f, 1.0f},
            {"torch_jog", 5.0f, false, false, true, -1.0f, 0.3f, side, -0.05f, 3.4f, 1.0f},
            {"chop", 0.0f, false, false, false, 0.55f, 0.0f, front, -0.05f, 3.2f, 1.0f},
            {"third_person", 5.0f, false, false, false, -1.0f, 0.6f, behind, -0.2f, 4.5f, kCamPivot},
        };
        FILE* rf = std::fopen("build/pose_sheet.txt", "w");
        if (rf) std::fprintf(rf, "%s\n%s\nfeet at (%.2f, %.2f, %.2f)\n", g_renderer.character().note().c_str(),
                             g_anim.clips() ? g_anim_lib.note.c_str() : "no clips: the procedural gait",
                             g_feet[0], g_feet[1], g_feet[2]);
        for (const Pose& p : poses) {
            engine::LocomotionInput in;
            in.dt = 0.05f;
            in.speed = p.speed;
            in.grounded = !p.air && !p.swim;
            in.swimming = p.swim;
            in.torch = p.torch;
            for (int i = 0; i < 60; ++i) {
                in.time = float(i) * in.dt;
                g_anim.set_phase(p.phase);
                g_anim.update(in);
            }
            g_anim.set_phase(p.phase);
            g_anim.set_action_time(p.action);
            in.dt = 0.0f;
            g_anim.update(in);
            g_char_yaw = 0.0f;
            publish_character();
            engine::Camera c = g_camera;
            c.yaw = p.cam_yaw;
            c.pitch = p.cam_pitch;
            float f[3], r[3], u[3];
            c.basis(f, r, u);
            c.x = g_feet[0] - f[0] * p.dist;
            c.y = g_feet[1] + p.pivot - f[1] * p.dist;
            c.z = g_feet[2] - f[2] * p.dist;
            engine::ViewConstants v = {};
            fill_view(c, g_res_w, g_res_h, 0.0f, v);
            std::string err;
            const std::string path = std::string("build/pose_") + p.name + ".bmp";
            g_renderer.capture(v, path.c_str(), err);
            if (rf) std::fprintf(rf, "%-13s speed %.1f %s phase %.2f: run %.2f air %.2f drop %.3f m%s%s\n", p.name,
                                 p.speed, p.swim ? "swim  " : (p.air ? "air   " : "ground"), g_anim.phase(), g_anim.run_blend(),
                                 g_anim.air_blend(), g_anim.foot_drop(), err.empty() ? "" : " -- ", err.c_str());
        }
        if (rf) std::fclose(rf);
        g_renderer.shutdown();
        g_physics.shutdown();
        return 0;
    }

    if (walk_seconds > 0.0) {
        // --uncapped measures frame cost at ground level, where the grass is:
        // a flight passes too high for blades to be emitted at all.
        const bool walk_uncapped = std::string(cmdline ? cmdline : "").find("--uncapped") != std::string::npos;
        g_renderer.set_vsync(!walk_uncapped);
        std::vector<double> walk_ms;
        g_walk = true;
        g_physics.place_character(g_spawn.x, Spawn::above(g_spawn.x, g_spawn.z), g_spawn.z);
        g_physics.character_position(g_feet);
        g_camera.pitch = -0.2f;
        // --cam-dist <m>: the third-person camera's distance; 0 is the old
        // first-person eye, with no character drawn.
        {
            const std::string wcl = cmdline ? cmdline : "";
            const size_t cd = wcl.find("--cam-dist");
            if (cd != std::string::npos) g_cam_dist = float(std::atof(wcl.c_str() + cd + 10));
            g_cam_dist_now = g_cam_dist;
        }
        g_char_yaw = 0.0f;
        g_have_prev_feet = false;

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
            walk_ms.push_back(dt * 1000.0);
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
            // The player as the window has it: the character posed from what
            // physics did, the third-person camera behind it.
            update_character(float(dt), float(total), sy * sp, cy * sp);
            place_walk_camera(float(dt));
            publish_character();

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
            if (walk_ms.size() > 60) {
                std::vector<double> sm(walk_ms.begin() + 60, walk_ms.end());
                std::sort(sm.begin(), sm.end());
                double sum = 0.0;
                for (size_t q = 0; q < sm.size(); ++q) sum += sm[q];
                std::fprintf(f, "frame time (%s, after the first 60): mean %.2f ms, 99th %.2f ms\n",
                             walk_uncapped ? "uncapped" : "vsync", sum / sm.size(),
                             sm[size_t(sm.size() * 0.99)]);
            }
            write_rock_report(f, g_feet[0], g_feet[2], true);
            {
                const engine::NearField& nf = g_renderer.near_field();
                std::fprintf(f, "near field: %s; %llu plans, %llu of them filled blocks (%llu blocks), %llu probes traced\n",
                             g_renderer.near_field_on() ? (nf.ready() ? "on, ready" : "on, NOT ready") : "off",
                             nf.plans(), nf.fill_frames(), nf.blocks_filled(), nf.probes_traced());
            }
            std::fprintf(f, "lightning: %u strikes (%u to the ground), %llu frames lit, seed %u, clock %.1f s at the end\n",
                         g_renderer.strikes_seen(), g_renderer.ground_strikes_seen(), g_renderer.flash_frames(),
                         g_renderer.lightning().seed(), double(g_renderer.weather_time()));
            std::fprintf(f, "frames %llu, travelled %.0f m\n", frames, travelled);
            std::fprintf(f, "character: %s; camera %.1f m behind (wanted %.1f)%s; speed %.2f m/s, gait phase %.2f, "
                            "run %.2f, planted-foot drop %.3f m\n",
                         (g_renderer.character().note() + "; " +
                          (g_anim.clips() ? g_anim_lib.note : std::string("procedural gait"))).c_str(),
                         g_cam_dist_now, g_cam_dist,
                         first_person() ? ", first person" : "", g_char_speed, g_anim.phase(), g_anim.run_blend(),
                         g_anim.foot_drop());
            std::fprintf(f, "canopy map: %s, %llu builds, %u trees in the last\n",
                         g_renderer.canopy() ? "on" : "off", g_renderer.canopy_builds(), g_renderer.canopy_trees());
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

        // Where the walk ended up, through the third-person camera as it was
        // on the last frame: the character mid-stride, and the ground.
        {
            engine::ViewConstants v = {};
            fill_view(g_camera, g_res_w, g_res_h, 0.0f, v);
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
        g_camera.x = g_spawn.x; g_camera.z = g_spawn.z;
        g_camera.y = g_spawn.ground + 10.0f;
        // --flight-from x y z: start somewhere else -- rough ground is where
        // the slot pool ran dry, and the origin is gentle.
        {
            const std::string fc = cmdline ? cmdline : "";
            const size_t ff = fc.find("--flight-from");
            float fx, fy, fz;
            if (ff != std::string::npos &&
                std::sscanf(fc.c_str() + ff + 13, "%f %f %f", &fx, &fy, &fz) == 3) {
                g_camera.x = fx; g_camera.y = fy; g_camera.z = fz;
            }
        }
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
        unsigned long long deficit_sum[engine::kLodCount] = {0, 0, 0, 0, 0, 0, 0, 0};
        unsigned long long budget_sum = 0, budget_n = 0;
        UINT budget_peak = 0;
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
                const size_t* db = g_renderer.streamer().deficit_by_lod();
                for (int L = 0; L < engine::kLodCount; ++L) deficit_sum[L] += db[L];
            }
            if (g_renderer.streamer().held_count() > peak_held)
                peak_held = g_renderer.streamer().held_count();
            {
                const engine::FrameStats bs = g_renderer.stats();
                budget_sum += bs.mesh_budget;
                if (bs.mesh_budget > budget_peak) budget_peak = bs.mesh_budget;
                ++budget_n;
            }
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
            std::fprintf(f, "mesh budget per frame: mean %.1f, peak %u (floor %u, ceiling %u, GPU target %.1f ms)\n",
                         budget_n ? double(budget_sum) / double(budget_n) : 0.0, budget_peak,
                         engine::kMeshBudgetPerFrame, engine::kMeshBudgetMax, engine::kMeshTargetMs);
            std::fprintf(f, "frames with work queued and no free slot: %llu\n",
                         g_renderer.streamer().starved_frames());
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
                // How late the detail is. Latency: frames from a surface chunk
                // first being wanted to it starting to show. Deficit: surface
                // chunks wanted and not yet shown, averaged per frame.
                std::fprintf(f, "detail latency, surface chunks (frames; x16.7 ms at 60 Hz) and mean deficit per frame:\n");
                std::fprintf(f, "  %-5s %6s %7s %6s %6s %9s\n", "LOD", "n", "median", "p90", "max", "deficit");
                for (int L = 0; L < engine::kLodCount; ++L) {
                    std::vector<int> lv = g_renderer.streamer().latency_frames(L);
                    std::sort(lv.begin(), lv.end());
                    const int med = lv.empty() ? 0 : lv[lv.size() / 2];
                    const int p90 = lv.empty() ? 0 : lv[(lv.size() * 9) / 10];
                    const int mx = lv.empty() ? 0 : lv.back();
                    std::fprintf(f, "  %-5d %6zu %7d %6d %6d %9.2f\n", L, lv.size(), med, p90, mx,
                                 judged_frames ? double(deficit_sum[L]) / double(judged_frames) : 0.0);
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

    setup_ui(hwnd);

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
        // The UI runs on the wall clock; the game's clock stops while paused.
        const double wall_dt = dt;
        if (g_paused) dt = 0.0;
        t += static_cast<float>(dt);

        RECT cr;
        GetClientRect(hwnd, &cr);
        const float w = static_cast<float>(cr.right - cr.left);
        const float h = static_cast<float>(cr.bottom - cr.top);
        if (w < 1.0f || h < 1.0f) continue;

        if (g_paused) {
            // Still hand physics what streamed in: the events are this
            // frame's only (see step_window_physics).
            sync_physics_terrain();
        } else if (g_walk) {
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
            // Strafe along Camera::basis's right vector, (-cos yaw, 0, sin yaw),
            // which is what the view matrix puts on screen right. This used
            // (+cos, -sin) -- screen LEFT -- so A and D were swapped in walk
            // mode while fly mode, which uses basis(), had them right.
            const float sy = std::sin(g_camera.yaw), cy = std::cos(g_camera.yaw);
            // A jog; Shift sprints, Caps Lock (toggled) walks. A diagonal is
            // no faster than straight ahead.
            float sp = kJogSpeed;
            if (GetKeyState(VK_CAPITAL) & 1) sp = kWalkSpeed;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) sp = kSprintSpeed;
            const float ml = std::sqrt(mx * mx + mz * mz);
            if (ml > 1.0f) { mx /= ml; mz /= ml; }
            const float vx = (sy * mz - cy * mx) * sp;
            const float vz = (cy * mz + sy * mx) * sp;
            const bool jump = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            // Ctrl dives. Space is both jump and swim-up, so the way out of
            // the water is the key you would already be pressing.
            const float swim = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? -1.0f : 0.0f;
            step_window_physics(float(dt), t, vx, vz, jump, swim);
            update_character(float(dt), t, vx, vz);
        } else {
            step_window_physics(float(dt), t, 0.0f, 0.0f, false, 0.0f);
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
                // Minus dx: increasing yaw turns forward toward +x, and the
                // view's right vector at yaw 0 is -x (Camera::basis), so +yaw
                // turns LEFT on screen. Mouse right must decrease it.
                g_camera.look(-dx * 0.0030f, -dy * 0.0030f);
                POINT back = c;
                ClientToScreen(hwnd, &back);
                SetCursorPos(back.x, back.y);
            }
            if (!g_walk) g_camera.update(static_cast<float>(dt));
        }
        // The camera follows the character after the mouse has turned it, so
        // the orbit never lags the look by a frame.
        if (g_walk && !g_paused) place_walk_camera(float(dt));
        publish_character();

        // Trees: colliders round the player, logs posed by physics; then any
        // swing, save or load the window asked for this frame.
        g_felling.update(g_walk ? g_feet[0] : g_camera.x, g_walk ? g_feet[2] : g_camera.z);
        if (g_want_chop) {
            g_want_chop = false;
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            float eye[3];
            chop_origin(eye);
            // The swing: the character turns to where the camera looks and
            // its upper body plays the axe over whatever the legs are doing.
            if (g_walk) {
                g_char_yaw = g_camera.yaw;
                g_anim.trigger_action();
            }
            uint32_t id = 0;
            int hits = 0, need = 0;
            const int r = g_felling.chop(eye, fwd, t, &id, &hits, &need);
            if (r == 0) std::snprintf(g_tree_msg, sizeof g_tree_msg, "nothing in reach");
            else if (r == 1) std::snprintf(g_tree_msg, sizeof g_tree_msg, "chop %d/%d", hits, need);
            else std::snprintf(g_tree_msg, sizeof g_tree_msg, "TIMBER (%zu felled)", g_felling.felled());
        }
        if (g_want_pick) {
            g_want_pick = false;
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
            float hand[3];
            chop_origin(hand);
            if (g_renderer.pick_mushroom(eye, fwd, hand, kPickReach))
                std::snprintf(g_tree_msg, sizeof g_tree_msg, "picked a glowing mushroom (%zu)",
                              g_renderer.mushrooms().picked());
            else
                std::snprintf(g_tree_msg, sizeof g_tree_msg, "nothing to pick up in reach");
        }
        if (g_dig_held && g_captured && !g_paused) {
            g_dig_timer += float(dt);
            if (g_dig_timer >= kDigRepeat) { g_dig_timer = 0.0f; g_want_dig = true; }
        }
        if (g_want_dig) {
            g_want_dig = false;
            float fwd[3], right[3], up[3];
            g_camera.basis(fwd, right, up);
            const float eye[3] = {g_camera.x, g_camera.y, g_camera.z};
            const engine::Renderer::AimedDig d =
                g_renderer.dig_aimed(eye, fwd, kDigReach, kDigRadii[g_dig_size]);
            if (!d.hit) std::snprintf(g_tree_msg, sizeof g_tree_msg, "dig: no ground within %.0f m", kDigReach);
            else std::snprintf(g_tree_msg, sizeof g_tree_msg, "dug r %.2f m at %.1f m, %zu voxels, %.1f ms",
                               kDigRadii[g_dig_size], d.dist, d.voxels, d.ms);
        }
        if (g_snap_walk_frames >= 0 && --g_snap_walk_frames < 0) {
            g_walk = true;
            g_physics.place_character(g_snap_feet[0], g_snap_feet[1] + 0.2f, g_snap_feet[2]);
            g_char_yaw = g_camera.yaw;
            g_have_prev_feet = false;
            if (g_snap_cam_dist >= 0.0f) g_cam_dist = g_snap_cam_dist;
        }
        if (g_want_snapshot) {
            g_want_snapshot = false;
            engine::ViewConstants sv = {};
            fill_view(g_camera, w, h, t, sv);
            const std::string dir = write_snapshot(sv);
            std::snprintf(g_tree_msg, sizeof g_tree_msg, dir.empty() ? "snapshot FAILED" : "snapshot %s",
                          dir.c_str());
        }
        if (g_want_save || g_want_load) {
            std::string err;
            bool ok = true, mismatch = false;
            if (g_want_save) {
                ok = g_renderer.save_edits("build/edits.bin", err) &&
                     g_felling.save("build/trees.bin", g_renderer.world().field_id(), err);
                std::snprintf(g_tree_msg, sizeof g_tree_msg, ok ? "saved" : "save FAILED");
            } else {
                ok = g_renderer.load_edits("build/edits.bin", err, mismatch);
                const bool ok2 = g_felling.load("build/trees.bin", g_renderer.world().field_id(), err, mismatch);
                std::snprintf(g_tree_msg, sizeof g_tree_msg, (ok || ok2) ? "loaded" : "nothing to load");
            }
            g_want_save = g_want_load = false;
        }

        g_ui.update(float(wall_dt));

        engine::ViewConstants view = {};
        fill_view(g_camera, w, h, t, view);
        g_renderer.render(view);

        if (g_want_shot) {
            g_want_shot = false;
            std::string err;
            g_renderer.capture(view, "build/shot.bmp", err);
        }

        if (g_time_lapse) g_renderer.set_time_of_day(g_renderer.time_of_day() + float(dt) * 0.1f);
        const double frame_ms = wall_dt * 1000.0;
        if (frame_ms > worst_frame_ms) worst_frame_ms = frame_ms;
        fps_accum += wall_dt;
        ++fps_frames;
        if (fps_accum >= 0.5) {
            const engine::FrameStats st = g_renderer.stats();
            char title[512];
            const float tod = g_renderer.time_of_day();
            std::snprintf(title, sizeof(title),
                          "biomes2 engine - %02d:%02d%s [PgUp/PgDn H] - wind %.1f m/s, sea Hs %.1f m%s [[ ] \\ ;] - %s [M] %s [T] - trees %.0f m [-=] - dig r %.2f m [RMB , .] %s - %.0f fps (worst %.1f ms) - "
                          "%zu chunks, %u tris - %u meshed/frame in %.3f ms - "
                          "%zu pending, %zu slots free%s%s",
                          int(tod), int((tod - float(int(tod))) * 60.0f), g_time_lapse ? " running" : "",
                          g_renderer.weather_state().wind_speed, ww_significant_height(g_renderer.sea()),
                          g_renderer.weather().automatic() ? "" : " fixed",
                          g_renderer.mode() == engine::Mode::Mesh ? "shaded" : "wireframe",
                          g_renderer.texture_mode() < 0.5f ? "flat"
                          : g_renderer.texture_mode() < 1.5f ? "detail"
                          : g_renderer.texture_mode() < 2.5f ? "grid" : "detail+grid",
                          g_renderer.tree_settings().max_dist, kDigRadii[g_dig_size], g_tree_msg,
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

    g_renderer.flush();
    g_renderer.set_overlay(nullptr);
    g_ui.shutdown();
    g_renderer.shutdown();
    g_physics.shutdown();
    return 0;
}
