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
#include "engine/render/rt_check.h"
#include "engine/render/sky.h"
#include "engine/render/local_lights.h"
#include "engine/render/fog.h"
#include "engine/render/rt_scene.h"
#include "engine/render/canopy.h"
#include "engine/render/ddgi.h"
#include "engine/render/height_gi.h"
#include "engine/render/far_shadow.h"
#include "engine/render/scan_library.h"
#include "engine/rocks/rock_field.h"
#include "engine/rocks/rock_renderer.h"

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
    void resize(UINT width, UINT height);
    void shutdown();

    // Recompiles every shader from disk. The whole of Track B's remaining work
    // is shader editing, so this stays reachable.
    bool reload_shaders(std::string& error);

    // Off for benchmarking: with vsync on, every measurement reads 60 and
    // says nothing about how much headroom there is.
    void set_vsync(bool on) { vsync_ = on; }
    bool tearing_available() const { return tearing_ok_; }
    // What the slot pool is actually holding, after a capture: per-chunk
    // vertex and index counts against the caps, and memory against the GPU.
    void slot_report(const char* path);
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    const FrameStats& stats() const { return stats_; }

    const WorldGpu& world() const { return world_; }

    // Sea state: 0 calm, 1 storm. Storms are a planned mechanic, so this is a
    // runtime value and nothing about the surface shape is baked.
    void  set_sea_state(float s) { sea_state_ = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s); }
    float sea_state() const { return sea_state_; }
    void  set_water(bool on) { water_ = on; }
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
    const TreeRenderer&    trees() const { return trees_; }
    TreeDrawSettings&      tree_settings() { return tree_settings_; }
    void set_trees(bool on) { trees_on_ = on; }
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
    void set_tonemap(unsigned mode) { tonemap_ = mode; }
    unsigned tonemap() const { return tonemap_; }
    void set_agx_saturation(float s) { agx_saturation_ = s; }
    void set_agx_power(float p) { agx_power_ = p; }
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
    uint32_t add_light(const LocalLight& l) {
        lights_.push_back(l);
        lights_.back().id = next_light_id_++;
        return lights_.back().id;
    }
    void clear_lights() { lights_.clear(); }
    size_t light_count() const { return lights_.size() + (torch_on_ ? 1 : 0); }
    void set_light_shadows(bool on) { light_shadows_on_ = on; }
    bool light_shadows() const { return light_shadows_on_; }
    struct LightStats {
        uint32_t lights = 0;          // sent to the GPU this frame
        uint32_t shadowed = 0;        // with a shadow ready
        uint32_t faces_drawn = 0;     // shadow faces redrawn this frame
        uint32_t chunk_draws = 0;     // terrain chunk draws in those faces
        // From the cluster pass, read back a couple of frames late.
        uint32_t overflowed = 0, max_in_cluster = 0, clusters_lit = 0, cluster_items = 0;
    };
    const LightStats& light_stats() const { return light_stats_; }

    // Volumetric mist (ADR-004 L4). On by default; N in the window and
    // --no-fog for A/B; a density multiplier for tuning.
    void set_fog(bool on) { fog_on_ = on; fog_hist_valid_ = false; }
    bool fog() const { return fog_on_; }
    void set_fog_density(float k) { fog_density_ = k; }
    const FogMap& fog_map() const { return fog_map_; }
    // GPU time of the post chain's parts, ms, smoothed over frames: the
    // atmosphere's tables, the mist's inject and integrate, the whole chain.
    // Frame-time A/B cannot resolve passes this small on a noisy machine.
    struct PostTimes { float atmosphere = 0, mist_inject = 0, mist_integrate = 0, post_total = 0; };
    const PostTimes& post_times() const { return post_times_; }

    // The ray-tracing scene (ADR-004 L5). Off until asked for: --rt-shadows
    // (R in the window) traces the key light's shadow in place of the
    // cascades; the RT instruments run whenever it is on.
    bool rt_available() const { return rt_ok_; }
    void set_rt_shadows(bool on);
    bool rt_shadows() const { return rt_shadows_; }
    void set_rt_active(bool on);   // the scene and the trace, without replacing the cascades
    void set_rt_checks(bool on) { rt_checks_ = on; }   // the CSM and normal instruments (--rt-report)
    // L6: the ambient scaled by a traced sky visibility -- terrain, trunks and
    // the crown volumes (--rt-sky, Shift+R). The mask picks what the rays see
    // (RtScene::kMask*), for A/B; the crowns enter the TLAS only when it
    // includes them.
    void set_rt_sky(bool on);
    bool rt_sky() const { return rt_sky_; }
    void set_rt_sky_mask(unsigned mask);
    void set_rt_sky_rays(int n) { rt_sky_rays_ = n < 1 ? 1 : (n > 16 ? 16 : n); }
    // L6's no-RT canopy shade: the canopy map (canopy.h) in gi_sky_visibility.
    // On by default; the traced sky takes over while --rt-sky is on.
    void set_canopy(bool on) { canopy_on_ = on; }
    bool canopy() const { return canopy_on_; }
    unsigned canopy_trees() const { return canopy_.trees_last_frame(); }
    unsigned long long canopy_builds() const { return canopy_.builds(); }
    // DDGI (ADR-004 L6): ray-traced probe GI. OFF by default since its
    // +2.8 ms walking was judged too dear (--gi, I); kept as the reference
    // the screen-space GI is measured against. Off, gi_diffuse is the
    // fallback with the canopy map and screen-space bounce.
    void set_gi(bool on);
    bool gi() const { return gi_on_; }
    bool gi_active() const { return gi_on_ && rt_ok_ && ddgi_.ready() && mode_ != Mode::Wireframe; }
    // The screen-space bounce (ADR-004 L6's no-RT experiment): OFF by
    // default (--ssgi, X). It measured 1-2% more light in shade for 0.3 ms --
    // the bounce that matters here comes from terrain and canopy off screen
    // or behind the camera. Read by gi_diffuse where DDGI does not answer.
    void set_ssgi(bool on) { ssgi_on_ = on; if (!on) ssgi_valid_ = false; }
    // The heightfield GI (ADR-004 L6, height_gi.h): the default diffuse GI
    // where DDGI is off (--no-hfgi, Z).
    void set_hfgi(bool on) { hfgi_on_ = on; }
    // Parallax occlusion on the scans' height (pom.hlsli; --no-pom, P).
    void set_pom(bool on) { pom_on_ = on; }
    bool pom() const { return pom_on_; }
    bool hfgi() const { return hfgi_on_; }
    unsigned long long hfgi_rebuilds() const { return hfgi_.rebuilds(); }
    // The far terrain shadow past the cascades (far_shadow.h; --no-far-shadow).
    void set_far_shadow(bool on) { far_on_ = on; }
    bool ssgi() const { return ssgi_on_; }
    float gi_ms() const { return ddgi_ms_; }
    // Whether the probes see local lights (--leak-test's A/B: the torch's
    // bounce against its direct light alone).
    void set_gi_local_lights(bool on) { gi_local_lights_ = on; }
    unsigned long long gi_updates() const { return ddgi_.updates(); }
    UINT64 gi_bytes() const { return ddgi_.bytes(); }
    const Ddgi::CameraIrradiance& gi_camera_irradiance() const { return ddgi_.camera_irradiance(); }
    float gi_camera_sky() const { return ddgi_.camera_sky(); }
    bool gi_counts(Ddgi::Counts& out, std::string& error, const float centre[3] = nullptr, float radius = 0.0f) {
        wait_for_gpu();
        return ddgi_.count(queue_.Get(), out, error, centre, radius);
    }
    const RtScene::Stats& rt_scene_stats() const { return rt_.stats(); }
    struct RtStats {
        uint32_t lit_lit = 0, shadow_shadow = 0, rt_only = 0, csm_only = 0;
        uint32_t primary_agree = 0, primary_nearer = 0, primary_farther = 0;
        float normal_err_deg = 0.0f;
        float sky_mean = 0.0f, sky_near_mean = 0.0f;   // L6: traced sky visibility, inside the cascades' reach / 60 m
        uint32_t sky_pixels = 0, sky_near_pixels = 0;
        // The canopy map against the traced sky on the same terrain pixels.
        float canopy_rt_mean = 0.0f, canopy_map_mean = 0.0f, canopy_mae = 0.0f;
        uint32_t canopy_pixels = 0;
        float build_ms = 0.0f, trace_ms = 0.0f;   // GPU, smoothed
    };
    const RtStats& rt_stats() const { return rt_stats_; }

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

    // What the device can do (ADR-004): ray-tracing tier, shader model.
    const DeviceCaps& caps() const { return caps_; }
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
    ComPtr<ID3D12Resource>       shadow_cb_[kFrameCount];
    void*                        shadow_cb_cpu_[kFrameCount] = {};
    ComPtr<ID3D12PipelineState>  mesh_shadow_pso_;
    bool shadows_on_ = true;

    // --- the post chain ---
    bool create_post_targets(std::string& error);
    bool create_post_pipelines(std::string& error);
    void record_post(D3D12_CPU_DESCRIPTOR_HANDLE backbuffer);
    ComPtr<ID3D12Resource> hdr_direct_, hdr_ambient_, scene_, ao_, history_[2];
    ComPtr<ID3D12DescriptorHeap> post_rtv_heap_;
    ComPtr<ID3D12RootSignature> post_root_;
    ComPtr<ID3D12PipelineState> ao_pso_, composite_pso_, taa_pso_, final_pso_, ssgi_pso_;
    ComPtr<ID3D12Resource> ssgi_;              // the screen-space bounce (L6): irradiance, distance
    HeightGi hfgi_;
    FarShadow far_;
    ScanLibrary scans_;
    bool far_on_ = true;
    void record_far(const float vp[16]);
    bool hfgi_on_ = true;
    bool pom_on_ = true;
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
    // The ray-tracing scene (ADR-004 L5).
    bool create_rt(std::string& error);
    void record_rt_trace();
    bool capture_raw_ = false;
    RtScene rt_;
    Ddgi ddgi_;
    bool gi_on_ = false, gi_local_lights_ = true;
    void record_ddgi();
    void read_ddgi_time();
    ComPtr<ID3D12QueryHeap> ddgi_queries_;
    ComPtr<ID3D12Resource>  ddgi_query_rb_;
    bool  ddgi_query_pending_[kFrameCount] = {};
    float ddgi_ms_ = 0.0f;
    CanopyMap canopy_;
    bool canopy_on_ = true, canopy_built_ = false;
    unsigned long long tree_list_version_ = 0;   // bumped whenever tree_all_ is rebuilt
    float canopy_origin_[3] = {0.0f, 0.0f, 0.0f};
    bool rt_ok_ = false, rt_shadows_ = false, rt_active_ = false, rt_seed_ = false, rt_checks_ = false;
    bool rt_sky_ = false;
    unsigned rt_sky_mask_ = RtScene::kMaskTerrain | RtScene::kMaskTrees | RtScene::kMaskCrowns;
    int rt_sky_rays_ = 1;
    RtStats rt_stats_;
    ComPtr<ID3D12RootSignature> rt_root_;
    ComPtr<ID3D12PipelineState> rt_pso_, rt_clear_pso_;
    ComPtr<ID3D12Resource> rt_vis_, rt_stats_buf_, rt_stats_rb_[kFrameCount], rt_cb_[kFrameCount];
    void* rt_cb_cpu_[kFrameCount] = {};
    bool  rt_stats_pending_[kFrameCount] = {};
    ComPtr<ID3D12QueryHeap> rt_queries_;
    ComPtr<ID3D12Resource>  rt_query_rb_;
    bool  rt_query_pending_[kFrameCount] = {};
    FogMap fog_map_;
    PostTimes post_times_;
    ComPtr<ID3D12QueryHeap> post_queries_;
    ComPtr<ID3D12Resource>  post_query_rb_;
    UINT64 post_query_freq_ = 0;
    bool   post_query_pending_[kFrameCount] = {};
    bool   fog_on_ = true, fog_hist_valid_ = false, fog_uploaded_ = false;
    float  fog_density_ = 1.0f;
    UINT   fog_cur_ = 0;
    ComPtr<ID3D12Resource> fog_map_tex_, fog_map_up_, fog_inject_[2], fog_integrated_;
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
    };
    std::vector<LocalLight> lights_;
    uint32_t next_light_id_ = 1;
    bool     torch_on_ = false, light_shadows_on_ = true;
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
    UINT  hist_cur_ = 0;
    bool  hist_valid_ = false;
    unsigned long long post_frame_ = 0;
    bool  taa_on_ = true, ao_on_ = true, contact_on_ = true;
    DeviceCaps caps_;
    CrackCount cracks_;
    float post_debug_ = 0.0f;
    float ev_comp_ = 0.0f, ev100_fixed_ = -1000.0f, agx_saturation_ = 1.4f, agx_power_ = 1.35f;
    unsigned tonemap_ = 1;
    bool  exposure_snap_ = false;            // a still: adapt instantly
    unsigned long long last_post_tick_ = 0;
    ExposureReport exposure_report_;
    SkyCheck sky_check_;
    TreeField        tree_field_;
    RockField        rock_field_;
    RockRenderer     rocks_;
    bool             rocks_on_ = true;
    TreeRenderer     trees_;
    TreeDrawSettings tree_settings_;
    FoliageRenderer  foliage_;
    FoliageSettings  foliage_settings_;
    bool foliage_on_ = true;
    std::vector<TreeGpuInstance> tree_extras_;
    std::vector<TreeGpuInstance> tree_all_;
    std::unordered_map<uint32_t, float> tree_hits_;
    unsigned long long tree_field_seen_ = 0;
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
    float       sea_state_ = 0.25f;
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
    bool tearing_ok_ = false;
    unsigned long long adapter_vram_ = 0;   // dedicated video memory, bytes
};

}  // namespace engine

#endif  // ENGINE_RENDERER_H
