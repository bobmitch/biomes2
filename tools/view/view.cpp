// ---------------------------------------------------------------------------
// Native world viewer (ADR-002).
//
// Links worldgen/ directly, so what is on screen is the shipping generator
// rather than a translation of it onto another backend. That is the whole
// argument for replacing the browser prototype: the thing being tuned against
// is now the thing that ships.
//
// Raw Win32 on purpose -- no dependencies.
//
// What it is for: deciding whether seeds make good worlds to PLAY. So beside
// the maps it measures the things a player meets -- where they start, what
// they can walk to, how far each biome is, where the level ground is, how
// danger climbs with distance -- for the seed on screen (the right panel) and
// over many seeds at once (the scan, also written to build/seed_scan.csv).
//
// - The world is built at grid 1024, the engine's own, so the spawn, the
//   biomes and the flat sites are the ones the engine will build -- as long
//   as the sliders are at the shipping values (the status says when not).
//   A slider being dragged previews at 512.
// - archetypes.json is watched: save it in an editor and the world rebuilds.
// - Wheel zooms about the cursor, drag pans, Home fits the disc. Click pins a
//   point for the inspector; right click drops a measuring marker.
// - "Open in engine" starts build/engine.exe over the pinned point (or the
//   spawn) on this seed (`--seed`, `--cam`), with the sliders' world: any
//   slider off shipping goes to the engine as BIOMES_L0. "Copy command" puts
//   that command and the matching validate line on the clipboard.
// - "Layer 0 prototype..." opens the proposal's controls (docs/worldgen-
//   design.md, "Layer 0, proposed"): the regime field, the fine continent
//   noise, per-seed draws, the shelf and the land knee. All start at today's
//   world.
//
// Rendering is threaded by rows. A relief pixel is three terrain heights
// (~2 us), so a full frame is ~1.4 s on one core and ~0.1 s on sixteen.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // windows.h defines min/max as macros, which breaks std::min
#include <windows.h>
#include <commctrl.h>

#include "worldgen/world.h"
#include "tools/shared/render.h"
#include "tools/shared/land_solve.h"
#include "tools/shared/seed_metrics.h"
#include "tools/shared/reach.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace worldgen;
using namespace view;

namespace {

enum {
    ID_SEED_PREV = 100, ID_SEED_NEXT, ID_SEED_EDIT, ID_SEED_GO,
    ID_W, ID_FEAT, ID_MODE, ID_LANDOFF, ID_RESET,
    ID_OV_SPAWN, ID_OV_RINGS, ID_OV_RIVERS, ID_OV_FLATS, ID_OV_PROPS, ID_OV_GRID,
    ID_HIGHLIGHT, ID_RELOAD, ID_AUTO_RELOAD, ID_ENGINE, ID_COPY,
    ID_SCAN, ID_SCAN_COUNT, ID_SCAN_LIST, ID_L0_OPEN,
    // The layer 0 window's controls.
    ID_L0_DRAW, ID_L0_KNEE, ID_L0_RECAL, ID_L0_SLIDER0 = 300
};
enum { TIMER_REFINE = 1, TIMER_WATCH = 2, TIMER_RESIZE = 3 };
enum { WM_APP_SCAN_ROW = WM_APP + 1, WM_APP_SCAN_DONE = WM_APP + 2 };

// The islandness slider reads in LAND FRACTION, tenths of a percent, because
// that is the number that decides whether the world is a continent or an
// archipelago. land_offset is an opaque proxy for it.
//
// The offset is read off a calibration curve built at startup, so the solve is
// GLOBAL: the slider sets the MEAN land fraction across seeds and leaves each
// seed free to land above or below. Per-seed solving would flatten exactly the
// spread the islandness comes from.
const int LAND_MIN = 250;    // 25.0%
const int LAND_MAX = 550;    // 55.0%

const int LEFT = 262;        // control panel
const int RIGHT = 410;       // inspector, this seed, scan
const int LEGEND_Y = 560;    // where the left panel's legend starts

const int kGridFull = 1024;  // the engine's grid: same cells, same spawn
const int kGridPreview = 512;
const int kPreviewScale = 3; // a preview renders a third of the pixels each way
const float kPropsMpp = 3.0f;  // trees and rocks drawn from this zoom in

const char* kArchetypes = "worldgen/data/archetypes.json";

struct Props {
    std::vector<TreeInstance> trees;
    std::vector<RockInstance> rocks;
};

struct App {
    WorldParams  wp;           // what the world is built with (sliders applied)
    WorldParams  shipping;     // the defaults, which the engine uses
    AssignParams ap;
    ArchetypeTable table;
    SeedWorld    world;
    LandCalibration calib;
    WorldParams  calib_wp;     // the layer 0 settings the curve was built at
    bool         calib_today = true;
    uint64_t     seed = 0;
    ViewMode     mode = VIEW_BIOME_RELIEF;
    bool         world_dirty = true;
    int          built_grid = 0;
    double       build_ms = 0.0, render_ms = 0.0;

    SpawnPoint   spawn;
    SeedMetrics  metrics;
    bool         metrics_valid = false;
    FootReach    reach;        // tools/shared/reach.h: on foot, wading included
    int          highlight = -1;

    // The view: world point at the map's centre, metres per screen pixel.
    float cx = 0.0f, cz = 0.0f, mpp = 25.0f;

    // The last rendered image and the view it was rendered at, so a pan or a
    // zoom can stretch it at once while a fresh one is made.
    std::vector<unsigned char> bgra;
    int   img_w = 0, img_h = 0, img_scale = 1;
    float img_cx = 0.0f, img_cz = 0.0f, img_mpp = 1.0f;
    int   img_map_w = 0, img_map_h = 0;
    bool  img_full = false;

    Props props;
    bool  props_shown = false;

    // Pointer state.
    bool  dragging = false, panned = false;
    int   drag_x = 0, drag_y = 0;
    float drag_cx = 0.0f, drag_cz = 0.0f;
    bool  hover = false;
    float hx = 0.0f, hz = 0.0f;
    bool  pinned = false;
    float px = 0.0f, pz = 0.0f;
    bool  marker = false;
    float mx = 0.0f, mz = 0.0f;

    // archetypes.json watching.
    FILETIME arch_time = {0, 0};
    std::string reload_note;

    // The scan.
    std::thread scan_thread;
    std::atomic<bool> scan_cancel{false};
    bool  scanning = false;
    std::vector<SeedMetrics> scan_rows;
    std::vector<std::string> scan_names;   // biome names the scan was run with
    std::string scan_summary;

    HWND hwnd = 0;
    HWND w_bar = 0, feat_bar = 0, land_bar = 0, mode_box = 0, hl_box = 0, status = 0;
    HWND seed_edit = 0, auto_reload = 0;
    HWND ov_spawn = 0, ov_rings = 0, ov_rivers = 0, ov_flats = 0, ov_props = 0, ov_grid = 0;
    HWND inspect = 0, seed_info = 0, scan_btn = 0, scan_count = 0, scan_list = 0, scan_label = 0;
    HWND right_labels[3] = {0, 0, 0};
    HWND scan_note = 0;
    HFONT ui_font = 0, mono_font = 0;

    // The layer 0 prototype's window.
    HWND l0_wnd = 0, l0_draw = 0, l0_knee = 0, l0_status = 0;
};

// A slider over one float parameter in the layer 0 window. The trackbar
// holds integers, so value = pos * scale; `metres_to_freq` shows a feature
// size in metres and sets a frequency of 1 / size.
struct L0Slider {
    const char* name;
    float scale;
    float* target;
    bool metres_to_freq;
    const char* fmt;
    HWND bar, lab;
};
std::vector<L0Slider> g_l0;

App g;

// --- small helpers ------------------------------------------------------------

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

std::string metres(float d) {
    if (d < 0.0f) return "-";
    if (d < 1000.0f) return fmt("%.0f m", static_cast<double>(d));
    return fmt("%.1f km", static_cast<double>(d / 1000.0f));
}

std::string biome_name(int b) {
    if (b >= 0 && b < static_cast<int>(g.table.size())) return g.table.biomes[b].name;
    return b == -1 ? "(ocean cell)" : "-";
}

bool checked(HWND h) { return SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

RECT map_rect() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    RECT m = {LEFT, 0, std::max<LONG>(LEFT + 64, rc.right - RIGHT), rc.bottom};
    return m;
}
int map_w() { const RECT m = map_rect(); return std::max<int>(1, m.right - m.left); }
int map_h() { const RECT m = map_rect(); return std::max<int>(1, m.bottom - m.top); }

// Screen (client) <-> world, through the current view.
void to_world(int sx, int sy, float& x, float& z) {
    const RECT m = map_rect();
    x = g.cx + (static_cast<float>(sx - m.left) - 0.5f * map_w()) * g.mpp;
    z = g.cz + (static_cast<float>(sy - m.top) - 0.5f * map_h()) * g.mpp;
}
void to_screen(float x, float z, int& sx, int& sy) {
    const RECT m = map_rect();
    sx = m.left + static_cast<int>(std::floor((x - g.cx) / g.mpp + 0.5f * map_w()));
    sy = m.top + static_cast<int>(std::floor((z - g.cz) / g.mpp + 0.5f * map_h()));
}
bool in_map(int sx, int sy) {
    const RECT m = map_rect();
    return sx >= m.left && sx < m.right && sy >= m.top && sy < m.bottom;
}

void fit_view() {
    g.cx = 0.0f;
    g.cz = 0.0f;
    g.mpp = 2.04f * g.wp.radius / static_cast<float>(std::min(map_w(), map_h()));
}

// Whether two parameter sets give the same layer 0, and so the same land
// fraction curve. The offset is left out: it is what the curve varies.
bool same_layer0(const WorldParams& a, const WorldParams& b) {
    return a.continent_freq == b.continent_freq && a.continent_octaves == b.continent_octaves &&
           a.centre_floor == b.centre_floor && a.fine_freq == b.fine_freq &&
           a.fine_octaves == b.fine_octaves && a.fine_weight == b.fine_weight &&
           a.fine_regime == b.fine_regime && a.regime_freq == b.regime_freq &&
           a.regime_mean == b.regime_mean && a.regime_amp == b.regime_amp &&
           a.regime_draw == b.regime_draw &&
           a.regime_size_lo == b.regime_size_lo && a.regime_size_hi == b.regime_size_hi &&
           a.regime_mean_lo == b.regime_mean_lo && a.regime_mean_hi == b.regime_mean_hi &&
           a.regime_amp_lo == b.regime_amp_lo && a.regime_amp_hi == b.regime_amp_hi &&
           a.shelf_width == b.shelf_width && a.shelf_depth == b.shelf_depth &&
           a.shelf_regime == b.shelf_regime &&
           a.knee == b.knee && a.knee_k == b.knee_k && a.knee_c == b.knee_c;
}

bool params_modified() {
    return g.wp.danger_w != g.shipping.danger_w ||
           g.wp.danger_freq != g.shipping.danger_freq ||
           g.wp.land_offset != g.shipping.land_offset ||
           !same_layer0(g.wp, g.shipping);
}

float ground_at(float x, float z) {
    const float rr = std::sqrt(x * x + z * z) / g.wp.radius;
    if (rr > 1.0f) return 0.0f;
    return terrain_height(g.world, g.wp, g.table, x, z, rr);
}

// --- the world ------------------------------------------------------------------

void ensure_world(int grid) {
    if (!g.world_dirty && g.built_grid >= grid) return;
    const auto t0 = std::chrono::steady_clock::now();
    g.wp.grid = grid;
    g.world = build_world(g.seed, g.wp, g.table, g.ap);
    g.spawn = find_spawn(g.world, g.wp, g.table);
    g.built_grid = grid;
    g.world_dirty = false;
    g.metrics_valid = false;
    g.props = Props();
    g.props_shown = false;
    if (grid >= kGridFull) {
        g.metrics = measure_seed(g.world, g.wp, g.table, g.spawn, g.seed, default_threads());
        g.metrics_valid = true;
        g.reach = foot_reach(g.world, g.wp, g.spawn.x, g.spawn.z, 384);
    }
    g.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Trees and rocks inside the view. Layer 8 and 9 are a CPU decision per 4 m
// cell, so only worth it zoomed in; threaded by columns of cells.
void compute_props() {
    g.props = Props();
    g.props_shown = false;
    if (!checked(g.ov_props) || g.mpp > kPropsMpp) return;
    const float hw = 0.5f * map_w() * g.mpp, hh = 0.5f * map_h() * g.mpp;
    const float x0 = g.cx - hw, x1 = g.cx + hw, z0 = g.cz - hh, z1 = g.cz + hh;
    const int threads = default_threads();
    std::vector<Props> part(threads);

    const float tc = g.table.trees.cell;
    const int ti0 = static_cast<int>(std::floor(x0 / tc)), ti1 = static_cast<int>(std::floor(x1 / tc)) + 1;
    const int tj0 = static_cast<int>(std::floor(z0 / tc)), tj1 = static_cast<int>(std::floor(z1 / tc)) + 1;
    const int strips = std::max(1, (ti1 - ti0 + 15) / 16);
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = next++; s < strips; s = next++) {
                const int a = ti0 + s * 16, b = std::min(ti1, a + 16);
                trees_in_cells(g.world, g.wp, g.table, a, tj0, b, tj1, part[t].trees);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
    pool.clear();

    if (g.table.rocks.present) {
        for (int kind = 0; kind < 2; ++kind) {
            const float rc = kind == 0 ? g.table.rocks.boulder.cell : g.table.rocks.outcrop.cell;
            // An outcrop's cell origin can lie outside the view while the
            // rock reaches into it: one cell of margin.
            const int i0 = static_cast<int>(std::floor(x0 / rc)) - 1, i1 = static_cast<int>(std::floor(x1 / rc)) + 2;
            const int j0 = static_cast<int>(std::floor(z0 / rc)) - 1, j1 = static_cast<int>(std::floor(z1 / rc)) + 2;
            const int rs = std::max(1, (i1 - i0 + 7) / 8);
            next = 0;
            for (int t = 0; t < threads; ++t) {
                pool.push_back(std::thread([&, t, kind, i0, i1, j0, j1, rs]() {
                    for (int s = next++; s < rs; s = next++) {
                        const int a = i0 + s * 8, b = std::min(i1, a + 8);
                        rocks_in_cells(g.world, g.wp, g.table, kind, a, j0, b, j1, part[t].rocks);
                    }
                }));
            }
            for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
            pool.clear();
        }
    }
    for (int t = 0; t < threads; ++t) {
        g.props.trees.insert(g.props.trees.end(), part[t].trees.begin(), part[t].trees.end());
        g.props.rocks.insert(g.props.rocks.end(), part[t].rocks.begin(), part[t].rocks.end());
    }
    g.props_shown = true;
}

void disc(std::vector<unsigned char>& img, int W, int H, float cxp, float cyp, float r,
          unsigned char b, unsigned char gg, unsigned char rr) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cxp - r)));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(cxp + r)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cyp - r)));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(cyp + r)));
    const float r2 = std::max(0.25f, r * r);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cxp, dy = y + 0.5f - cyp;
            if (dx * dx + dy * dy > r2) continue;
            unsigned char* p = &img[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = b; p[1] = gg; p[2] = rr;
        }
    }
}

// Props go INTO the image rather than over it: there can be 100k trees, and
// the image is redrawn when the overlay is toggled anyway.
void draw_props() {
    if (!g.props_shown) return;
    const float hwx = 0.5f * g.img_map_w * g.img_mpp, hhz = 0.5f * g.img_map_h * g.img_mpp;
    const float ox = g.img_cx - hwx, oz = g.img_cz - hhz;
    const float s = 1.0f / (g.img_mpp * g.img_scale);
    for (size_t i = 0; i < g.props.trees.size(); ++i) {
        const TreeInstance& t = g.props.trees[i];
        // A crown's radius is about a sixth of the tree's height.
        const float r = std::max(0.7f, t.height * 0.17f * s);
        unsigned char c[3];
        biome_colour(static_cast<int>(t.species) * 3 + 11, c);   // a hue per species
        disc(g.bgra, g.img_w, g.img_h, (t.x - ox) * s, (t.z - oz) * s, r,
             static_cast<unsigned char>(c[2] * 0.30f), static_cast<unsigned char>(c[1] * 0.42f),
             static_cast<unsigned char>(c[0] * 0.30f));
    }
    for (size_t i = 0; i < g.props.rocks.size(); ++i) {
        const RockInstance& k = g.props.rocks[i];
        const float r = std::max(k.kind ? 1.5f : 0.6f, 0.5f * k.size * s);
        const unsigned char v = k.kind ? 185 : 150;
        disc(g.bgra, g.img_w, g.img_h, (k.x - ox) * s, (k.z - oz) * s, r, v - 12, v - 6, v);
    }
}

void render_map(bool full) {
    ensure_world(full ? kGridFull : kGridPreview);
    if (full) compute_props();
    else { g.props = Props(); g.props_shown = false; }

    const auto t0 = std::chrono::steady_clock::now();
    const int scale = full ? 1 : kPreviewScale;
    const int mw = map_w(), mh = map_h();
    const int W = std::max(1, mw / scale), H = std::max(1, mh / scale);
    g.bgra.assign(static_cast<size_t>(W) * H * 4, 255);
    const float mpp = g.mpp, cx = g.cx, cz = g.cz;
    const ViewMode mode = g.mode;
    const int hl = (mode == VIEW_BIOME || mode == VIEW_BIOME_RELIEF) ? g.highlight : -1;
    parallel_rows(H, default_threads(), [&](int py) {
        const float z = cz + ((py + 0.5f) * scale - 0.5f * mh) * mpp;
        for (int i = 0; i < W; ++i) {
            const float x = cx + ((i + 0.5f) * scale - 0.5f * mw) * mpp;
            unsigned char c[3];
            const int b = view_pixel(g.world, g.wp, g.table, mode, x, z, c);
            if (hl >= 0 && b >= 0 && b != hl) {
                // Dim, and grey, every other biome so one reads at a glance.
                const float l = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
                for (int ch = 0; ch < 3; ++ch) c[ch] = static_cast<unsigned char>(0.35f * (0.3f * c[ch] + 0.7f * l));
            }
            unsigned char* p = &g.bgra[(static_cast<size_t>(py) * W + i) * 4];
            p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; p[3] = 255;
        }
    });
    g.img_w = W; g.img_h = H; g.img_scale = scale;
    g.img_cx = cx; g.img_cz = cz; g.img_mpp = mpp;
    g.img_map_w = mw; g.img_map_h = mh;
    g.img_full = full;
    draw_props();
    g.render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// --- text panels --------------------------------------------------------------

float target_land() { return g.calib.land_for_offset(g.wp.land_offset); }

void set_status() {
    std::string s = fmt("seed %llu   build %.0f ms, draw %.0f ms\r\n",
                        static_cast<unsigned long long>(g.seed), g.build_ms, g.render_ms);
    s += fmt("grid %d%s   %.1f m/px\r\n", g.built_grid,
             g.built_grid >= kGridFull ? " (the engine's)" : " (preview)", static_cast<double>(g.mpp));
    s += fmt("land target %.1f%% (offset %+.4f)%s\r\nthis seed %.1f%% land, %d landmasses\r\n",
             100.0 * static_cast<double>(target_land()), static_cast<double>(g.wp.land_offset),
             same_layer0(g.wp, g.calib_wp) ? "" : " STALE",
             100.0 * static_cast<double>(g.world.land_fraction),
             g.metrics_valid ? g.metrics.landmasses : 0);
    if (params_modified())
        s += "off shipping: the engine gets these\r\n";
    if (checked(g.ov_props) && g.mpp > kPropsMpp)
        s += fmt("trees and rocks: zoom in under %.0f m/px\r\n", static_cast<double>(kPropsMpp));
    else if (g.props_shown)
        s += fmt("%zu trees, %zu rocks in view\r\n", g.props.trees.size(), g.props.rocks.size());
    if (!g.reload_note.empty()) s += g.reload_note;
    SetWindowTextA(g.status, s.c_str());
}

std::string slope_word(float sl) {
    int n = 0;
    const SlopeClass* k = slope_classes(n);
    int i = 0;
    while (i + 1 < n && sl >= k[i].upto) ++i;
    return k[i].label;
}

std::string inspect_text(float x, float z) {
    const float R = g.wp.radius;
    const float r = std::sqrt(x * x + z * z);
    std::string s = fmt("x %.0f  z %.0f   %.2f km from centre, (r/R)^2 %.2f\r\n",
                        static_cast<double>(x), static_cast<double>(z),
                        static_cast<double>(r / 1000.0f), static_cast<double>((r / R) * (r / R)));
    if (r > R) return s + "off the disc\r\n";
    const float rr = r / R;
    const float h = terrain_height(g.world, g.wp, g.table, x, z, rr);
    if (h <= g.wp.sea_level) {
        s += fmt("sea, %.1f m deep\r\n", static_cast<double>(g.wp.sea_level - h));
    } else {
        const float e = 2.0f;
        const float gx = (ground_at(x + e, z) - ground_at(x - e, z)) / (2.0f * e);
        const float gz = (ground_at(x, z + e) - ground_at(x, z - e)) / (2.0f * e);
        const float sl = std::sqrt(gx * gx + gz * gz);
        s += fmt("ground %.1f m   slope %.2f (%.0f deg)\r\n   %s\r\n", static_cast<double>(h),
                 static_cast<double>(sl), static_cast<double>(std::atan(sl) * 57.2958f),
                 slope_word(sl).c_str());
    }
    int bi[8];
    float bw[8];
    const int n = biome_mix(g.world, g.wp, x, z, bi, bw);
    // Merge entries that share a biome (two of the four cells can).
    std::vector<std::pair<float, int> > mix;
    for (int q = 0; q < n; ++q) {
        bool merged = false;
        for (size_t k = 0; k < mix.size(); ++k)
            if (mix[k].second == bi[q]) { mix[k].first += bw[q]; merged = true; }
        if (!merged) mix.push_back(std::make_pair(bw[q], bi[q]));
    }
    std::sort(mix.rbegin(), mix.rend());
    // Over the sea the blend names the cells round the point, which is the
    // nearest coast's biome rather than anything here.
    s += h <= g.wp.sea_level ? "coast's biome " : "biome ";
    for (size_t k = 0; k < mix.size() && k < 3; ++k) {
        if (mix[k].first < 0.005f) continue;
        s += fmt("%s%s %.0f%%", k ? ", " : "", biome_name(mix[k].second).c_str(),
                 100.0 * static_cast<double>(mix[k].first));
    }
    if (mix.empty()) s += "none (sea)";
    s += "\r\n";
    const float d = layer1_danger(x, z, rr, g.wp, g.world.s_dang);
    s += fmt("danger %.2f", static_cast<double>(d));
    if (!mix.empty() && mix[0].second >= 0 && mix[0].second < static_cast<int>(g.table.size()))
        s += fmt("  (%s wants %.2f)", biome_name(mix[0].second).c_str(),
                 static_cast<double>(g.table.biomes[mix[0].second].danger_preferred));
    s += "\r\n";
    float rw = 0.0f, rd = 0.0f;
    const float rdist = nearest_river(g.world, g.wp, x, z, rw, rd);
    if (rdist < 5000.0f && rw > 0.0f)
        s += fmt("river %s off, channel %.0f m wide\r\n", metres(rdist).c_str(), static_cast<double>(rw));
    const int fs = flat_site_at(g.world, x, z, 0.0f);
    if (fs >= 0) {
        const SeedWorld::FlatSite& f = g.world.flats[fs];
        const float fd = std::sqrt((x - f.x) * (x - f.x) + (z - f.z) * (z - f.z));
        s += fmt("flat site: pad r %.0f m + %.0f m shoulder, level %.1f m (%s)\r\n",
                 static_cast<double>(f.radius), static_cast<double>(f.shoulder),
                 static_cast<double>(f.level), fd <= f.radius ? "on the pad" : "on the shoulder");
    }
    const float sd = std::sqrt((x - g.spawn.x) * (x - g.spawn.x) + (z - g.spawn.z) * (z - g.spawn.z));
    s += fmt("from spawn %s", metres(sd).c_str());
    if (g.metrics_valid) {
        const int G = g.metrics.G;
        const int i = static_cast<int>((x + R) / g.metrics.step), j = static_cast<int>((z + R) / g.metrics.step);
        if (i >= 0 && j >= 0 && i < G && j < G && h > g.wp.sea_level)
            s += g.metrics.foot[static_cast<size_t>(j) * G + i] ? ", on foot" : ", NOT on foot (water between)";
    }
    s += "\r\n";
    if (g.marker) {
        const float md = std::sqrt((x - g.mx) * (x - g.mx) + (z - g.mz) * (z - g.mz));
        s += fmt("from marker %s\r\n", metres(md).c_str());
    }
    return s;
}

void update_inspect() {
    std::string s;
    if (g.pinned) {
        s = "PINNED (Esc to unpin)\r\n" + inspect_text(g.px, g.pz);
        if (g.hover) {
            const float d = std::sqrt((g.hx - g.px) * (g.hx - g.px) + (g.hz - g.pz) * (g.hz - g.pz));
            s += fmt("cursor %s from the pin\r\n", metres(d).c_str());
        }
    } else if (g.hover) {
        s = inspect_text(g.hx, g.hz);
    } else {
        s = "Hover over the map. Click to pin a point,\r\nright-click to drop a measuring marker.\r\n";
    }
    static std::string last;
    if (s != last) { SetWindowTextA(g.inspect, s.c_str()); last = s; }
}

std::string metrics_text() {
    if (!g.metrics_valid) return "measuring...";
    const SeedMetrics& m = g.metrics;
    const int nb = static_cast<int>(g.table.size());
    std::string s;
    s += fmt("SPAWN (%.0f, %.0f), %s from centre, ground %.1f m\r\n",
             static_cast<double>(m.spawn.x), static_cast<double>(m.spawn.z), metres(m.spawn_r).c_str(),
             static_cast<double>(m.spawn.ground));
    s += fmt("  in %s at weight %.2f%s, danger %.2f\r\n", biome_name(m.spawn.biome).c_str(),
             static_cast<double>(m.spawn.weight),
             m.spawn.strict ? "" : " -- FALLBACK, no point passed every test",
             static_cast<double>(m.spawn_danger));
    s += fmt("LAND %.1f%%, %d landmasses, biggest %d%% of land;\r\n  spawn's landmass %.0f%% of land\r\n",
             100.0 * static_cast<double>(m.land_fraction), m.landmasses, m.biggest_pct,
             m.land_samples ? 100.0 * m.foot_samples / m.land_samples : 0.0);
    {
        const SeedWorld& w = g.world;
        if (g.wp.knee)
            s += fmt("LAYER 0 knee: land %.1f%% -> %.1f%%, %.0f ms\r\n",
                     100.0 * w.knee_raw_land, 100.0 * w.knee_land, static_cast<double>(w.knee_ms));
        else
            s += "LAYER 0 knee off\r\n";
        if (w.l0.regime_on)
            s += fmt("  regime %.0f m, mean %+.3f, amp %.3f%s\r\n",
                     w.l0.regime_freq > 0.0f ? 1.0 / w.l0.regime_freq : 0.0,
                     static_cast<double>(w.l0.regime_mean), static_cast<double>(w.l0.regime_amp),
                     g.wp.regime_draw ? " (drawn)" : "");
        s += fmt("  wading too (< 0.85 m): %.1f%% of land, %d/%d\r\n"
                 "    biomes; shallows %.1f km2\r\n",
                 100.0 * static_cast<double>(g.reach.reach_share), g.reach.biomes_reached,
                 g.reach.biomes_present, static_cast<double>(g.reach.shelf_km2));
    }
    s += fmt("FLAT SITES %d: %d within 1 km, %d within 2 km, nearest %s\r\n",
             m.flats, m.flats_1k, m.flats_2k, metres(m.nearest_flat).c_str());
    s += fmt("RIVERS nearest channel %s from spawn\r\n\r\n", metres(m.nearest_river).c_str());
    s += "biome        land  nearest  on foot  foot%\r\n";
    for (int b = 0; b < nb; ++b) {
        std::string nm = g.table.biomes[b].name;
        if (nm.size() > 11) nm.resize(11);
        if (m.samples[b] == 0) {
            s += fmt("%-11s     -   absent\r\n", nm.c_str());
            continue;
        }
        s += fmt("%-11s %4.1f%% %8s %8s  %4.0f\r\n", nm.c_str(),
                 m.land_samples ? 100.0 * m.samples[b] / m.land_samples : 0.0,
                 metres(m.nearest[b]).c_str(),
                 m.nearest_foot[b] < 0.0f ? "NO" : metres(m.nearest_foot[b]).c_str(),
                 100.0 * m.foot_count[b] / m.samples[b]);
    }
    s += "(straight-line distances; on foot = no water\r\n crossed, on a 50 m grid)\r\n\r\n";
    s += "danger by distance from spawn (land mean)\r\n";
    const char* band[kDangerBands] = {"0-1", "1-2", "2-4", "4-6", "6+ "};
    for (int q = 0; q < kDangerBands; ++q) {
        if (m.danger_n[q] == 0) continue;
        s += fmt("  %s km  %.2f  ", band[q], static_cast<double>(m.danger_mean[q]));
        const int bar = static_cast<int>(m.danger_mean[q] * 30.0f + 0.5f);
        s += std::string(static_cast<size_t>(std::max(0, bar)), '#');
        s += "\r\n";
    }
    return s;
}

void refresh_text() {
    set_status();
    update_inspect();
    const std::string m = metrics_text();
    static std::string last;
    if (m != last) { SetWindowTextA(g.seed_info, m.c_str()); last = m; }
}

void redraw(bool full) {
    SetCursor(LoadCursor(0, IDC_WAIT));
    render_map(full);
    refresh_text();
    SetCursor(LoadCursor(0, IDC_ARROW));
    InvalidateRect(g.hwnd, 0, FALSE);
    if (!full) SetTimer(g.hwnd, TIMER_REFINE, 250, 0);
}

// A preview now and the full image when the user stops: wheel, drag, resize.
void preview_then_refine() { redraw(false); }

// --- the biome table: reload and the highlight list ------------------------------

void fill_highlight_box() {
    SendMessageA(g.hl_box, CB_RESETCONTENT, 0, 0);
    SendMessageA(g.hl_box, CB_ADDSTRING, 0, (LPARAM)"(none)");
    for (size_t b = 0; b < g.table.size(); ++b)
        SendMessageA(g.hl_box, CB_ADDSTRING, 0, (LPARAM)g.table.biomes[b].name.c_str());
    if (g.highlight >= static_cast<int>(g.table.size())) g.highlight = -1;
    SendMessage(g.hl_box, CB_SETCURSEL, static_cast<WPARAM>(g.highlight + 1), 0);
}

bool file_time(const char* path, FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return false;
    t = d.ftLastWriteTime;
    return true;
}

void reload_table() {
    FILETIME t;
    if (file_time(kArchetypes, t)) g.arch_time = t;
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof(when), "%H:%M:%S", std::localtime(&now));
    try {
        g.table = load_archetypes(kArchetypes);
        g.reload_note = fmt("archetypes.json loaded %s\r\n", when);
    } catch (const std::exception& e) {
        // Keep the last good table: a half-typed edit must not blank the map.
        g.reload_note = fmt("archetypes.json NOT loaded %s:\r\n%s\r\n", when, e.what());
        MessageBeep(MB_ICONWARNING);
        set_status();
        return;
    }
    fill_highlight_box();
    g.world_dirty = true;
    redraw(true);
}

// --- the scan -------------------------------------------------------------------

std::string scan_line(const SeedMetrics& m) {
    const int off = m.biomes_off_foot();
    return fmt("%5llu %5.1f%% %3d %4.0f%% %6.0f%c %3d %4s %6s",
               static_cast<unsigned long long>(m.seed), 100.0 * static_cast<double>(m.land_fraction),
               m.landmasses, m.land_samples ? 100.0 * m.foot_samples / m.land_samples : 0.0,
               static_cast<double>(m.spawn_r), m.spawn.strict ? ' ' : '!', m.flats_2k,
               off ? fmt("%d", off).c_str() : "-",
               metres(m.furthest_foot()).c_str());
}

void fill_scan_list() {
    SendMessageA(g.scan_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g.scan_rows.size(); ++i) {
        const LRESULT k = SendMessageA(g.scan_list, LB_ADDSTRING, 0, (LPARAM)scan_line(g.scan_rows[i]).c_str());
        SendMessageA(g.scan_list, LB_SETITEMDATA, static_cast<WPARAM>(k), static_cast<LPARAM>(g.scan_rows[i].seed));
    }
}

void write_scan_csv() {
    FILE* f = std::fopen("build/seed_scan.csv", "w");
    if (!f) return;
    std::fprintf(f, "seed,land_pct,landmasses,biggest_mass_pct,spawn_mass_pct,spawn_x,spawn_z,spawn_r_m,"
                    "spawn_strict,spawn_biome,spawn_danger,flats,flats_1km,flats_2km,nearest_flat_m,"
                    "nearest_river_m,biomes_off_foot,furthest_biome_on_foot_m");
    for (size_t b = 0; b < g.scan_names.size(); ++b)
        std::fprintf(f, ",%s_land_pct,%s_nearest_m,%s_on_foot_m", g.scan_names[b].c_str(),
                     g.scan_names[b].c_str(), g.scan_names[b].c_str());
    for (int q = 0; q < kDangerBands; ++q) std::fprintf(f, ",danger_band%d", q);
    std::fprintf(f, "\n");
    for (size_t i = 0; i < g.scan_rows.size(); ++i) {
        const SeedMetrics& m = g.scan_rows[i];
        const int sb = m.spawn.biome;
        std::fprintf(f, "%llu,%.2f,%d,%d,%.1f,%.0f,%.0f,%.0f,%d,%s,%.3f,%d,%d,%d,%.0f,%.0f,%d,%.0f",
                     static_cast<unsigned long long>(m.seed), 100.0 * static_cast<double>(m.land_fraction),
                     m.landmasses, m.biggest_pct,
                     m.land_samples ? 100.0 * m.foot_samples / m.land_samples : 0.0,
                     static_cast<double>(m.spawn.x), static_cast<double>(m.spawn.z),
                     static_cast<double>(m.spawn_r), m.spawn.strict ? 1 : 0,
                     sb >= 0 && sb < static_cast<int>(g.scan_names.size()) ? g.scan_names[sb].c_str() : "-",
                     static_cast<double>(m.spawn_danger), m.flats, m.flats_1k, m.flats_2k,
                     static_cast<double>(m.nearest_flat), static_cast<double>(m.nearest_river),
                     m.biomes_off_foot(), static_cast<double>(m.furthest_foot()));
        for (size_t b = 0; b < g.scan_names.size() && b < m.samples.size(); ++b)
            std::fprintf(f, ",%.2f,%.0f,%.0f",
                         m.land_samples ? 100.0 * m.samples[b] / m.land_samples : 0.0,
                         static_cast<double>(m.nearest[b]), static_cast<double>(m.nearest_foot[b]));
        for (int q = 0; q < kDangerBands; ++q) std::fprintf(f, ",%.3f", static_cast<double>(m.danger_mean[q]));
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void stop_scan() {
    if (g.scan_thread.joinable()) {
        g.scan_cancel = true;
        g.scan_thread.join();
    }
    g.scanning = false;
}

void start_scan() {
    stop_scan();
    char buf[32];
    GetWindowTextA(g.scan_count, buf, sizeof(buf));
    const int count = std::max(1, std::min(2000, std::atoi(buf)));
    g.scan_rows.clear();
    g.scan_names.clear();
    for (size_t b = 0; b < g.table.size(); ++b) g.scan_names.push_back(g.table.biomes[b].name);
    fill_scan_list();
    g.scan_cancel = false;
    g.scanning = true;
    SetWindowTextA(g.scan_btn, "Stop scan");
    g.scan_summary = fmt("scanning %d seeds from %llu...", count, static_cast<unsigned long long>(g.seed));
    SetWindowTextA(g.scan_note, g.scan_summary.c_str());

    WorldParams wp = g.wp;
    wp.grid = kGridFull;
    const ArchetypeTable table = g.table;
    const AssignParams ap = g.ap;
    const uint64_t first = g.seed;
    const HWND hwnd = g.hwnd;
    g.scan_thread = std::thread([wp, table, ap, first, count, hwnd]() {
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        const int threads = std::max(1, std::min(default_threads(), count));
        for (int t = 0; t < threads; ++t) {
            pool.push_back(std::thread([&]() {
                for (int i = next++; i < count && !g.scan_cancel; i = next++) {
                    const uint64_t seed = first + static_cast<uint64_t>(i);
                    const SeedWorld w = build_world(seed, wp, table, ap);
                    if (g.scan_cancel) break;
                    const SpawnPoint sp = find_spawn(w, wp, table);
                    SeedMetrics* m = new SeedMetrics(measure_seed(w, wp, table, sp, seed, 1));
                    m->dom.clear(); m->dom.shrink_to_fit();
                    m->foot.clear(); m->foot.shrink_to_fit();
                    if (!PostMessage(hwnd, WM_APP_SCAN_ROW, 0, reinterpret_cast<LPARAM>(m))) delete m;
                }
            }));
        }
        for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
        PostMessage(hwnd, WM_APP_SCAN_DONE, 0, 0);
    });
}

void scan_finished() {
    if (g.scan_thread.joinable()) g.scan_thread.join();
    g.scanning = false;
    SetWindowTextA(g.scan_btn, "Scan seeds");
    int fallback = 0, off = 0;
    std::vector<float> furthest;
    for (size_t i = 0; i < g.scan_rows.size(); ++i) {
        if (!g.scan_rows[i].spawn.strict) ++fallback;
        if (g.scan_rows[i].biomes_off_foot() > 0) ++off;
        furthest.push_back(g.scan_rows[i].furthest_foot());
    }
    std::sort(furthest.begin(), furthest.end());
    const float med = furthest.empty() ? 0.0f : furthest[furthest.size() / 2];
    write_scan_csv();
    g.scan_summary = fmt("%zu seeds%s: %d fallback spawns (!), %d with a biome\r\nnot on foot; "
                         "median furthest biome %s. build/seed_scan.csv",
                         g.scan_rows.size(), g.scan_cancel ? " (stopped)" : "", fallback, off,
                         metres(med).c_str());
    SetWindowTextA(g.scan_note, g.scan_summary.c_str());
}

// --- painting -------------------------------------------------------------------

void draw_text(HDC dc, int x, int y, const std::string& s, COLORREF c) {
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

// The text a halo would give, cheaply: dark copies round a light one.
void label(HDC dc, int x, int y, const std::string& s, COLORREF c) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) draw_text(dc, x + dx, y + dy, s, RGB(0, 0, 0));
    draw_text(dc, x, y, s, c);
}

void circle(HDC dc, float x, float z, float r_m, float min_px) {
    int sx, sy;
    to_screen(x, z, sx, sy);
    const int r = static_cast<int>(std::max(min_px, r_m / g.mpp));
    Ellipse(dc, sx - r, sy - r, sx + r + 1, sy + r + 1);
}

void draw_overlays(HDC dc) {
    const RECT m = map_rect();
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    const float hw = 0.5f * map_w() * g.mpp, hh = 0.5f * map_h() * g.mpp;
    const float vx0 = g.cx - hw, vx1 = g.cx + hw, vz0 = g.cz - hh, vz1 = g.cz + hh;

    if (checked(g.ov_grid)) {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(90, 96, 110));
        HGDIOBJ old = SelectObject(dc, p);
        const float step = g.mpp > 40.0f ? 2000.0f : (g.mpp > 4.0f ? 1000.0f : 100.0f);
        for (float x = std::ceil(vx0 / step) * step; x <= vx1; x += step) {
            int sx, sy;
            to_screen(x, 0.0f, sx, sy);
            MoveToEx(dc, sx, m.top, 0);
            LineTo(dc, sx, m.bottom);
            label(dc, sx + 3, m.top + 3, fmt("x %.0f", static_cast<double>(x)), RGB(200, 205, 215));
        }
        for (float z = std::ceil(vz0 / step) * step; z <= vz1; z += step) {
            int sx, sy;
            to_screen(0.0f, z, sx, sy);
            MoveToEx(dc, m.left, sy, 0);
            LineTo(dc, m.right, sy);
            label(dc, m.left + 3, sy + 2, fmt("z %.0f", static_cast<double>(z)), RGB(200, 205, 215));
        }
        SelectObject(dc, old);
        DeleteObject(p);
    }

    if (checked(g.ov_rivers)) {
        for (size_t i = 0; i < g.world.rivers.size(); ++i) {
            const SeedWorld::RiverSeg& s = g.world.rivers[i];
            if (std::max(s.ax, s.bx) < vx0 || std::min(s.ax, s.bx) > vx1 ||
                std::max(s.az, s.bz) < vz0 || std::min(s.az, s.bz) > vz1) continue;
            const int wpx = std::max(1, static_cast<int>(s.width / g.mpp + 0.5f));
            HPEN p = CreatePen(PS_SOLID, wpx, RGB(80, 160, 240));
            HGDIOBJ old = SelectObject(dc, p);
            int ax, ay, bx, by;
            to_screen(s.ax, s.az, ax, ay);
            to_screen(s.bx, s.bz, bx, by);
            MoveToEx(dc, ax, ay, 0);
            LineTo(dc, bx, by);
            SelectObject(dc, old);
            DeleteObject(p);
        }
    }

    if (checked(g.ov_flats)) {
        HPEN pad = CreatePen(PS_SOLID, 2, RGB(255, 220, 60));
        HPEN sh = CreatePen(PS_DOT, 1, RGB(255, 220, 60));
        for (size_t i = 0; i < g.world.flats.size(); ++i) {
            const SeedWorld::FlatSite& f = g.world.flats[i];
            HGDIOBJ old = SelectObject(dc, pad);
            circle(dc, f.x, f.z, f.radius, 3.0f);
            if ((f.radius + f.shoulder) / g.mpp > 6.0f) {
                SelectObject(dc, sh);
                circle(dc, f.x, f.z, f.radius + f.shoulder, 4.0f);
            }
            SelectObject(dc, old);
        }
        DeleteObject(pad);
        DeleteObject(sh);
    }

    if (checked(g.ov_rings)) {
        HPEN p = CreatePen(PS_DOT, 1, RGB(235, 235, 235));
        HGDIOBJ old = SelectObject(dc, p);
        const float step = g.mpp < 3.0f ? 250.0f : 1000.0f;
        for (int k = 1; k * step <= 2.0f * g.wp.radius; ++k) {
            const float r = k * step;
            if (r / g.mpp < 12.0f) continue;
            circle(dc, g.spawn.x, g.spawn.z, r, 0.0f);
            int sx, sy;
            to_screen(g.spawn.x, g.spawn.z - r, sx, sy);
            if (r / g.mpp > 22.0f) label(dc, sx + 3, sy + 1, metres(r), RGB(240, 240, 240));
        }
        SelectObject(dc, old);
        DeleteObject(p);
    }

    if (checked(g.ov_spawn)) {
        HPEN p = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
        HGDIOBJ old = SelectObject(dc, p);
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(dc, b);
        circle(dc, g.spawn.x, g.spawn.z, 0.0f, 6.0f);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        // find_spawn's own ring: the calmest biome must hold a majority on it.
        if (60.0f / g.mpp > 8.0f) circle(dc, g.spawn.x, g.spawn.z, 60.0f, 0.0f);
        SelectObject(dc, ob);
        DeleteObject(b);
        SelectObject(dc, old);
        DeleteObject(p);
        int sx, sy;
        to_screen(g.spawn.x, g.spawn.z, sx, sy);
        label(dc, sx + 9, sy - 8, g.spawn.strict ? "spawn" : "spawn (fallback)", RGB(255, 255, 255));
    }

    if (g.marker) {
        HPEN p = CreatePen(PS_SOLID, 2, RGB(255, 80, 200));
        HGDIOBJ old = SelectObject(dc, p);
        int sx, sy;
        to_screen(g.mx, g.mz, sx, sy);
        MoveToEx(dc, sx - 7, sy - 7, 0); LineTo(dc, sx + 8, sy + 8);
        MoveToEx(dc, sx - 7, sy + 7, 0); LineTo(dc, sx + 8, sy - 8);
        if (g.hover) {
            HPEN d = CreatePen(PS_DOT, 1, RGB(255, 80, 200));
            SelectObject(dc, d);
            int hx, hy;
            to_screen(g.hx, g.hz, hx, hy);
            MoveToEx(dc, sx, sy, 0);
            LineTo(dc, hx, hy);
            const float md = std::sqrt((g.hx - g.mx) * (g.hx - g.mx) + (g.hz - g.mz) * (g.hz - g.mz));
            label(dc, hx + 12, hy + 6, metres(md), RGB(255, 160, 230));
            SelectObject(dc, p);
            DeleteObject(d);
        }
        SelectObject(dc, old);
        DeleteObject(p);
    }

    if (g.pinned) {
        HPEN p = CreatePen(PS_SOLID, 2, RGB(255, 255, 0));
        HGDIOBJ old = SelectObject(dc, p);
        int sx, sy;
        to_screen(g.px, g.pz, sx, sy);
        MoveToEx(dc, sx - 10, sy, 0); LineTo(dc, sx - 3, sy);
        MoveToEx(dc, sx + 4, sy, 0);  LineTo(dc, sx + 11, sy);
        MoveToEx(dc, sx, sy - 10, 0); LineTo(dc, sx, sy - 3);
        MoveToEx(dc, sx, sy + 4, 0);  LineTo(dc, sx, sy + 11);
        SelectObject(dc, old);
        DeleteObject(p);
    }

    // Scale bar, bottom left.
    {
        const float target = 120.0f * g.mpp;
        const float steps[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
        float len = steps[0];
        for (float s : steps) if (s <= target) len = s;
        const int px = static_cast<int>(len / g.mpp);
        HPEN p = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc, p);
        MoveToEx(dc, m.left + 14, m.bottom - 16, 0);
        LineTo(dc, m.left + 14 + px, m.bottom - 16);
        SelectObject(dc, old);
        DeleteObject(p);
        label(dc, m.left + 14, m.bottom - 34, metres(len) + "   (+x right, +z down)", RGB(255, 255, 255));
    }
}

int legend_row_h() { return 19; }

void draw_legend(HDC dc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    SetBkMode(dc, TRANSPARENT);
    int y = LEGEND_Y;
    const COLORREF ink = GetSysColor(COLOR_BTNTEXT);
    if (g.mode == VIEW_BIOME || g.mode == VIEW_BIOME_RELIEF) {
        draw_text(dc, 12, y, "biome", ink);
        draw_text(dc, 126, y, "quota", ink);
        draw_text(dc, 190, y, "danger", ink);
        y += legend_row_h() + 2;
        for (size_t b = 0; b < g.table.size(); ++b) {
            unsigned char c[3];
            biome_colour(static_cast<int>(b), c);
            HBRUSH br = CreateSolidBrush(RGB(c[0], c[1], c[2]));
            RECT sw = {12, y + 2, 28, y + 16};
            FillRect(dc, &sw, br);
            DeleteObject(br);
            const BiomeArchetype& a = g.table.biomes[b];
            if (static_cast<int>(b) == g.highlight) {
                RECT fr = {8, y, LEFT - 8, y + legend_row_h()};
                FrameRect(dc, &fr, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            draw_text(dc, 34, y + 1, a.name, ink);
            draw_text(dc, 126, y + 1, fmt("%4.1f%%", 100.0 * static_cast<double>(a.area_fraction)), ink);
            draw_text(dc, 190, y + 1, fmt("%.2f", static_cast<double>(a.danger_preferred)), ink);
            y += legend_row_h();
        }
    } else if (g.mode == VIEW_SLOPE) {
        draw_text(dc, 12, y, "slope over 6 m (rise / run)", ink);
        y += legend_row_h() + 2;
        int n = 0;
        const SlopeClass* k = slope_classes(n);
        for (int i = 0; i < n; ++i) {
            HBRUSH br = CreateSolidBrush(RGB(k[i].rgb[0], k[i].rgb[1], k[i].rgb[2]));
            RECT sw = {12, y + 2, 28, y + 16};
            FillRect(dc, &sw, br);
            DeleteObject(br);
            draw_text(dc, 34, y + 1, k[i].label, ink);
            y += legend_row_h();
        }
    } else if (g.mode == VIEW_DANGER) {
        draw_text(dc, 12, y, "layer 1 danger, 0 to 1", ink);
        y += legend_row_h() + 2;
        for (int x = 0; x < LEFT - 40; ++x) {
            unsigned char c[3];
            danger_colour(static_cast<float>(x) / (LEFT - 41), c);
            HPEN p = CreatePen(PS_SOLID, 1, RGB(c[0], c[1], c[2]));
            HGDIOBJ old = SelectObject(dc, p);
            MoveToEx(dc, 12 + x, y, 0);
            LineTo(dc, 12 + x, y + 16);
            SelectObject(dc, old);
            DeleteObject(p);
        }
        y += 20;
        draw_text(dc, 12, y, "0 calm", ink);
        draw_text(dc, LEFT - 70, y, "1 deadly", ink);
        y += legend_row_h();
    } else {
        draw_text(dc, 12, y, "height: lowland green, brown,", ink);
        y += legend_row_h();
        draw_text(dc, 12, y, "snow from ~170 m", ink);
        y += legend_row_h();
    }
    y += 8;
    draw_text(dc, 12, y, "overlays: white dot spawn, yellow", ink); y += legend_row_h();
    draw_text(dc, 12, y, "flat sites, blue rivers, pink marker", ink); y += legend_row_h();
    y += 6;
    draw_text(dc, 12, y, "click a legend row to highlight it", ink); y += legend_row_h();
    draw_text(dc, 12, y, "wheel zoom, drag pan, Home fit", ink); y += legend_row_h();
    draw_text(dc, 12, y, "PgUp/PgDn seed, 1-5 view, F5 reload", ink);
}

void on_paint() {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(g.hwnd, &ps);
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W = std::max<int>(1, rc.right), H = std::max<int>(1, rc.bottom);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ old_bm = SelectObject(dc, bm);
    HGDIOBJ old_font = SelectObject(dc, g.ui_font);

    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    const RECT m = map_rect();
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 26));
    FillRect(dc, &m, bg);
    DeleteObject(bg);

    if (g.img_w > 0) {
        // Where the last image's world rectangle falls in today's view.
        const float wx0 = g.img_cx - 0.5f * g.img_map_w * g.img_mpp;
        const float wz0 = g.img_cz - 0.5f * g.img_map_h * g.img_mpp;
        int sx, sy;
        to_screen(wx0, wz0, sx, sy);
        const int dw = static_cast<int>(g.img_w * g.img_scale * g.img_mpp / g.mpp + 0.5f);
        const int dh = static_cast<int>(g.img_h * g.img_scale * g.img_mpp / g.mpp + 0.5f);
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g.img_w;
        bi.bmiHeader.biHeight = -g.img_h;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, m.left, m.top, m.right, m.bottom);
        SetStretchBltMode(dc, g.img_scale == 1 && dw == g.img_w ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, 0);
        StretchDIBits(dc, sx, sy, dw, dh, 0, 0, g.img_w, g.img_h,
                      g.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        draw_overlays(dc);
        RestoreDC(dc, saved);
    }
    draw_legend(dc);

    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bm);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(g.hwnd, &ps);
}

// --- controls -------------------------------------------------------------------

HWND make(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id,
          HFONT font = 0) {
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g.hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), 0, 0);
    SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g.ui_font), TRUE);
    return c;
}

HWND make_bar(int y, int id, int lo, int hi, int pos) {
    HWND b = make(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS, 8, y, LEFT - 16, 24, id);
    SendMessage(b, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessage(b, TBM_SETPOS, TRUE, pos);
    return b;
}

void set_sliders_from_params() {
    SendMessage(g.w_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(g.wp.danger_w * 100.0f + 0.5f));
    SendMessage(g.feat_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(1.0f / g.wp.danger_freq + 0.5f));
    SendMessage(g.land_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(target_land() * 1000.0f + 0.5f));
}

void build_controls() {
    int y = 10;
    make("BUTTON", "< seed", 0, 8, y, 62, 26, ID_SEED_PREV);
    g.seed_edit = make("EDIT", "0", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 74, y + 2, 76, 22, ID_SEED_EDIT);
    make("BUTTON", "Go", 0, 154, y, 36, 26, ID_SEED_GO);
    make("BUTTON", "seed >", 0, 194, y, 60, 26, ID_SEED_NEXT);
    y += 36;

    make("STATIC", "view", 0, 12, y, 60, 18, 0);
    g.mode_box = make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 60, y - 3, LEFT - 68, 200, ID_MODE);
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"1 Biome map");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"2 Shaded relief");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"3 Danger field");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"4 Biomes over relief");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"5 Slope (buildability)");
    SendMessage(g.mode_box, CB_SETCURSEL, static_cast<WPARAM>(g.mode), 0);
    y += 30;

    make("STATIC", "highlight", 0, 12, y, 60, 18, 0);
    g.hl_box = make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 74, y - 3, LEFT - 82, 300, ID_HIGHLIGHT);
    y += 32;

    g.ov_spawn  = make("BUTTON", "spawn",       BS_AUTOCHECKBOX, 12, y, 118, 20, ID_OV_SPAWN);
    g.ov_rings  = make("BUTTON", "km rings",    BS_AUTOCHECKBOX, 134, y, 118, 20, ID_OV_RINGS);  y += 21;
    g.ov_rivers = make("BUTTON", "rivers",      BS_AUTOCHECKBOX, 12, y, 118, 20, ID_OV_RIVERS);
    g.ov_flats  = make("BUTTON", "flat sites",  BS_AUTOCHECKBOX, 134, y, 118, 20, ID_OV_FLATS);  y += 21;
    g.ov_props  = make("BUTTON", "trees, rocks", BS_AUTOCHECKBOX, 12, y, 118, 20, ID_OV_PROPS);
    g.ov_grid   = make("BUTTON", "coord grid",  BS_AUTOCHECKBOX, 134, y, 118, 20, ID_OV_GRID);   y += 28;
    SendMessage(g.ov_spawn, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g.ov_flats, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g.ov_props, BM_SETCHECK, BST_CHECKED, 0);

    make("STATIC", "danger wobble w", 0, 12, y, LEFT - 24, 16, 0); y += 17;
    g.w_bar = make_bar(y, ID_W, 0, 100, 0); y += 28;
    make("STATIC", "danger feature size (m)", 0, 12, y, LEFT - 24, 16, 0); y += 17;
    g.feat_bar = make_bar(y, ID_FEAT, 1500, 14000, 2000); y += 28;
    // The islandness lever. Below about 35% the land fraction crosses the
    // percolation threshold and the single continent starts breaking up.
    make("STATIC", "land fraction % (islandness)", 0, 12, y, LEFT - 24, 16, 0); y += 17;
    g.land_bar = make_bar(y, ID_LANDOFF, LAND_MIN, LAND_MAX, 320); y += 30;
    make("BUTTON", "Reset sliders to shipping", 0, 8, y, LEFT - 16, 24, ID_RESET); y += 30;

    make("BUTTON", "Reload biomes (F5)", 0, 8, y, 132, 24, ID_RELOAD);
    g.auto_reload = make("BUTTON", "on save", BS_AUTOCHECKBOX, 148, y + 2, 100, 20, ID_AUTO_RELOAD);
    SendMessage(g.auto_reload, BM_SETCHECK, BST_CHECKED, 0);
    y += 30;
    make("BUTTON", "Open in engine", 0, 8, y, 120, 24, ID_ENGINE);
    make("BUTTON", "Copy command", 0, 132, y, LEFT - 140, 24, ID_COPY);
    y += 30;
    make("BUTTON", "Layer 0 prototype...", 0, 8, y, LEFT - 16, 24, ID_L0_OPEN);
    y += 32;
    g.status = make("STATIC", "", 0, 12, y, LEFT - 20, LEGEND_Y - y - 6, 0);

    // The right panel. Positions are set by layout_right().
    g.right_labels[0] = make("STATIC", "Inspect", 0, 0, 0, 10, 10, 0);
    g.inspect = make("EDIT", "", WS_BORDER | ES_MULTILINE | ES_READONLY, 0, 0, 10, 10, 0, g.mono_font);
    g.right_labels[1] = make("STATIC", "This seed", 0, 0, 0, 10, 10, 0);
    g.seed_info = make("EDIT", "", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10, 0,
                       g.mono_font);
    g.scan_btn = make("BUTTON", "Scan seeds", 0, 0, 0, 10, 10, ID_SCAN);
    g.scan_count = make("EDIT", "32", WS_BORDER | ES_NUMBER, 0, 0, 10, 10, ID_SCAN_COUNT);
    g.right_labels[2] = make("STATIC", "from this one; double-click a row", 0, 0, 0, 10, 10, 0);
    g.scan_note = make("STATIC", "A scan builds each seed at the engine's grid, with the\r\n"
                                 "sliders and biomes as they are now.", 0, 0, 0, 10, 10, 0);
    g.scan_label = make("STATIC", " seed  land  mass spawn  spawn-r flat  off  furthest\r\n"
                                  "              mass%      (! fb)  2km foot biome", 0, 0, 0, 10, 10, 0,
                        g.mono_font);
    g.scan_list = make("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       0, 0, 10, 10, ID_SCAN_LIST, g.mono_font);
}

// --- the layer 0 prototype's window ------------------------------------------------

void redraw(bool full);

float l0_value(const L0Slider& s) {
    return static_cast<float>(SendMessage(s.bar, TBM_GETPOS, 0, 0)) * s.scale;
}

void l0_label(const L0Slider& s) {
    SetWindowTextA(s.lab, (std::string(s.name) + "  " + fmt(s.fmt, static_cast<double>(l0_value(s)))).c_str());
}

void l0_set_status() {
    if (!g.l0_status) return;
    std::string t = same_layer0(g.wp, g.calib_wp)
        ? (g.calib_today ? "land %: today's curve" : "land %: curve at these settings")
        : "land %: the curve is STALE (main window says so) -- recalibrate";
    t += "\r\nEvery slider starts at today's world. Open in engine and\r\n"
         "Copy command (main window) carry these settings.";
    SetWindowTextA(g.l0_status, t.c_str());
}

void l0_write_controls() {
    for (size_t i = 0; i < g_l0.size(); ++i) {
        const L0Slider& s = g_l0[i];
        const float v = s.metres_to_freq ? 1.0f / *s.target : *s.target;
        SendMessage(s.bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::floor(v / s.scale + 0.5f)));
        l0_label(s);
    }
    SendMessage(g.l0_draw, BM_SETCHECK, g.wp.regime_draw ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g.l0_knee, BM_SETCHECK, g.wp.knee ? BST_CHECKED : BST_UNCHECKED, 0);
    l0_set_status();
}

// The land fraction curve at the current layer 0 settings, knee included.
// Wider than the startup curve: the regime's mean moves it sideways. The land
// slider keeps its place, so the mean land fraction it asks for is kept.
void recalibrate() {
    SetCursor(LoadCursor(0, IDC_WAIT));
    const int land10 = static_cast<int>(SendMessage(g.land_bar, TBM_GETPOS, 0, 0));
    WorldParams cw = g.wp;
    cw.grid = kGridPreview;
    g.calib.build(cw, 16, 192, -0.40f, 0.40f, 32);
    g.calib_wp = g.wp;
    g.calib_today = same_layer0(g.wp, g.shipping);
    g.wp.land_offset = g.calib.offset_for_land(static_cast<float>(land10) / 1000.0f);
    if (g.calib_today) g.wp.land_offset = g.shipping.land_offset;
    SetCursor(LoadCursor(0, IDC_ARROW));
}

LRESULT CALLBACK l0_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_HSCROLL: {
            const HWND bar = reinterpret_cast<HWND>(lp);
            for (size_t i = 0; i < g_l0.size(); ++i) {
                L0Slider& s = g_l0[i];
                if (s.bar != bar) continue;
                const float v = l0_value(s);
                *s.target = s.metres_to_freq ? 1.0f / std::max(1.0f, v) : v;
                l0_label(s);
                g.world_dirty = true;
                l0_set_status();
                const int code = LOWORD(wp);
                redraw(code == TB_ENDTRACK || code == SB_ENDSCROLL);
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_L0_DRAW || id == ID_L0_KNEE) {
                g.wp.regime_draw = checked(g.l0_draw);
                g.wp.knee = checked(g.l0_knee);
                g.world_dirty = true;
                l0_set_status();
                redraw(true);
            } else if (id == ID_L0_RECAL) {
                recalibrate();
                set_sliders_from_params();
                l0_set_status();
                g.world_dirty = true;
                redraw(true);
            }
            return 0;
        }
        case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;   // hidden, not destroyed
        case WM_CTLCOLORSTATIC: {
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
    }
    return DefWindowProc(h, m, wp, lp);
}

void build_l0_window(HINSTANCE inst) {
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = l0_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = "biomes2_view_l0";
    RegisterClassA(&wc);
    const int COL = 250;
    g.l0_wnd = CreateWindowA("biomes2_view_l0", "Layer 0 prototype", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 2 * COL + 28, 620, g.hwnd, 0, inst, 0);
    auto ctl = [&](const char* cls, const char* text, DWORD style, int x, int y, int w, int hh, int id) {
        HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hh, g.l0_wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), 0, 0);
        SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.ui_font), TRUE);
        return c;
    };
    auto slider = [&](int x, int& y, const char* name, int lo, int hi, float scale, float* target,
                      bool mtf, const char* f) {
        L0Slider s;
        s.name = name; s.scale = scale; s.target = target; s.metres_to_freq = mtf; s.fmt = f;
        s.lab = ctl("STATIC", name, 0, x + 4, y, COL - 8, 16, 0);
        y += 17;
        s.bar = ctl(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS, x, y, COL, 24,
                    ID_L0_SLIDER0 + static_cast<int>(g_l0.size()));
        SendMessage(s.bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        y += 28;
        g_l0.push_back(s);
    };
    WorldParams& p = g.wp;
    int xa = 8, ya = 8;
    slider(xa, ya, "centre floor", 0, 30, 0.01f, &p.centre_floor, false, "%.2f");
    slider(xa, ya, "regime feature m", 1500, 14000, 1.0f, &p.regime_freq, true, "%.0f");
    slider(xa, ya, "regime mean (mask)", -150, 150, 0.001f, &p.regime_mean, false, "%+.3f");
    slider(xa, ya, "regime amp (mask/std)", 0, 300, 0.001f, &p.regime_amp, false, "%.3f");
    slider(xa, ya, "fine weight", 0, 100, 0.01f, &p.fine_weight, false, "%.2f");
    slider(xa, ya, "fine from regime (/std)", 0, 100, 0.01f, &p.fine_regime, false, "%.2f");
    slider(xa, ya, "fine feature m", 600, 4000, 1.0f, &p.fine_freq, true, "%.0f");
    ya += 4;
    g.l0_knee = ctl("BUTTON", "land knee", BS_AUTOCHECKBOX, xa + 4, ya, COL - 8, 20, ID_L0_KNEE);
    ya += 24;
    slider(xa, ya, "knee k", 250, 450, 0.001f, &p.knee_k, false, "%.3f");
    slider(xa, ya, "knee ceiling c", 300, 550, 0.001f, &p.knee_c, false, "%.3f");

    int xb = COL + 16, yb = 8;
    g.l0_draw = ctl("BUTTON", "draw regime per seed", BS_AUTOCHECKBOX, xb + 4, yb, COL - 8, 20, ID_L0_DRAW);
    yb += 24;
    slider(xb, yb, "draw: size lo m", 1500, 14000, 1.0f, &p.regime_size_lo, false, "%.0f");
    slider(xb, yb, "draw: size hi m", 1500, 14000, 1.0f, &p.regime_size_hi, false, "%.0f");
    slider(xb, yb, "draw: mean lo", -150, 150, 0.001f, &p.regime_mean_lo, false, "%+.3f");
    slider(xb, yb, "draw: mean hi", -150, 150, 0.001f, &p.regime_mean_hi, false, "%+.3f");
    slider(xb, yb, "draw: amp lo", 0, 300, 0.001f, &p.regime_amp_lo, false, "%.3f");
    slider(xb, yb, "draw: amp hi", 0, 300, 0.001f, &p.regime_amp_hi, false, "%.3f");
    yb += 4;
    slider(xb, yb, "shelf width (mask, 0 off)", 0, 200, 0.001f, &p.shelf_width, false, "%.3f");
    slider(xb, yb, "shelf depth m", 0, 80, 0.01f, &p.shelf_depth, false, "%.2f");
    slider(xb, yb, "shelf from regime (/std)", 0, 100, 0.01f, &p.shelf_regime, false, "%.2f");

    const int y = std::max(ya, yb) + 8;
    ctl("BUTTON", "Recalibrate land % to these settings", 0, 8, y, 2 * COL + 8, 26, ID_L0_RECAL);
    g.l0_status = ctl("STATIC", "", 0, 12, y + 32, 2 * COL, 50, 0);
    RECT r = {0, 0, 2 * COL + 24, y + 90};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    SetWindowPos(g.l0_wnd, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    l0_write_controls();
}

void layout_right() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int x = rc.right - RIGHT + 6, w = RIGHT - 12;
    int y = 6;
    MoveWindow(g.right_labels[0], x, y, w, 16, TRUE); y += 18;
    MoveWindow(g.inspect, x, y, w, 172, TRUE); y += 178;
    MoveWindow(g.right_labels[1], x, y, w, 16, TRUE); y += 18;
    // Room for every line of the seed's table, then the scan takes the rest.
    const int info_h = std::max(180, std::min(398, static_cast<int>(rc.bottom - y) - 290));
    MoveWindow(g.seed_info, x, y, w, info_h, TRUE); y += info_h + 8;
    MoveWindow(g.scan_btn, x, y, 92, 24, TRUE);
    MoveWindow(g.scan_count, x + 98, y + 1, 46, 22, TRUE);
    MoveWindow(g.right_labels[2], x + 150, y + 4, w - 150, 18, TRUE);
    y += 30;
    MoveWindow(g.scan_note, x, y, w, 32, TRUE); y += 34;
    MoveWindow(g.scan_label, x, y, w, 30, TRUE); y += 32;
    MoveWindow(g.scan_list, x, y, w, std::max(40, static_cast<int>(rc.bottom) - y - 6), TRUE);
}

// --- actions --------------------------------------------------------------------

void go_to_seed(uint64_t s) {
    g.seed = s;
    SetWindowTextA(g.seed_edit, fmt("%llu", static_cast<unsigned long long>(s)).c_str());
    g.world_dirty = true;
    g.pinned = false;
    redraw(true);
}

std::string engine_command() {
    float x = g.spawn.x, z = g.spawn.z;
    if (g.pinned) { x = g.px; z = g.pz; }
    const float h = std::max(g.wp.sea_level, ground_at(x, z));
    // The engine's own opening shot: 60 m back along -z, 40 m up, looking
    // along +z at the point.
    return fmt("build\\engine.exe --cam %.1f %.1f %.1f 0 -0.55 --seed %llu",
               static_cast<double>(x), static_cast<double>(h + 40.0f), static_cast<double>(z - 60.0f),
               static_cast<unsigned long long>(g.seed));
}

// The sliders' world as the engine's BIOMES_L0 (empty at shipping) and as
// validate flags. Every layer 0 value is written, defaults included, so a
// line does not depend on what the defaults are when it is read back.
std::string l0_env() {
    if (!params_modified()) return "";
    const WorldParams& p = g.wp;
    std::string e = fmt("danger_w=%.2f,danger_feat=%.0f,land_offset=%.5f,centre_floor=%.2f,"
                        "regime_feat=%.0f,regime_mean=%.4f,regime_amp=%.4f,"
                        "fine_weight=%.2f,fine_regime=%.2f,fine_feat=%.0f",
                        p.danger_w, 1.0 / p.danger_freq, p.land_offset, p.centre_floor,
                        1.0 / p.regime_freq, p.regime_mean, p.regime_amp,
                        p.fine_weight, p.fine_regime, 1.0 / p.fine_freq);
    if (p.regime_draw)
        e += fmt(",regime_draw=1,regime_size_lo=%.0f,regime_size_hi=%.0f,regime_mean_lo=%.4f,"
                 "regime_mean_hi=%.4f,regime_amp_lo=%.4f,regime_amp_hi=%.4f",
                 p.regime_size_lo, p.regime_size_hi, p.regime_mean_lo, p.regime_mean_hi,
                 p.regime_amp_lo, p.regime_amp_hi);
    if (p.shelf_width > 0.0f)
        e += fmt(",shelf_width=%.3f,shelf_depth=%.2f,shelf_regime=%.2f",
                 p.shelf_width, p.shelf_depth, p.shelf_regime);
    if (p.knee) e += fmt(",knee=1,knee_k=%.3f,knee_c=%.3f", p.knee_k, p.knee_c);
    return e;
}

std::string validate_command() {
    const WorldParams& p = g.wp;
    std::string v = fmt("build\\validate.exe --w %.2f --danger-feat %.0f --land-offset %.5f "
                        "--centre-floor %.2f --regime-feat %.0f --regime-mean %.4f --regime-amp %.4f "
                        "--fine-weight %.2f --fine-regime %.2f --fine-feat %.0f",
                        p.danger_w, 1.0 / p.danger_freq, p.land_offset,
                        p.centre_floor, 1.0 / p.regime_freq, p.regime_mean, p.regime_amp,
                        p.fine_weight, p.fine_regime, 1.0 / p.fine_freq);
    if (p.regime_draw)
        v += fmt(" --regime-draw --regime-feat-range %.0f %.0f --regime-mean-range %.4f %.4f "
                 "--regime-amp-range %.4f %.4f",
                 p.regime_size_lo, p.regime_size_hi, p.regime_mean_lo, p.regime_mean_hi,
                 p.regime_amp_lo, p.regime_amp_hi);
    if (p.shelf_width > 0.0f)
        v += fmt(" --shelf-width %.3f --shelf-depth %.2f --shelf-regime %.2f",
                 p.shelf_width, p.shelf_depth, p.shelf_regime);
    if (p.knee) v += fmt(" --knee --knee-k %.3f --knee-c %.3f", p.knee_k, p.knee_c);
    return v;
}

void copy_command() {
    // PowerShell: set (or clear) BIOMES_L0 for the engine, then the sweep.
    // Also written to build/view_settings.txt.
    const std::string cmd = "$env:BIOMES_L0=\"" + l0_env() + "\"; " + engine_command() +
                            "\r\n" + validate_command() + "\r\n";
    if (FILE* f = std::fopen("build/view_settings.txt", "wb")) {
        std::fwrite(cmd.data(), 1, cmd.size(), f);
        std::fclose(f);
    }
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, cmd.size() + 1);
    if (mem) {
        std::memcpy(GlobalLock(mem), cmd.c_str(), cmd.size() + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
    g.reload_note = "copied the engine and validate commands (also build/view_settings.txt)\r\n";
    set_status();
}

void open_engine() {
    std::string cmd = engine_command();
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    // The child inherits this, so the engine builds the sliders' world; at
    // shipping it is cleared, in case the viewer was itself started with one.
    const std::string env = l0_env();
    SetEnvironmentVariableA("BIOMES_L0", env.empty() ? nullptr : env.c_str());
    if (CreateProcessA(0, line.data(), 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g.reload_note = "started: " + cmd + "\r\n";
    } else {
        g.reload_note = "could not start build\\engine.exe (built? run from the repo root)\r\n";
    }
    if (params_modified()) g.reload_note += "with the sliders' world (BIOMES_L0)\r\n";
    set_status();
}

void zoom_at(int sx, int sy, float factor) {
    float wx, wz;
    to_world(sx, sy, wx, wz);
    const float max_mpp = 3.0f * g.wp.radius / static_cast<float>(std::min(map_w(), map_h()));
    const float nm = std::max(0.25f, std::min(max_mpp, g.mpp * factor));
    // Keep the world point under the cursor where it is.
    g.cx = wx - (wx - g.cx) * (nm / g.mpp);
    g.cz = wz - (wz - g.cz) * (nm / g.mpp);
    g.mpp = nm;
    preview_then_refine();
}

void set_mode(int m) {
    g.mode = static_cast<ViewMode>(m);
    SendMessage(g.mode_box, CB_SETCURSEL, static_cast<WPARAM>(m), 0);
    redraw(true);
}

bool handle_key(WPARAM k) {
    switch (k) {
        case VK_PRIOR: go_to_seed(g.seed == 0 ? 0 : g.seed - 1); return true;
        case VK_NEXT:  go_to_seed(g.seed + 1); return true;
        case VK_HOME:  fit_view(); redraw(true); return true;
        case VK_F5:    reload_table(); return true;
        case VK_ESCAPE:
            g.pinned = false;
            g.marker = false;
            update_inspect();
            InvalidateRect(g.hwnd, 0, FALSE);
            return true;
        case VK_ADD: case VK_OEM_PLUS: {
            const RECT m = map_rect();
            zoom_at((m.left + m.right) / 2, (m.top + m.bottom) / 2, 1.0f / 1.5f);
            return true;
        }
        case VK_SUBTRACT: case VK_OEM_MINUS: {
            const RECT m = map_rect();
            zoom_at((m.left + m.right) / 2, (m.top + m.bottom) / 2, 1.5f);
            return true;
        }
        default:
            if (k >= '1' && k <= '5') { set_mode(static_cast<int>(k - '1')); return true; }
            return false;
    }
}

void slider_moved(HWND bar, int code) {
    const int pos = static_cast<int>(SendMessage(bar, TBM_GETPOS, 0, 0));
    // Only the slider that moved changes its parameter: re-reading all three
    // would round the land offset to the slider's 0.1% and quietly move the
    // world off the shipping one.
    if (bar == g.w_bar)         g.wp.danger_w = static_cast<float>(pos) / 100.0f;
    else if (bar == g.feat_bar) g.wp.danger_freq = 1.0f / static_cast<float>(pos);
    else if (bar == g.land_bar) g.wp.land_offset = g.calib.offset_for_land(static_cast<float>(pos) / 1000.0f);
    else return;
    g.world_dirty = true;
    if (code == TB_ENDTRACK || code == SB_ENDSCROLL) redraw(true);
    else redraw(false);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE:
            g.hwnd = h;
            build_controls();
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case ID_SEED_PREV: go_to_seed(g.seed == 0 ? 0 : g.seed - 1); break;
                case ID_SEED_NEXT: go_to_seed(g.seed + 1); break;
                case ID_SEED_GO: {
                    char buf[32];
                    GetWindowTextA(g.seed_edit, buf, sizeof(buf));
                    go_to_seed(std::strtoull(buf, nullptr, 10));
                    break;
                }
                case ID_MODE:
                    if (code == CBN_SELCHANGE)
                        set_mode(static_cast<int>(SendMessage(g.mode_box, CB_GETCURSEL, 0, 0)));
                    break;
                case ID_HIGHLIGHT:
                    if (code == CBN_SELCHANGE) {
                        g.highlight = static_cast<int>(SendMessage(g.hl_box, CB_GETCURSEL, 0, 0)) - 1;
                        redraw(true);
                    }
                    break;
                case ID_OV_PROPS: redraw(true); break;
                case ID_OV_SPAWN: case ID_OV_RINGS: case ID_OV_RIVERS: case ID_OV_FLATS: case ID_OV_GRID:
                    InvalidateRect(h, 0, FALSE);
                    break;
                case ID_RESET: {
                    const int grid = g.wp.grid;
                    g.wp = g.shipping;
                    g.wp.grid = grid;
                    if (!same_layer0(g.calib_wp, g.shipping)) recalibrate();
                    set_sliders_from_params();
                    l0_write_controls();
                    g.world_dirty = true;
                    redraw(true);
                    break;
                }
                case ID_L0_OPEN:
                    ShowWindow(g.l0_wnd, SW_SHOW);
                    SetForegroundWindow(g.l0_wnd);
                    break;
                case ID_RELOAD: reload_table(); break;
                case ID_ENGINE: open_engine(); break;
                case ID_COPY:   copy_command(); break;
                case ID_SCAN:
                    if (g.scanning) { stop_scan(); scan_finished(); }
                    else start_scan();
                    break;
                case ID_SCAN_LIST:
                    if (code == LBN_DBLCLK) {
                        const LRESULT sel = SendMessage(g.scan_list, LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR)
                            go_to_seed(static_cast<uint64_t>(SendMessage(g.scan_list, LB_GETITEMDATA, sel, 0)));
                    }
                    break;
            }
            if (id >= ID_OV_SPAWN && id <= ID_OV_GRID) SetFocus(h);
            return 0;
        }

        case WM_HSCROLL:
            slider_moved(reinterpret_cast<HWND>(lp), LOWORD(wp));
            return 0;

        case WM_TIMER:
            if (wp == TIMER_REFINE) {
                KillTimer(h, TIMER_REFINE);
                if (!g.dragging) redraw(true);
            } else if (wp == TIMER_RESIZE) {
                KillTimer(h, TIMER_RESIZE);
                redraw(true);
            } else if (wp == TIMER_WATCH && checked(g.auto_reload)) {
                FILETIME t;
                if (file_time(kArchetypes, t) && CompareFileTime(&t, &g.arch_time) != 0) reload_table();
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const int sx = static_cast<short>(LOWORD(lp)), sy = static_cast<short>(HIWORD(lp));
            SetFocus(h);
            if (in_map(sx, sy)) {
                g.dragging = true;
                g.panned = false;
                g.drag_x = sx; g.drag_y = sy;
                g.drag_cx = g.cx; g.drag_cz = g.cz;
                SetCapture(h);
            } else if (sx < LEFT && sy >= LEGEND_Y + legend_row_h() + 2 &&
                       (g.mode == VIEW_BIOME || g.mode == VIEW_BIOME_RELIEF)) {
                const int row = (sy - LEGEND_Y - legend_row_h() - 2) / legend_row_h();
                if (row >= 0 && row < static_cast<int>(g.table.size())) {
                    g.highlight = g.highlight == row ? -1 : row;
                    SendMessage(g.hl_box, CB_SETCURSEL, static_cast<WPARAM>(g.highlight + 1), 0);
                    redraw(true);
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int sx = static_cast<short>(LOWORD(lp)), sy = static_cast<short>(HIWORD(lp));
            if (g.dragging) {
                const int dx = sx - g.drag_x, dy = sy - g.drag_y;
                if (!g.panned && dx * dx + dy * dy > 16) g.panned = true;
                if (g.panned) {
                    g.cx = g.drag_cx - dx * g.mpp;
                    g.cz = g.drag_cz - dy * g.mpp;
                    InvalidateRect(h, 0, FALSE);
                }
            }
            const bool was = g.hover;
            g.hover = in_map(sx, sy);
            if (g.hover) {
                to_world(sx, sy, g.hx, g.hz);
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            if (g.hover || was) {
                update_inspect();
                if (g.marker) InvalidateRect(h, 0, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hover = false;
            update_inspect();
            if (g.marker) InvalidateRect(h, 0, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (!g.dragging) return 0;
            g.dragging = false;
            ReleaseCapture();
            const int sx = static_cast<short>(LOWORD(lp)), sy = static_cast<short>(HIWORD(lp));
            if (g.panned) {
                redraw(true);
            } else {
                g.pinned = true;
                to_world(sx, sy, g.px, g.pz);
                update_inspect();
                InvalidateRect(h, 0, FALSE);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            const int sx = static_cast<short>(LOWORD(lp)), sy = static_cast<short>(HIWORD(lp));
            if (in_map(sx, sy)) {
                g.marker = true;
                to_world(sx, sy, g.mx, g.mz);
                update_inspect();
                InvalidateRect(h, 0, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(h, &p);
            if (!in_map(p.x, p.y)) return 0;
            const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            zoom_at(p.x, p.y, std::pow(1.0f / 1.35f, static_cast<float>(notches)));
            return 0;
        }

        case WM_APP_SCAN_ROW: {
            SeedMetrics* row = reinterpret_cast<SeedMetrics*>(lp);
            g.scan_rows.push_back(*row);
            delete row;
            std::sort(g.scan_rows.begin(), g.scan_rows.end(),
                      [](const SeedMetrics& a, const SeedMetrics& b) { return a.seed < b.seed; });
            fill_scan_list();
            SetWindowTextA(g.scan_note, fmt("scanning... %zu done", g.scan_rows.size()).c_str());
            return 0;
        }
        case WM_APP_SCAN_DONE:
            if (g.scanning) scan_finished();
            return 0;

        case WM_PAINT:   on_paint(); return 0;
        case WM_SIZE:
            if (g.inspect) layout_right();
            SetTimer(h, TIMER_RESIZE, 200, 0);
            InvalidateRect(h, 0, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = LEFT + RIGHT + 300;
            mm->ptMinTrackSize.y = 760;
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY:
            stop_scan();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

// --- headless still (--shot) ---------------------------------------------------
// `view.exe --shot build/view.bmp [--seed n] [--mode 1-5] [--view cx cz mpp]
// [--pin x z] [--marker x z] [--highlight b] [--scan n] [--no-props]` opens
// the window, sets it up, writes what it shows to a BMP and its panels' text
// to <path>.txt, and exits. For checking the viewer without driving it, and
// for pictures of a seed to talk about.

std::string window_text(HWND c) {
    const int n = GetWindowTextLengthA(c);
    std::string s(static_cast<size_t>(n) + 1, '\0');
    GetWindowTextA(c, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void pump() {
    MSG msg;
    while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

bool write_shot(const std::string& path) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    HDC wdc = GetDC(g.hwnd);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ old = SelectObject(dc, bm);
    PrintWindow(g.hwnd, dc, 1 | 2);   // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = H;        // bottom-up, as a BMP stores it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    const int stride = (W * 3 + 3) & ~3;
    std::vector<unsigned char> px(static_cast<size_t>(stride) * H);
    SelectObject(dc, old);
    GetDIBits(dc, bm, 0, H, px.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(bm);
    DeleteDC(dc);
    ReleaseDC(g.hwnd, wdc);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    std::fwrite(px.data(), 1, px.size(), f);
    std::fclose(f);

    FILE* t = std::fopen((path + ".txt").c_str(), "w");
    if (t) {
        std::string all = "[status]\r\n" + window_text(g.status) + "\r\n[inspect]\r\n" + window_text(g.inspect) +
                          "\r\n[this seed]\r\n" + window_text(g.seed_info) + "\r\n[scan]\r\n" +
                          window_text(g.scan_note) + "\r\n" + window_text(g.scan_label) + "\r\n";
        const int n = static_cast<int>(SendMessage(g.scan_list, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < n; ++i) {
            char line[256] = {0};
            if (SendMessageA(g.scan_list, LB_GETTEXTLEN, i, 0) < 255)
                SendMessageA(g.scan_list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(line));
            all += std::string(line) + "\r\n";
        }
        std::string out;
        for (char c : all) if (c != '\r') out += c;
        std::fputs(out.c_str(), t);
        std::fclose(t);
    }
    return true;
}

// Every flag after `name`, as text; "" if absent.
const char* arg_after(const std::string& cl, const char* name) {
    const size_t p = cl.find(name);
    return p == std::string::npos ? nullptr : cl.c_str() + p + std::strlen(name);
}

int run_shot(const std::string& cl, const std::string& path) {
    if (const char* a = arg_after(cl, "--seed ")) { g.seed = std::strtoull(a, nullptr, 10); g.world_dirty = true; }
    if (const char* a = arg_after(cl, "--mode ")) {
        const int m = std::atoi(a) - 1;
        if (m >= 0 && m <= 4) { g.mode = static_cast<ViewMode>(m); SendMessage(g.mode_box, CB_SETCURSEL, m, 0); }
    }
    if (const char* a = arg_after(cl, "--view ")) {
        float v[3];
        if (std::sscanf(a, "%f %f %f", &v[0], &v[1], &v[2]) == 3) { g.cx = v[0]; g.cz = v[1]; g.mpp = v[2]; }
    }
    if (const char* a = arg_after(cl, "--pin ")) {
        if (std::sscanf(a, "%f %f", &g.px, &g.pz) == 2) g.pinned = true;
    }
    if (const char* a = arg_after(cl, "--marker ")) {
        if (std::sscanf(a, "%f %f", &g.mx, &g.mz) == 2) g.marker = true;
    }
    if (const char* a = arg_after(cl, "--highlight ")) {
        g.highlight = std::atoi(a);
        SendMessage(g.hl_box, CB_SETCURSEL, static_cast<WPARAM>(g.highlight + 1), 0);
    }
    if (cl.find("--l0-window") != std::string::npos) ShowWindow(g.l0_wnd, SW_SHOWNOACTIVATE);
    if (cl.find("--no-props") != std::string::npos) SendMessage(g.ov_props, BM_SETCHECK, BST_UNCHECKED, 0);
    if (cl.find("--rivers") != std::string::npos) SendMessage(g.ov_rivers, BM_SETCHECK, BST_CHECKED, 0);
    if (cl.find("--rings") != std::string::npos) SendMessage(g.ov_rings, BM_SETCHECK, BST_CHECKED, 0);
    if (cl.find("--grid") != std::string::npos) SendMessage(g.ov_grid, BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextA(g.seed_edit, fmt("%llu", static_cast<unsigned long long>(g.seed)).c_str());
    redraw(true);
    KillTimer(g.hwnd, TIMER_REFINE);
    if (const char* a = arg_after(cl, "--scan ")) {
        SetWindowTextA(g.scan_count, fmt("%d", std::atoi(a)).c_str());
        start_scan();
        while (g.scanning) {
            MSG msg;
            if (GetMessage(&msg, 0, 0, 0) <= 0) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    pump();
    InvalidateRect(g.hwnd, 0, FALSE);
    UpdateWindow(g.hwnd);
    pump();
    const bool ok = write_shot(path);
    stop_scan();
    DestroyWindow(g.hwnd);
    return ok ? 0 : 1;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    try {
        g.table = load_archetypes(kArchetypes);
    } catch (const std::exception& e) {
        MessageBoxA(0, (std::string("Could not load archetypes.json:\n\n") + e.what() +
                        "\n\nRun this from the repository root.").c_str(),
                    "biomes2 viewer", MB_ICONERROR);
        return 1;
    }
    file_time(kArchetypes, g.arch_time);
    g.shipping = g.wp;

    // Build the land-fraction curve before any control reads it. Two seconds
    // once at startup, against about a second for every slider step if the
    // offset were bisected on demand. Coarse on purpose -- it only has to be
    // accurate enough to place a slider, and the harness is what measures.
    {
        WorldParams cw = g.wp;
        cw.grid = kGridPreview;
        g.calib.build(cw, 16, 192, -0.25f, 0.30f, 22);
        g.calib_wp = g.wp;
    }

    // BIOMES_L0, as Copy command writes it and the engine reads it: start on
    // those settings. The curve above is today's, so the land % label says
    // STALE until it is recalibrated; the offset is the string's own.
    if (const char* env = std::getenv("BIOMES_L0")) {
        if (apply_param_spec(g.wp, env) > 0) g.reload_note = "settings from BIOMES_L0\r\n";
    }

    g.ui_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                            0, "Segoe UI");
    g.mono_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                              FIXED_PITCH, "Consolas");

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = "biomes2_view";
    RegisterClassA(&wc);

    // Plain ASCII: this is the ANSI entry point, and an em-dash arrives mangled.
    HWND h = CreateWindowA("biomes2_view", "biomes2 - world viewer",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                           LEFT + RIGHT + 900, 980, 0, 0, inst, 0);
    fill_highlight_box();
    build_l0_window(inst);
    set_sliders_from_params();
    layout_right();
    fit_view();
    ShowWindow(h, show);
    UpdateWindow(h);

    const std::string cl = GetCommandLineA();
    if (const char* a = arg_after(cl, "--shot ")) {
        std::string path;
        while (*a == ' ') ++a;
        while (*a && *a != ' ') path += *a++;
        KillTimer(h, TIMER_RESIZE);
        return run_shot(cl, path.empty() ? std::string("build/view.bmp") : path);
    }

    redraw(true);
    KillTimer(h, TIMER_RESIZE);
    SetTimer(h, TIMER_WATCH, 1000, 0);

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            const HWND f = GetFocus();
            const bool in_edit = (f == g.seed_edit || f == g.scan_count);
            if (in_edit && msg.wParam == VK_RETURN) {
                if (f == g.seed_edit) SendMessage(h, WM_COMMAND, ID_SEED_GO, 0);
                else start_scan();
                continue;
            }
            if (!in_edit && handle_key(msg.wParam)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
