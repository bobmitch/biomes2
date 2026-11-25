// ---------------------------------------------------------------------------
// Device, swapchain, and the frame.
//
// Deliberately not a "graphics abstraction". Everything here is the minimum
// D3D12 the rest of Track B builds on -- descriptor heaps, per-frame command
// allocators, fence-value synchronisation -- and nothing is wrapped until
// there are two callers to justify the wrapper. See ADR-003 for why D3D12.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDERER_H
#define ENGINE_RENDERER_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/streaming/streamer.h"
#include "engine/svdag/brick_store.h"
#include "engine/svdag/brick_pager.h"
#include "engine/field/world_gpu.h"
#include "engine/trees/tree_field.h"
#include "engine/trees/tree_renderer.h"
#include "engine/foliage/foliage_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/local_lights.h"
#include "engine/render/fog.h"
#include "engine/render/weather.h"
#include "engine/render/ocean_detail.h"
#include "engine/render/clouds.h"
#include "engine/render/rain.h"
#include "engine/render/lightning.h"
#include "engine/render/canopy.h"
#include "engine/render/height_gi.h"
#include "engine/render/near_field.h"
#include "engine/render/sea_access.h"
#include "engine/render/far_shadow.h"
#include "engine/render/scan_library.h"
#include "engine/render/forest_map.h"
#include "engine/rocks/rock_field.h"
#include "engine/rocks/rock_renderer.h"
#include "engine/props/glow_mushrooms.h"
#include "engine/character/character_renderer.h"

#include <functional>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Two frames in flight. The design's readback path wants 2-3 frames of
// latency, so this is the number that will matter later; it is here now
// because getting allocator recycling wrong is invisible until it is not.
static const UINT kFrameCount = 2;

// The design's cap on remeshing per frame. Without it, flying into an unloaded
// region meshes everything at once and the frame that does it is the one the
// player notices.
static const UINT kMeshBudgetPerFrame = 8;
// The cap is now a FLOOR: the budget grows while the GPU's meshing time stays
// under this, up to kMeshBudgetMax, and shrinks back when it does not. A fixed
// 8 used 0.5 ms of a 16.7 ms frame at 15 m/s and still made distant detail
// wait 30 frames. 4 ms is what leaves the 60 Hz frame room for the draw.
static const double kMeshTargetMs = 4.0;   // only with --adaptive-budget

struct ViewConstants {
    float cam_pos[3];    float time;
    float cam_fwd[3];    float aspect;
    float cam_right[3];  float tan_half_fov;
    float cam_up[3];     float pad0;
    // The character capsule, drawn by the raymarch. cap_r <= 0 means none.
    float cap_pos[3];    float cap_r;
    float cap_half;      float pad1[3];
};

// What the frame draws. Raymarch and Mesh render the same field from the same
// camera; the only intended difference is that one marches it and the other
// draws triangles built from it. Any other difference is a meshing bug, which
// is why the raymarch survives rather than being deleted once there is a mesh.
// The raymarch is not streamed -- it is the whole field, always.
// B1 had a third mode that raymarched the field directly. M1 retired it
// with the trivial field it was the only remaining caller of.
enum class Mode { Mesh, Wireframe };

struct FrameStats {
    // Readback: how many height patches came back, how stale they were, and
    // how far the surface they describe is from the field itself.
    unsigned long long patches_total = 0;
    UINT   patches_this_frame = 0;
    size_t collision_patches = 0;
    unsigned long long chunks_classified = 0;
    unsigned long long chunks_needing_mesh = 0;
    unsigned long long mesh_shapes_delivered = 0;
    size_t mesh_backlog = 0;
    size_t edit_bricks = 0;
    size_t edit_bytes = 0;
    UINT   bricks_uploaded = 0;
    size_t chunks_invalidated = 0;
    size_t edit_voxels = 0;
    size_t bricks_resident = 0;
    unsigned long long brick_evictions = 0;
    unsigned long long brick_misses = 0;
    unsigned long long brick_upload_deferrals = 0;
    UINT   readback_latency_frames = 0;
    size_t empty_resident = 0;
    float  height_residual_voxel = 0.0f;
    UINT   peak_slot_vertices = 0;
    UINT   peak_slot_indices = 0;
    float  height_residual_max = 0.0f;
    double height_residual_mean = 0.0;
    unsigned long long height_samples_checked = 0;
    size_t resident = 0, pending = 0, free_slots = 0;
    UINT   vertices = 0, indices = 0, overflowed = 0;
    UINT   meshed_this_frame = 0;
    UINT   mesh_budget = 0;        // what this frame was allowed
    double mesh_ms = 0.0;
};

class Renderer {
public:
    // Returns false and fills `error` rather than throwing: the caller puts it
    // in a message box, which is the only diagnostic a windowed app has before
    // there is anything on screen.
    // debug_layer turns on D3D12 validation regardless of build config and
    // makes drain_debug_messages useful. Validation is the only way to see a
    // state or binding mistake: the symptom otherwise is an empty screen.
    bool init(HWND hwnd, std::string& error, bool debug_layer = false);

    // Writes every validation message produced so far to a file.
    void drain_debug_messages(const char* path);
    void dump_draw_args(const char* path);
    void render(const ViewConstants& view);
    bool capture(const ViewConstants& view, const char* path, std::string& error);
    // --shake: frames recorded off screen with TAA history kept, then raw
    // captures (no warm-up, no history reset) of consecutive frames.
    void step_offscreen(const ViewConstants& view);
    void set_capture_raw(bool on) { capture_raw_ = on; }
    // --shot --ui: draw the overlay into the still as well.
    void set_capture_overlay(bool on) { capture_overlay_ = on; }
    void resize(UINT width, UINT height);
    void shutdown();

    // Recompiles every shader from disk. The whole of Track B's remaining work
    // is shader editing, so this stays reachable.
    bool reload_shaders(std::string& error);

    // Off for benchmarking: with vsync on, every measurement reads 60 and
    // says nothing about how much headroom there is.
    void set_vsync(bool on) { vsync_ = on; }
    bool vsync() const { return vsync_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

    // Drawn over the finished frame, straight onto the back buffer, in
    // render() only -- capture() never calls it, so a --shot holds no UI.
    // The callee may bind its own descriptor heap: nothing after it in the
    // command list needs the renderer's.
    using Overlay = std::function<void(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, UINT, UINT)>;
    void set_overlay(Overlay o) { overlay_ = std::move(o); }
    // Waits until the GPU has finished everything submitted so far.
    void flush() { wait_for_gpu(); }
    bool tearing_available() const { return tearing_ok_; }
    // What the slot pool is actually holding, after a capture: per-chunk
    // vertex and index counts against the caps, and memory against the GPU.
    void slot_report(const char* path);
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    const FrameStats& stats() const { return stats_; }

    const WorldGpu& world() const { return world_; }

    // The weather (weather.h): the wind every swaying thing shares and the sea
    // it raises. Automatic unless overridden; --sea-state and [ ] set the
    // storm dial (0 calm, 1 gale), --wind a speed. update_weather(t) is
    // called by record_scene with the frame's clock, and by anything (the
    // physics sync) that needs this frame's sea before the frame is drawn.
    void  set_sea_state(float s) { weather_.set_storm(s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s)); }
    float sea_state() const { return weather_now_.storm; }
    // Whether the last frame's eye was under the sea (ADR-006 stage 2), and
    // how far under the surface over it.
    bool  camera_underwater() const { return cam_under_; }
    float camera_water_depth() const { return cam_under_depth_; }
    Weather& weather() { return weather_; }
    const WeatherState& weather_state() const { return weather_now_; }
    const WwSea& sea() const { return sea_; }
    const WwSwash& swash() const { return swash_; }
    void  update_weather(double t);
    void  set_water(bool on) { water_ = on; }
    // Screen-space reflections on the sea (F7, --no-ssr).
    void  set_ssr(bool on) { ssr_on_ = on; }
    bool  ssr() const { return ssr_on_; }
    // The RT scene's trees lean with their meshes (tree_sway.h); off for A/B.
    void  set_clouds(bool on) { clouds_on_ = on; }
    bool  clouds() const { return clouds_on_; }
    const Clouds& cloud_layer() const { return clouds_; }
    // Rain (ADR-005 C3): from the clouds' precipitation, or fixed with
    // --rain <0..1> (negative: automatic). Shift+F3 / --no-rain.
    void  set_rain(float k) { rain_override_ = k; }
    float rain_override() const { return rain_override_; }
    void  set_rain_on(bool on) { rain_on_ = on; }
    bool  rain_on() const { return rain_on_; }
    float rain_intensity() const;
    // The wet world (ADR-005 C4): the surfaces' wetness and the puddles, from
    // the weather's rain history; fixed with --wet <0..1> (both at once).
    void  set_wet(float k) { wet_override_ = k; }
    float wet_override() const { return wet_override_; }
    void  wetness(float& wet, float& puddle) const;
    // How wet the crowns are: drips under the trees, after the rain too.
    float drips() const;
    // Lightning (ADR-005 C5): the strike this frame, if any. --lightning
    // <dist> <bearing_deg> forces one for a still.
    const Strike& strike() const { return strike_; }
    // How much of the sky's light a storm deck lets down (ADR-005 C5): 1
    // under anything but a heavy deck of tall cloud, 0.2 under a closed
    // cumulonimbus. Every sky term, the haze and the clouds' own ambient.
    float storm_dim() const;
    // The exploration fog (haze.hlsli): layered height fog that limits how
    // far the low land can be seen. --no-height-fog, --height-fog <k> scales it.
    void set_height_fog(bool on) { hfog_on_ = on; }
    void set_height_fog_scale(float k) { hfog_scale_ = k; }
    bool  height_fog() const { return hfog_on_; }
    float height_fog_scale() const { return hfog_scale_; }
    // Its extinction at an altitude now (1/m), for the shot report.
    float height_fog_sigma(float y) const {
        if (!hfog_on_ || !fog_on_ || !fog_map_.built()) return 0.0f;
        return fog_map_.height_sigma(y, world_.params().sea_level,
                                     fog_map_.valley_factor(time_hours_, weather_now_.wind_speed)) *
               fog_density_ * (1.0f + 1.5f * rain_intensity()) * hfog_scale_;
    }
    float height_fog_valley() const { return fog_map_.valley_factor(time_hours_, weather_now_.wind_speed); }
    // Strikes seen since start (distinct), ground strikes among them, and
    // frames drawn with a flash lit -- the schedule's instrument.
    unsigned strikes_seen() const { return strikes_seen_; }
    double weather_time() const { return weather_t_; }
    unsigned ground_strikes_seen() const { return ground_strikes_seen_; }
    unsigned long long flash_frames() const { return flash_frames_; }
    Lightning& lightning() { return lightning_; }
    void set_lightning(bool on) { lightning_on_ = on; }
    bool lightning_on() const { return lightning_on_; }
    void  set_wave_detail(bool on) { detail_on_ = on; }
    bool  wave_detail() const { return detail_on_; }
    const OceanDetail& ocean() const { return ocean_; }
    // LOD transitions: hold the outgoing chunk until its replacements are
    // meshed, then dither-cross-fade over `frames`. See Streamer.
    void  set_lod_hold(bool on) { streamer_.set_lod_hold(on); }
    void  set_lod_fade_frames(int n) { streamer_.set_fade_frames(n); }
    void  set_measure_holes(bool on) { streamer_.set_measure_holes(on); }
    // Surface-first streaming (default on) and the GPU-time budget (default
    // off -- see adaptive_budget_). Both switchable for A/B.
    void  set_prioritise(bool on) { streamer_.set_prioritise(on); }
    void  set_lod_align(bool on) { streamer_.set_align(on); }
    // 1 tints the skirt curtain magenta, 2 tints sharp creases cyan.
    void  set_mesh_debug(float m) { mesh_debug_ = m; }
    // Surface texture: 0 flat, 1 noise detail (default), 2 metric grid, 3 both.
    void  set_texture_mode(float m) { texture_mode_ = m; }
    float texture_mode() const { return texture_mode_; }
    void  set_adaptive_budget(bool on) {
        adaptive_budget_ = on;
        if (!on) mesh_budget_ = kMeshBudgetPerFrame;
    }
    const Streamer& streamer() const { return streamer_; }
    // 0 real shading, 1 depth, 2 foam, 3 normal, 4 displacement.
    void  set_water_debug(float m) { water_debug_ = m; }
    float water_debug() const { return water_debug_; }
    bool  water() const { return water_; }

    // Mesh one chunk and compare its density grid against the CPU generator.
    // Returns false only if the GPU work failed; disagreement is reported in
    // the numbers, not as an error.
    bool probe_field(const float origin[3], float voxel, const char* path,
                     std::string& error);

    // What residency costs: how tall the band is over each column, how much
    // of that is terrain and how much is margin, and how many chunks that
    // works out to. Reading only; it changes nothing.
    void band_report(const char* path) const;

    // Is the shading normal the surface's normal? field_normal takes a
    // central difference with a half-voxel arm, which is 0.25 m at LOD 0 and
    // 32 m at LOD 7. This meshes one chunk at each voxel size and compares
    // every stored normal against the CPU gradient taken at a SMALL arm.
    void normal_check(const char* path);

    // The edit overlay. Digging goes through here, and the renderer takes care
    // of uploading what changed and re-meshing what it touched.
    BrickStore& edits() { return edits_; }

    // Trees. The field is the authority on which trees stand (CPU, layer 8);
    // the caller adds what has been felled as extra instances -- logs and
    // stumps, posed by physics -- every frame they move.
    TreeField&             tree_field() { return tree_field_; }
    // Layer 9's rocks: placement, drawing, colliders (--no-rocks).
    RockField&             rock_field() { return rock_field_; }
    RockRenderer&          rocks() { return rocks_; }
    void set_rocks(bool on) { rocks_on_ = on; }
    // Glowing mushrooms in the caves (engine/props/glow_mushrooms.h;
    // --no-mushrooms): populated near the camera, a light per cluster.
    GlowMushrooms&         mushrooms() { return mushrooms_; }
    void set_mushrooms(bool on) { mushrooms_on_ = on; }
    // Picks the mushroom nearest along the camera's ray within `reach` of
    // `hand` and in front of the terrain the ray meets: true if one was.
    bool pick_mushroom(const float eye[3], const float dir[3], const float hand[3], float reach);
    // The player's character (engine/character/): the pack is chosen before
    // init (--character), and the caller poses it every frame.
    void set_character_pack(const std::string& path) { character_pack_ = path; }
    CharacterRenderer&     character() { return character_; }
    // The canopy shell past the trees' draw distance (forest_map.h; --no-forest-shell).
    void set_forest_shell(bool on) { forest_on_ = on; }
    const TreeRenderer&    trees() const { return trees_; }
    // The overdraw instrument (tree_renderer.h), for the report modes.
    bool tree_overdraw_begin(std::string& error) {
        wait_for_gpu();
        return trees_.overdraw_begin(queue_.Get(), width_, height_, error);
    }
    bool tree_overdraw_read(std::vector<uint32_t>& out, std::string& error) {
        wait_for_gpu();
        const bool ok = trees_.overdraw_read(queue_.Get(), out, error);
        trees_.overdraw_end();
        return ok;
    }
    // Per-pass GPU timing and pipeline statistics (gpu_profile.h), for
    // --gpu-profile. Off by default; while on, every pass is serialised.
    GpuProfiler& profiler() { return prof_; }
    void set_profiling(bool on) { prof_.set_enabled(on); }
    TreeDrawSettings&      tree_settings() { return tree_settings_; }
    void set_trees(bool on) { trees_on_ = on; }
    // The trees' depth prepass (tree_renderer.h); on by default, --no-tree-prepass for A/B.
    void set_tree_prepass(bool on) { trees_.set_prepass(on); }
    bool tree_prepass() const { return trees_.prepass(); }
    // The terrain's depth prepass: each terrain pixel shaded once. On by
    // default; --no-terrain-prepass and the Terrain tunable for A/B.
    void set_terrain_prepass(bool on) { terrain_prepass_ = on; }
    bool terrain_prepass() const { return terrain_prepass_; }
    // Ground cover: grass, flowers, bushes. --no-foliage and G for A/B.
    void set_foliage(bool on) { foliage_on_ = on; }
    bool foliage_on() const { return foliage_on_; }
    FoliageSettings& foliage_settings() { return foliage_settings_; }
    const FoliageRenderer& foliage() const { return foliage_; }
    bool foliage_counts(uint32_t out[5], std::string& error) {
        wait_for_gpu();
        return foliage_.read_counts(queue_.Get(), out, error);
    }
    // Sun shadows, three cascades. On by default; --no-shadows and L for A/B.
    void set_shadows(bool on) { shadows_on_ = on; }
    bool shadows() const { return shadows_on_; }
    // The post chain (post.hlsl). All on by default; Y, O and J in the window.
    void set_taa(bool on) { taa_on_ = on; hist_valid_ = false; }
    bool taa() const { return taa_on_; }
    void set_ao(bool on) { ao_on_ = on; }
    bool ao() const { return ao_on_; }
    void set_contact(bool on) { contact_on_ = on; }
    bool contact() const { return contact_on_; }
    void set_post_debug(float d) { post_debug_ = d; }
    // Exposure and tonemap (ADR-004 L1). Metered by default; a fixed EV100
    // (> -100) overrides the meter. Tonemap 0 is the pre-L1 display path
    // (no exposure), 1 AgX, 2 a fitted ACES.
    void set_ev_comp(float stops) { ev_comp_ = stops; }
    float ev_comp() const { return ev_comp_; }
    void set_ev100_fixed(float ev) { ev100_fixed_ = ev; }
    // Dark vision: darkness underground and at night comes from the eye and
    // the display, never from added light. No ambient floor in any lighting:
    // it would relight sealed spaces, flatten a torch's contrast and make
    // emitters worth less. The defaults are here, not in archetypes.json:
    // there is no exposure block there, and these are the eye's, not the
    // world's. The tuning panel's Exposure group sets them live.
    struct DarkVision {
        // How far down the eye adapts (EV100) with NOTHING lighting the view,
        // by its share of open sky (camera_open): under the open sky at night,
        // and with none over it -- a tunnel, a cave. Blended between, so a
        // shaft or a cave mouth passes smoothly. These make the unlit dark;
        // they must not hold down a lit one (floor_lit, below).
        float floor_open = -0.5f;
        float floor_under = 1.5f;
        // A light in the view sets the exposure. The CPU sums the illuminance
        // the torch and every fire in clear view (a physics ray) put on the
        // ground under the eye -- where the player looks; at the eye itself, a
        // torch carried by a camera 22 m up counted in full and lit nothing in
        // view; between lit_lux_lo and lit_lux_hi lux that becomes a light
        // presence 0..1, which lowers the floor towards floor_lit and, in a
        // dark scene only, keys the meter on the lit part of the view -- the
        // mean of its 70th-97th percentiles `lit_key` stops over middle grey
        // -- instead of the mean of a mostly dark frame. Before this a
        // torch-lit tunnel (walls 0.05-0.2 nits) was held 1-3 stops under
        // middle grey by floor_under.
        float floor_lit = -2.0f;
        float lit_key = 1.5f;
        float lit_lux_lo = 0.03f, lit_lux_hi = 0.3f;
        // Adaptation, one time constant each way (seconds). The darkness is
        // the floors', not a wait: a slow rod phase (2 s for 4 stops, then
        // 20-40 s) took 75 s to settle a torch-lit tunnel entered from
        // daylight, and read as the eye "opening up" on an artificial dark.
        float dark = 0.8f;
        float light = 0.3f;
        // The display's dark floor (ps_final, after the tonemap): true black
        // lifted towards a dark blue-grey, this share of the display range;
        // and colour fading out below `threshold` scene nits (gone by a tenth
        // of it) -- rod vision, with the Purkinje shift's blue.
        bool  on = true;
        float lift = 0.015f;
        // A moonlit meadow is ~0.005 nits and goes grey; a torch-lit tunnel
        // is 0.05-0.2 and keeps its colour (0.05 greyed its darkest quarter,
        // 0.3 the whole wall).
        float threshold = 0.015f;
        // The player's Brightness (Graphics, saved): a display gamma, 1 none.
        float brightness = 1.0f;
    };
    DarkVision& dark_vision() { return dark_; }
    // A ray through the application's physics (terrain near the player,
    // trunks, rocks): metres to the first hit within `len`, or < 0 for none.
    // Dark vision uses it to find the ground under the eye and whether a light
    // is in clear view of it. None set: the eye's own height, every light.
    void set_ray_query(std::function<float(const float from[3], const float dir[3], float len)> f) { ray_query_ = std::move(f); }
    // Dark vision's light presence, 0..1 (DarkVision::floor_lit), and the
    // illuminance behind it (lux at the eye from visible local lights).
    float light_presence() const { return light_presence_; }
    float light_lux_at_eye() const { return light_lux_eye_; }
    const DarkVision& dark_vision() const { return dark_; }
    // A simulated time step for the eye's adaptation instead of the wall
    // clock's (< 0: the wall clock), so a timed test is the same curve
    // however fast it renders.
    void set_post_dt_override(float s) { post_dt_override_ = s; }
    // The exposure state as the GPU last left it: adapted log2 nits, the
    // exposure multiplier, EV100, this frame's target log2 nits. Read back
    // per frame, a frame or two late (immediately after step_offscreen).
    const float* live_exposure() const { return live_expo_; }
    void set_tonemap(unsigned mode) { tonemap_ = mode; }
    unsigned tonemap() const { return tonemap_; }
    void set_agx_saturation(float s) { agx_saturation_ = s; }
    void set_agx_power(float p) { agx_power_ = p; }
    float agx_saturation() const { return agx_saturation_; }
    float agx_power() const { return agx_power_; }
    // Time of day (ADR-004 L2), hours 0..24. The sun, the moon, the sky and
    // every light the scene uses follow it.
    void  set_time_of_day(float hours);
    float time_of_day() const { return time_hours_; }
    const SkyFrame& sky_frame() const { return sky_frame_; }

    // Local lights (ADR-004 L3). A torch carried at the camera; lights
    // placed in the world (returns the light's id); their shadows, on by
    // default, off for A/B.
    void set_carried_torch(bool on) { torch_on_ = on; }
    bool carried_torch() const { return torch_on_; }
    // Where the carried torch is held: a world point (the character's hand
    // in third person), or nullptr for beside the eye as before.
    void set_torch_anchor(const float* p) {
        torch_anchored_ = p != nullptr;
        if (p) for (int k = 0; k < 3; ++k) torch_anchor_[k] = p[k];
    }
    uint32_t add_light(const LocalLight& l) {
        lights_.push_back(l);
        lights_.back().id = next_light_id_++;
        return lights_.back().id;
    }
    void clear_lights() { lights_.clear(); }
    const std::vector<LocalLight>& lights() const { return lights_; }
    size_t light_count() const { return lights_.size() + (torch_on_ ? 1 : 0); }
    void set_light_shadows(bool on) { light_shadows_on_ = on; }
    bool light_shadows() const { return light_shadows_on_; }
    struct LightStats {
        uint32_t lights = 0;          // sent to the GPU this frame
        uint32_t shadowed = 0;        // with a shadow ready
        uint32_t faces_drawn = 0;     // shadow faces redrawn this frame
        uint32_t chunk_draws = 0;     // terrain chunk draws in those faces
        uint32_t body_redraws = 0;    // lights whose faces were redrawn for the player's body alone
        uint32_t body_faces = 0;      // faces the body was drawn into
        // From the cluster pass, read back a couple of frames late.
        uint32_t overflowed = 0, max_in_cluster = 0, clusters_lit = 0, cluster_items = 0;
    };
    const LightStats& light_stats() const { return light_stats_; }

    // Volumetric mist (ADR-004 L4). On by default; N in the window and
    // --no-fog for A/B; a density multiplier for tuning.
    void set_fog(bool on) { fog_on_ = on; fog_hist_valid_ = false; }
    bool fog() const { return fog_on_; }
    void set_fog_density(float k) { fog_density_ = k; }
    // Bare ground past the near field (scan_library.h GroundLook): live.
    GroundLook& ground_look() { return ground_look_; }
    float fog_density() const { return fog_density_; }
    const FogMap& fog_map() const { return fog_map_; }
    // The fog's authored settings, live: the layers and the mist's top are
    // read every frame, so a tuning panel can move them without a rebake.
    // The per-biome density map is baked once and does NOT follow.
    FogSettings& fog_settings() { return fog_map_.settings(); }
    // GPU time of the post chain's parts, ms, smoothed over frames: the
    // atmosphere's tables, the mist's inject and integrate, the whole chain.
    // Frame-time A/B cannot resolve passes this small on a noisy machine.
    struct PostTimes { float atmosphere = 0, mist_inject = 0, mist_integrate = 0, post_total = 0; };
    const PostTimes& post_times() const { return post_times_; }

    // Hardware ray tracing (ADR-004 L5/L6: RT shadows, their denoiser, the
    // traced sky, DDGI, leaf cutouts) was REMOVED; git tag `rt-last` holds it.
    // Shadows are the cascades and PCSS, GI the heightfield's.
    // L6's canopy shade: the canopy map (canopy.h) in gi_sky_visibility.
    void set_canopy(bool on) { canopy_on_ = on; }
    bool canopy() const { return canopy_on_; }
    unsigned canopy_trees() const { return canopy_.trees_last_frame(); }
    unsigned long long canopy_builds() const { return canopy_.builds(); }
    // The screen-space bounce (ADR-004 L6's no-RT experiment): OFF by
    // default (--ssgi, X). It measured 1-2% more light in shade for 0.3 ms --
    // the bounce that matters here comes from terrain and canopy off screen
    // or behind the camera.
    void set_ssgi(bool on) { ssgi_on_ = on; if (!on) ssgi_valid_ = false; }
    // The heightfield GI (ADR-004 L6, height_gi.h): the diffuse GI
    // (--no-hfgi, Z).
    void set_hfgi(bool on) { hfgi_on_ = on; }
    // The near field (near_field.h): light under the top surface. --no-near-field for A/B.
    void set_near_field(bool on) { near_on_ = on; }
    bool near_field_on() const { return near_on_; }
    const NearField& near_field() const { return near_; }
    NearField::Schedule& near_field_schedule() { return near_.schedule(); }
    // Parallax occlusion on the scans' height (pom.hlsli; --no-pom, P).
    void set_pom(bool on) { pom_on_ = on; }
    bool pom() const { return pom_on_; }
    // The scans' anisotropic filtering (terrain, rocks, character, bark): 1
    // trilinear, 2, 4 (default), 8 or 16 -- rounded to a power of two. Live:
    // it picks which of the sampler heap's fixed samplers s5's table points
    // at. --aniso <n>, Graphics.
    void set_scan_aniso(float k);
    float scan_aniso() const { return scan_aniso_; }
    // The scans' sampler for the current setting, for s5's table.
    D3D12_GPU_DESCRIPTOR_HANDLE scan_sampler() const;
    bool hfgi() const { return hfgi_on_; }
    unsigned long long hfgi_rebuilds() const { return hfgi_.rebuilds(); }
    // The far terrain shadow past the cascades (far_shadow.h; --no-far-shadow).
    void set_far_shadow(bool on) { far_on_ = on; }
    bool ssgi() const { return ssgi_on_; }

    // What the meter saw in the last capture(): the histogram and the
    // exposure it chose. bins[k] covers log2 nits kLumLogMin + k * step.
    struct ExposureReport {
        bool  valid = false;
        float adapted_log2 = 0, exposure = 0, ev100 = 0, target_log2 = 0;
        float lo_nits = 0, hi_nits = 0, metered = 0;
        float log_min = -10.0f, log_max = 18.0f;
        std::vector<unsigned> bins;
    };
    const ExposureReport& last_exposure() const { return exposure_report_; }
    // The GPU's sky-view table against the CPU's march of the same texels,
    // from the last capture() (ADR-004 L2): the atmosphere's --probe-field.
    struct SkyCheck {
        bool  valid = false;
        int   texels = 0;
        float worst_rel = 0.0f, mean_rel = 0.0f;   // relative luminance error
        float worst_at[2] = {0, 0};                // texel (x, y)
    };
    const SkyCheck& last_sky_check() const { return sky_check_; }
    // The clouds' shadow map from the last capture() (ADR-005 C2): the share
    // of its 12 x 12 km where the key light's beam gets through (T > 0.5),
    // and its mean transmittance -- how cloudy the ground's light is, over
    // far more sky than one view shows.
    struct CloudShadowReport {
        bool  valid = false;
        float sunlit = 0.0f, mean_t = 0.0f;
        float pct[5] = {0, 0, 0, 0, 0};   // the map's 10th, 25th, 50th, 75th, 90th percentiles
    };
    const CloudShadowReport& last_cloud_shadow() const { return cloud_shadow_report_; }

    ID3D12Device*       device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }

    // Holes, counted by the last capture() below the horizon only.
    // `undrawn` is the definition that survives a tonemap: far depth AND both
    // HDR scene targets still at their clear values -- nothing, sea included,
    // was drawn there. Far depth alone counts the sea beyond the clipmap,
    // which writes no depth, as holes. (The old count by final sky colour
    // went with L1: the tonemap is entitled to change the sky's colour.)
    struct CrackCount {
        size_t below_horizon = 0, by_depth = 0, undrawn = 0;
        std::vector<std::pair<int, int> > where;   // the first undrawn pixels, for --pick
    };
    const CrackCount& last_cracks() const { return cracks_; }
    bool trees_on() const { return trees_on_; }
    void set_tree_extras(const std::vector<TreeGpuInstance>& extra) {
        tree_extras_ = extra;
        tree_dirty_ = true;
    }
    // A chop: the tree shakes for half a second from `time`.
    void tree_hit(uint32_t id, float time) { tree_hits_[id] = time; tree_dirty_ = true; }
    // Per-draw visible counts from the last frame, for reports. Waits.
    bool tree_counts(std::vector<uint32_t>& per_draw, uint32_t& overflow, std::string& error) {
        wait_for_gpu();
        return trees_.read_counts(queue_.Get(), per_draw, overflow, error);
    }
    bool dig(float x, float y, float z, float radius, float strength,
             unsigned char material);

    // The terrain as LOD 0 meshes it, on the CPU: the player's value at each
    // lattice corner where one is stored, else the field at the 0.5 m voxel,
    // trilinear between. Away from any edit it is the field itself.
    float edited_density(float x, float y, float z) const;
    // Whether the column at (x, z) holds anything solid between y_lo and
    // y_hi, by the same field: a march up, careful near surfaces, with no
    // bisection after the first rock. Physics' sea cover (a roof between
    // the feet and sea level means no sea).
    bool solid_between(float x, float z, float y_lo, float y_hi) const;
    // ADR-006 stage 3: the sea as the connectivity flood labels it
    // (sea_access.h), at a point. -1 not labelled here (the caller falls
    // back to the column rule and stage 1's roof test); 0 not water; 1
    // water, with the surface over it (sea level, or a pocket's floor) and
    // how much of the open sea's waves reach it (1 the open sea, falling
    // with the path through the water to it).
    int water_at(float x, float y, float z, float& surface, float& waves) const;
    // Where the player is, so the flood is asked about blocks round it as
    // well as round the camera (a third-person camera can be metres off).
    void set_flood_focus(float x, float y, float z) { flood_focus_[0] = x; flood_focus_[1] = y; flood_focus_[2] = z; flood_focus_set_ = true; }
    // Until the flood's worker has answered everything asked (a test).
    void flood_settle();
    const SeaAccess& sea_access() const { return sea_access_; }
    size_t flood_blocks() const { return flood_.size(); }
    // Where a ray first enters the terrain (edits included), within `reach`
    // metres. False if it starts inside rock or meets nothing.
    bool aim_terrain(const float from[3], const float dir[3], float reach,
                     float hit[3], float& dist) const;
    // A dig aimed along a ray: carve a sphere of `radius` centred on the
    // nearest SOLID lattice point half a voxel past where the ray enters the
    // ground, so even the smallest dig always removes rock. `voxels` is how
    // many lattice points it wrote.
    struct AimedDig {
        bool  hit = false, dug = false;
        float dist = 0.0f;
        float hit_pos[3] = {0, 0, 0};
        float centre[3] = {0, 0, 0};
        size_t voxels = 0;
        double ms = 0.0;      // the CPU time the aim and the edit took
    };
    AimedDig dig_aimed(const float from[3], const float dir[3], float reach, float radius);
    // The first ground at or below `from_y` on the vertical through (x, z),
    // edits included; `from_y` itself if there is none within 200 m.
    float ground_below(float x, float z, float from_y, bool with_edits = true) const;


    // Persistence. Saving writes only the voxels the player actually edited;
    // loading throws away every resident chunk, because the terrain they were
    // meshed from has just changed underneath them.
    bool save_edits(const char* path, std::string& error) const;
    bool load_edits(const char* path, std::string& error, bool& field_mismatch);

    // Height patches that arrived through the readback ring this frame, and
    // the chunks whose patches are no longer valid. Drained by the caller and
    // handed to physics; the renderer keeps no collision state of its own.
    struct PatchEvent {
        ChunkKey key;
        float    origin[3];
        float    voxel;
        // Columns in this chunk holding more than one surface. Non-zero means
        // a heightfield cannot represent it and it needs a mesh shape.
        UINT     multi_surface_columns;
        std::vector<float> h;
    };
    // A chunk the cheap shape cannot represent, with its actual triangles.
    // Arrives four frames after the chunk was meshed: two for the
    // classification to come back, two more for the geometry.
    struct MeshPatchEvent {
        ChunkKey key;
        std::vector<float>    verts;   // xyz triples, world space
        std::vector<unsigned> idx;     // into verts
    };

    const std::vector<PatchEvent>& patches_added() const { return patches_added_; }
    const std::vector<MeshPatchEvent>& mesh_patches_added() const {
        return mesh_patches_added_;
    }
    const std::vector<ChunkKey>&   patches_removed() const { return patches_removed_; }

    // Max and mean |density| at the vertices of slot 0, in metres. A vertex on
    // the isosurface reads zero; surface nets averages edge crossings rather
    // than solving for the surface, so a small residual is expected and a
    // large one means the mesh is not the field's surface.
    bool check_against_field(float& max_err, float& mean_err, UINT& checked);

private:
    bool create_device(std::string& error, bool debug_layer);
    bool create_swapchain(HWND hwnd, std::string& error);
    bool create_rtvs(std::string& error);
    bool create_depth(std::string& error);
    bool create_mesh_pipeline(std::string& error);
    void record_frame(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void record_scene(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void wait_for_gpu();
    void next_frame();

    ComPtr<ID3D12InfoQueue>           info_queue_;
    ComPtr<IDXGIFactory4>             factory_;
    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        queue_;
    ComPtr<IDXGISwapChain3>           swapchain_;
    ComPtr<ID3D12DescriptorHeap>      rtv_heap_;
    ComPtr<ID3D12Resource>            targets_[kFrameCount];
    ComPtr<ID3D12CommandAllocator>    allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmd_;

    ComPtr<ID3D12DescriptorHeap>   dsv_heap_;
    ComPtr<ID3D12Resource>         depth_;
    ComPtr<ID3D12RootSignature>    mesh_root_;
    ComPtr<ID3D12PipelineState>    mesh_pso_, mesh_wire_pso_;
    // The terrain's depth prepass and its EQUAL colour pass (record_scene).
    ComPtr<ID3D12PipelineState>    mesh_prepass_pso_, mesh_eq_pso_;
    bool terrain_prepass_ = true;
    ComPtr<ID3D12RootSignature> water_root_;
    ComPtr<ID3D12PipelineState> water_pso_;
    ComPtr<ID3D12Resource>      water_indices_;
    ComPtr<ID3D12Resource>      water_cb_[kFrameCount];
    ComPtr<ID3D12Resource>      fade_buf_[kFrameCount];
    void*                       fade_cpu_[kFrameCount] = {};
    void*                       water_cb_cpu_[kFrameCount] = {};
    D3D12_INDEX_BUFFER_VIEW     water_ibv_ = {};
    UINT                        water_index_count_ = 0;
    bool create_water_pipeline(std::string& error);
    // Advance the tree field and rebuild the instance list if anything moved.
    void update_trees(const ViewConstants& view, bool settle);
    // Trees the player dug out from under drop straight down to the new
    // ground under their trunk -- not physics, they stay upright and
    // choppable. `settle` lands them at once (a still).
    void settle_trees(float time, bool settle);
    // Rocks the same way, from the support under their whole footprint: a
    // rock drops by the LEAST ground lost under its middle and eight points
    // round it, so an outcrop undercut on one side stays where it is.
    void settle_rocks(float dt, bool settle);


    // --- sun shadows ---
    // 4096: twice the old resolution in each axis. Four cascades: the fourth
    // reaches 1.6 km, where shadows used to stop at 520 m.
    static const UINT kShadowRes = 4096;
    static const UINT kCascades = 4;
    struct Cascade {
        float vp[16];
        float planes[6][4];
        float right[3], up[3];
        float texel, depth_range;
    };
    bool create_shadow_resources(std::string& error);
    // Fits the cascades `due` marks (the rest keep the matrix their map was
    // drawn with).
    void compute_cascades(const ViewConstants& view, const bool due[]);
    bool cascade_valid_[4] = {false, false, false, false};
    // Which cascades are redrawn this frame: 0 every frame, 1 every second,
    // 2 and 3 every fourth, interleaved -- two a frame, not four.
    void cascades_due(bool due[]) const;
    void record_shadows(const ViewConstants& view);
    Cascade cascades_[kCascades];
    ComPtr<ID3D12Resource>       shadow_map_;
    ComPtr<ID3D12DescriptorHeap> shadow_dsv_heap_;
    // The one shader-visible heap: [0] the shadow map, [1..2] the tree
    // impostor atlases. One heap, because only one can be bound at a time
    // and the terrain and the trees both sample the shadow.
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    // The scans' samplers, trilinear and anisotropic 2x..16x (shader_heaps.h).
    static constexpr UINT kScanSamplers = 5;
    ComPtr<ID3D12DescriptorHeap> sampler_heap_;
    UINT sampler_inc_ = 0;
    ComPtr<ID3D12Resource>       shadow_cb_[kFrameCount];
    void*                        shadow_cb_cpu_[kFrameCount] = {};
    ComPtr<ID3D12PipelineState>  mesh_shadow_pso_;
    bool shadows_on_ = true;

    // --- the post chain ---
    bool create_post_targets(std::string& error);
    bool create_post_pipelines(std::string& error);
    void record_post(D3D12_CPU_DESCRIPTOR_HANDLE backbuffer);
    ComPtr<ID3D12Resource> hdr_direct_, hdr_ambient_, scene_, ao_, history_[2];
    // The sea draws after the composite (record_water) and reads these: the
    // lit scene and depth as they were before it.
    ComPtr<ID3D12Resource> scene_copy_, depth_copy_;
    void record_water();
    bool water_pending_ = false;
    bool ssr_on_ = true;
    // The ripples: two FFT tiles a frame (ocean_detail.h). F4, --no-wave-detail.
    OceanDetail ocean_;
    // Volumetric clouds (ADR-005, clouds.h). F3, --no-clouds.
    Clouds clouds_;
    bool clouds_on_ = true;
    Rain rain_;
    Lightning lightning_;
    Strike strike_;
    bool flash_suppress_ = false;   // a still's warm-up frames, before a forced strike
    bool lightning_on_ = true;      // --no-lightning
    bool hfog_on_ = true;
    float hfog_scale_ = 1.0f;
    void height_fog_constants(float out[3][4], bool on) const;
    uint32_t last_strike_id_ = 0xFFFFFFFFu;
    unsigned strikes_seen_ = 0, ground_strikes_seen_ = 0;
    unsigned long long flash_frames_ = 0;
    void record_lightning();
    bool rain_on_ = true;
    float rain_override_ = -1.0f;
    float wet_override_ = -1.0f;
    void record_rain();
    float clouds_cam_[3] = {1e9f, 1e9f, 1e9f};
    void record_clouds();
    bool detail_on_ = true;
    float ocean_t_ = -1.0f;
    // archetypes.json's "water": what the sea's body does to light.
    struct WaterOptics {
        float extinction[3] = {0.40f, 0.11f, 0.09f};   // per metre, linear rgb
        float deep_colour[3] = {0.0012f, 0.0067f, 0.022f};   // linear
        float refraction = 0.04f;
        float storm_turbidity = 4.0f;   // extinction x (1 + this x roughness^2)
    } water_optics_;
    bool load_water_optics(const char* archetypes_path, std::string& error);
    ComPtr<ID3D12DescriptorHeap> post_rtv_heap_;
    ComPtr<ID3D12Resource> puddle_;   // the scene's third target: the puddle mask (ADR-005 C4)
    ComPtr<ID3D12RootSignature> post_root_;
    ComPtr<ID3D12PipelineState> ao_pso_, composite_pso_, taa_pso_, final_pso_, ssgi_pso_;
    // ADR-006 stage 2: the water between the eye and everything, while the
    // eye is under the sea (post.hlsl cs_underwater; cam_under_ decides).
    ComPtr<ID3D12PipelineState> underwater_pso_;
    bool  cam_under_ = false;
    float cam_under_depth_ = 0.0f;   // metres of water over the eye
    ComPtr<ID3D12Resource> ssgi_;              // the screen-space bounce (L6): irradiance, distance
    HeightGi hfgi_;
    NearField near_;
    // ADR-006 stage 3 (sea_access.h): the worker, and on the main thread its
    // answers by block (kept while a newer one is asked for, so a dig does
    // not blink the water off), what is asked and not yet answered, and the
    // blocks' columns' placed caves (lowest cave air, cached).
    SeaAccess sea_access_;
    // arrive and t0: the fill (sea_access.h) -- a wet cell counts from
    // t0 + arrive x kArriveStep on the game clock (flood_now_).
    struct FloodBlock { uint64_t gen; double t0; uint8_t cell[SeaAccess::kCells]; uint8_t arrive[SeaAccess::kCells]; };
    double flood_now_ = 0.0;
    bool  flood_arrived(const FloodBlock& b, int idx) const;
    // The edited density at the cell under sea level of columns round a
    // FILLING hole (the mask's third plane), by world column; forgotten at
    // every edit (BrickStore::generation).
    std::unordered_map<int64_t, float> flood_rim_;
    uint64_t flood_rim_gen_ = ~0ull;
    std::unordered_map<int64_t, FloodBlock> flood_;
    std::unordered_map<int64_t, uint64_t> flood_asked_;
    std::unordered_map<int64_t, float> flood_cave_lo_;
    float flood_focus_[3] = {0.0f, 0.0f, 0.0f};
    bool  flood_focus_set_ = false;
    int   flood_last_[6] = {1 << 30, 0, 0, 1 << 30, 0, 0};
    uint64_t flood_last_gen_ = 0;
    int   flood_frames_ = 0;
    void  update_flood(const float cam[3]);
    bool  flood_wanted(int bx, int by, int bz);
    int   flood_label(int x, int y, int z, uint8_t& code) const;   // 0 unknown, 1 known
    // The sea's surface mask round the camera (water.hlsl g_flood): a byte a
    // 0.5 m column (128 m a side), 0 no word, else the waves' share x 255
    // where the cell under sea level is flooded; one upload buffer a frame in
    // flight.
    static const int kFloodMask = 256;
    ComPtr<ID3D12Resource> flood_mask_[kFrameCount];
    void* flood_mask_cpu_[kFrameCount] = {};
    int   flood_mask_origin_[2] = {0, 0};
    bool  flood_mask_on_ = false;
    void  fill_flood_mask(const float cam[3]);
    FarShadow far_;
    ScanLibrary scans_;
    ForestMap forest_;
    bool forest_on_ = true;
    bool far_on_ = true;
    void record_far(const float vp[16]);
    bool hfgi_on_ = true;
    bool near_on_ = true;
    bool pom_on_ = true;
    float scan_aniso_ = 4.0f;
    ComPtr<ID3D12PipelineState> mesh_top_pso_;
    void record_hfgi(const float vp[16]);
    static bool gi_dbg_hf_off() { return false; }
    bool ssgi_on_ = false, ssgi_valid_ = false;
    float ssgi_radius_ = 30.0f;   // the dunes' bounce comes from slopes 20-50 m off; at 6 m it found almost none
    ComPtr<ID3D12PipelineState> hist_clear_pso_, hist_pso_, expo_pso_;
    // The atmosphere (ADR-004 L2): the CPU model and this frame's light, the
    // two static tables uploaded from it, and the per-frame sky-view table
    // and aerial-perspective volume (atmosphere.hlsl).
    bool create_atmosphere(std::string& error);
    bool create_atmosphere_pipelines(std::string& error);
    void update_sky(float cam_y);
    void record_atmosphere();
    void fill_light(float out[7][4]) const;
    Sky      sky_;
    SkyFrame sky_frame_;
    float    time_hours_ = 14.0f;
    float    sky_hours_ = -1.0f, sky_cam_y_ = -1.0e9f;   // what sky_frame_ was evaluated for
    // Volumetric mist (ADR-004 L4).
    bool create_fog(std::string& error);
    bool create_fog_pipelines(std::string& error);
    void record_fog();
    bool capture_raw_ = false;
    bool capture_overlay_ = false;
    CanopyMap canopy_;
    bool canopy_on_ = true, canopy_built_ = false;
    unsigned long long tree_list_version_ = 0;   // bumped whenever tree_all_ is rebuilt
    float canopy_origin_[3] = {0.0f, 0.0f, 0.0f};
    FogMap fog_map_;
    Weather      weather_;
    WeatherState weather_now_;
    WwSea        sea_ = {};
    WwSwash      swash_ = {};
    double       weather_t_ = -1.0;
    PostTimes post_times_;
    ComPtr<ID3D12QueryHeap> post_queries_;
    ComPtr<ID3D12Resource>  post_query_rb_;
    UINT64 post_query_freq_ = 0;
    bool   post_query_pending_[kFrameCount] = {};
    bool   fog_on_ = true, fog_hist_valid_ = false, fog_uploaded_ = false;
    float  fog_density_ = 1.0f;
    GroundLook ground_look_;
    UINT   fog_cur_ = 0;
    ComPtr<ID3D12Resource> fog_map_tex_, fog_map_up_, fog_inject_[2], fog_integrated_;
    // Each froxel's share of open air's light (hfgi.hlsli nf_openness), and its
    // mean along the view ray to each slice: what lights the haze and the
    // height fog along a pixel's own path, not as the eye is lit.
    ComPtr<ID3D12Resource> fog_open_, fog_open_avg_;
    bool fog_open_avg_read_ = false;
    ComPtr<ID3D12Resource> fog_cb_[kFrameCount];
    void* fog_cb_cpu_[kFrameCount] = {};
    ComPtr<ID3D12RootSignature> fog_root_;
    ComPtr<ID3D12PipelineState> fog_inject_pso_, fog_integrate_pso_;
    // Local lights (ADR-004 L3).
    bool create_lights(std::string& error);
    bool create_lights_pipelines(std::string& error);
    void record_lights(const ViewConstants& view);
    struct ShadowSlot {
        uint32_t light = 0xFFFFFFFFu;     // light id; 0 is the carried torch
        bool     ready = false;
        float    pos[3] = {0, 0, 0};
        unsigned long long drawn = 0;     // post_frame_ when its faces were drawn
        int      faces = 6;
        unsigned long long fast_until = 0;   // the near field traces its probes on the timer until then
        unsigned long long moving_until = 0; // moved (not just placed) within 60 frames: no far hits (near_field.hlsl nf_hit)
        uint32_t body_faces = 0;             // faces last drawn with the player's body in them
        unsigned long long body_serial = 0;  // the pose they were drawn with
    };
    std::vector<LocalLight> lights_;
    uint32_t next_light_id_ = 1;
    bool     torch_on_ = false, light_shadows_on_ = true;
    bool     torch_anchored_ = false;
    float    torch_anchor_[3] = {0, 0, 0};
    ShadowSlot shadow_slots_[kMaxShadowLights];
    LightFaceGpu faces_cpu_[kMaxShadowLights * 6] = {};
    float    cluster_consts_[3][4] = {};
    LightStats light_stats_;
    bool     cluster_readable_ = false;
    ComPtr<ID3D12Resource> lights_gpu_, faces_gpu_, cluster_n_, cluster_ix_, cluster_stats_, light_atlas_;
    ComPtr<ID3D12Resource> lights_up_[kFrameCount], faces_up_[kFrameCount], cluster_cb_[kFrameCount];
    ComPtr<ID3D12Resource> stats_rb_[kFrameCount];
    void* lights_up_cpu_[kFrameCount] = {};
    void* faces_up_cpu_[kFrameCount] = {};
    void* cluster_cb_cpu_[kFrameCount] = {};
    bool  stats_pending_[kFrameCount] = {};
    ComPtr<ID3D12DescriptorHeap> light_dsv_heap_;
    ComPtr<ID3D12RootSignature> cluster_root_;
    ComPtr<ID3D12PipelineState> cluster_pso_, cluster_clear_pso_;
    ComPtr<ID3D12RootSignature> atm_root_;
    ComPtr<ID3D12PipelineState> sky_view_pso_, aerial_pso_;
    ComPtr<ID3D12Resource> trans_tex_, ms_tex_, sky_view_tex_, ap_tex_;
    ComPtr<ID3D12Resource> trans_up_, ms_up_;
    ComPtr<ID3D12Resource> atm_cb_[kFrameCount];
    void* atm_cb_cpu_[kFrameCount] = {};
    bool  atm_uploaded_ = false;
    ComPtr<ID3D12Resource> lumhist_, expo_;   // 256 bins; 2 float4 of exposure state
    ComPtr<ID3D12Resource> post_cb_[kFrameCount];
    void* post_cb_cpu_[kFrameCount] = {};
    float view_proj_[16] = {};      // this frame, jittered
    float view_proj_nj_[16] = {};   // this frame, unjittered
    float prev_vp_[16] = {};        // last frame, unjittered
    double vp_nj_d_[16] = {}, prev_vp_d_[16] = {};   // the same in double, for TAA's reprojection
    float prev_cam_[3] = {0, 0, 0};
    float cur_cam_[3] = {0, 0, 0};
    // The camera's basis this frame, for the mist's froxels (L4).
    float cur_fwd_[3] = {0, 0, 1}, cur_right_[3] = {1, 0, 0}, cur_up_[3] = {0, 1, 0};
    float cur_tan_ = 1.0f, cur_aspect_ = 1.0f, cur_time_ = 0.0f;
    float jitter_[2] = {0, 0};
    float prev_jitter_[2] = {0, 0};   // last frame's, for the shadow denoiser's reprojection
    UINT  hist_cur_ = 0;
    bool  hist_valid_ = false;
    unsigned long long post_frame_ = 0;
    // The sun and sky last frame, for the GI's histories (record_scene).
    bool  have_prev_light_ = false;
    float prev_key_ = 0.0f, prev_sky_ = 0.0f, prev_key_dir_[3] = {0.0f, 1.0f, 0.0f};
    float light_change_ = 0.0f;
    bool  taa_on_ = true, ao_on_ = true, contact_on_ = true;
    CrackCount cracks_;
    float post_debug_ = 0.0f;
    float ev_comp_ = 0.0f, ev100_fixed_ = -1000.0f, agx_saturation_ = 1.4f, agx_power_ = 1.35f;
    DarkVision dark_;
    std::function<float(const float from[3], const float dir[3], float len)> ray_query_;
    float light_presence_ = 0.0f, light_lux_eye_ = 0.0f;
    float post_dt_override_ = -1.0f;
    float live_expo_[8] = {};   // u_expo[0] (adapted, exposure, EV100, target) and [1] (the band in nits)
    ComPtr<ID3D12Resource> expo_live_rb_[kFrameCount];
    void* expo_live_cpu_[kFrameCount] = {};
    void read_live_exposure(UINT slot);
    unsigned tonemap_ = 1;
    bool  exposure_snap_ = false;            // a still: adapt instantly
    unsigned long long last_post_tick_ = 0;
    ExposureReport exposure_report_;
    SkyCheck sky_check_;
    CloudShadowReport cloud_shadow_report_;
    TreeField        tree_field_;
    RockField        rock_field_;
    RockRenderer     rocks_;
    bool             rocks_on_ = true;
    GlowMushrooms    mushrooms_;
    bool             mushrooms_on_ = true;
    CharacterRenderer character_;
    std::string      character_pack_ = "assets/models/packed/male.chr";
    TreeRenderer     trees_;
    GpuProfiler      prof_;
    TreeDrawSettings tree_settings_;
    FoliageRenderer  foliage_;
    FoliageSettings  foliage_settings_;
    bool foliage_on_ = true;
    std::vector<TreeGpuInstance> tree_extras_;
    std::vector<TreeGpuInstance> tree_all_;
    std::unordered_map<uint32_t, float> tree_hits_;
    unsigned long long tree_field_seen_ = 0;
    struct TreeSink {
        float orig = 0.0f;     // the generated base
        float target = 0.0f;   // the ground under the trunk now
        float y = 0.0f, v = 0.0f;
        uint64_t gen = 0;      // the edit generation `target` was found for
    };
    std::unordered_map<uint32_t, TreeSink> tree_sink_;
    uint64_t sink_gen_seen_ = ~0ull;
    unsigned long long sink_field_seen_ = ~0ull;
    float sink_time_ = -1.0f;
    struct RockSink {
        float target = 0.0f;   // metres it must drop
        float drop = 0.0f, v = 0.0f;
        uint64_t gen = 0;
    };
    std::unordered_map<uint32_t, RockSink> rock_sink_;
    uint64_t rock_gen_seen_ = ~0ull;
    unsigned long long rock_field_seen_ = ~0ull;
    bool tree_dirty_ = true;
    bool trees_on_ = true;
    ComPtr<ID3D12CommandSignature> cmd_sig_;

    WorldGpu    world_;
    ChunkMesher mesher_;
    Streamer    streamer_;
    UINT        mesh_budget_ = kMeshBudgetPerFrame;
    // OFF by default, measured: it took 60 m/s detail latency from 3 to 2
    // frames at the 90th percentile and the 99th-percentile frame from 21 to
    // 27 ms, with the GPU's meshing time unchanged -- a CPU cost that grows
    // with chunks per frame, not yet found. --adaptive-budget turns it on.
    bool        adaptive_budget_ = false;
    BrickStore  edits_;
    BrickPager  pager_;
    std::vector<BrickKey> dirty_bricks_;
    std::vector<BrickKey> brick_uploads_;
    std::vector<PatchEvent> patches_added_;
    std::vector<MeshPatchEvent> mesh_patches_added_;
    std::vector<ChunkKey>   patches_removed_;
    FrameStats  stats_;
    Mode        mode_ = Mode::Mesh;
    bool        water_ = true;
    float       water_debug_ = 0.0f;
    float       mesh_debug_ = 0.0f;
    float       texture_mode_ = 1.0f;

    ComPtr<ID3D12Fence> fence_;
    HANDLE              fence_event_ = nullptr;
    UINT64              fence_values_[kFrameCount] = {0, 0};
    UINT                frame_ = 0;

    UINT rtv_stride_ = 0;
    UINT width_ = 0, height_ = 0;
    // One record per staging buffer: what was copied into it and when, so the
    // consumer knows which chunk each patch belongs to.
    struct RingRecord {
        UINT     count = 0;
        unsigned long long frame = 0;
        ChunkKey keys[kReadbackChunksPerFrame];
        UINT     slot[kReadbackChunksPerFrame];
        float    origin[kReadbackChunksPerFrame][3];
        float    voxel[kReadbackChunksPerFrame];
    };
    RingRecord ring_[kReadbackRing];

    // The same shape again for the mesh path, which is staged a frame later
    // than the heights because it cannot start until the classification has
    // come back.
    struct MeshRingRecord {
        UINT     count = 0;
        unsigned long long frame = 0;
        ChunkKey keys[kMeshChunksPerFrame];
        UINT     slots[kMeshChunksPerFrame];
    };
    MeshRingRecord mesh_ring_[kReadbackRing];
    std::vector<ChunkKey> mesh_wanted_;

    unsigned long long frame_index_ = 0;
    void stage_heights();
    void consume_heights();
    void stage_meshes();
    void consume_meshes();

    bool ready_ = false;
    bool vsync_ = true;
    Overlay overlay_;
    bool tearing_ok_ = false;
    unsigned long long adapter_vram_ = 0;   // dedicated video memory, bytes
};

}  // namespace engine

#endif  // ENGINE_RENDERER_H
