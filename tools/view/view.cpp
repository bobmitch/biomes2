// ---------------------------------------------------------------------------
// Native world viewer (ADR-002).
//
// Links worldgen/ directly, so what is on screen is the shipping generator
// rather than a translation of it onto another backend. That is the whole
// argument for replacing the browser prototype: the thing being tuned against
// is now the thing that ships.
//
// Raw Win32 on purpose -- no dependencies, and small enough to fold into the
// engine later as a hidden debug mode.
//
// Costs, measured: a world rebuild is ~450 ms and relief is ~1.9 us/px, so a
// full-resolution relief redraw is seconds. Hence two resolutions: a coarse
// preview while a slider is moving, full detail when it is released. Biome and
// danger need one height sample per pixel rather than three and are far
// cheaper.
//
// The right-hand column is the layer 0 PROTOTYPE (docs/worldgen-design.md,
// "Layer 0, proposed"): the regime field, the fine continent noise, per-seed
// draws, the shelf and the land knee. Every slider starts at today's world.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // windows.h defines min/max as macros, which breaks std::min
#include <windows.h>
#include <commctrl.h>

#include "worldgen/world.h"
#include "tools/shared/render.h"
#include "tools/shared/land_solve.h"
#include "tools/shared/reach.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace worldgen;
using namespace view;

namespace {

enum {
    ID_SEED_PREV = 100, ID_SEED_NEXT, ID_MODE, ID_LANDOFF, ID_RECAL,
    ID_DRAW, ID_KNEE, ID_RESET, ID_SLIDER0 = 200
};

// The islandness slider reads in LAND FRACTION, tenths of a percent, because
// that is the number that decides whether the world is a continent or an
// archipelago. land_offset is an opaque proxy for it.
//
// The offset is read off a calibration curve, so the solve is GLOBAL: the
// slider sets the MEAN land fraction across seeds and leaves each seed free to
// land above or below. Per-seed solving would flatten exactly the spread the
// islandness comes from. The curve is built at startup against today's noise;
// "recalibrate" rebuilds it against the current layer 0 settings (knee
// included), and the label says which curve the slider is reading.
const int LAND_MIN = 200;    // 20.0%
const int LAND_MAX = 550;    // 55.0%

const int COL = 236;           // one column of controls
const int PANEL = 2 * COL + 8;
const int PREVIEW_PX = 220;    // while dragging
const int FULL_PX = 768;       // on release

// A slider over one float parameter. The trackbar holds integers, so value =
// pos * scale. `metres_to_freq` sliders show a feature size in metres and set
// a frequency of 1 / size.
struct Slider {
    const char* name;
    int lo, hi;
    float scale;
    float* target;
    bool metres_to_freq;
    const char* fmt;
    HWND bar = 0, lab = 0;
};

struct App {
    WorldParams  wp;
    WorldParams  defaults;
    AssignParams ap;
    ArchetypeTable table;
    SeedWorld    world;
    LandCalibration calib;
    WorldParams  calib_wp;          // the settings the curve was built at
    bool         calib_today = true;
    uint64_t     seed = 0;
    ViewMode     mode = VIEW_RELIEF;
    bool         world_dirty = true;
    int          rendered_px = 0;
    int          land10 = 320;      // the land slider, tenths of a percent

    std::vector<unsigned char> rgb;   // tile*tile*3
    std::vector<unsigned char> bgra;  // tile*tile*4, what Windows blits
    int tile = 0;

    HWND hwnd = 0, mode_box = 0, land_bar = 0, land_lab = 0, status = 0;
    HWND draw_box = 0, knee_box = 0;
    std::vector<Slider> sliders;

    // Readings of the world on screen, recomputed on every rebuild.
    Landmasses lm;
    FootReach  reach;
    SpawnPoint spawn;
    float      build_ms = 0.0f;
};

App g;

// Whether two parameter sets give the same layer 0 (and so the same land
// fraction curve). The offset is left out: it is what the curve varies.
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
           a.knee == b.knee && a.knee_k == b.knee_k && a.knee_c == b.knee_c;
}

void rebuild_world() {
    const std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    g.world = build_world(g.seed, g.wp, g.table, g.ap);
    const std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    g.build_ms = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
    g.spawn = find_spawn(g.world, g.wp, g.table);
    g.lm = landmasses(g.world, cell_at(g.world, g.wp, g.spawn.x, g.spawn.z));
    g.reach = foot_reach(g.world, g.wp, g.spawn.x, g.spawn.z, 384);
    g.world_dirty = false;
}

void render(int px) {
    if (g.world_dirty) rebuild_world();
    g.tile = px;
    g.rgb.assign(static_cast<size_t>(px) * px * 3, 0);
    render_view(g.world, g.wp, g.table, g.mode, g.rgb, px, 0, 0, px);

    // The spawn, as a small white cross.
    {
        const float R = g.wp.radius;
        const int sx = static_cast<int>((g.spawn.x + R) / (2.0f * R) * px);
        const int sz = static_cast<int>((g.spawn.z + R) / (2.0f * R) * px);
        for (int d = -4; d <= 4; ++d) {
            const int pts[2][2] = {{sx + d, sz}, {sx, sz + d}};
            for (int q = 0; q < 2; ++q) {
                const int x = pts[q][0], y = pts[q][1];
                if (x < 0 || y < 0 || x >= px || y >= px) continue;
                unsigned char* c = &g.rgb[(static_cast<size_t>(y) * px + x) * 3];
                c[0] = 255; c[1] = 255; c[2] = 255;
            }
        }
    }

    // Windows wants BGRA, bottom-up handled by a negative height below.
    g.bgra.resize(static_cast<size_t>(px) * px * 4);
    for (size_t i = 0, n = static_cast<size_t>(px) * px; i < n; ++i) {
        g.bgra[i * 4 + 0] = g.rgb[i * 3 + 2];
        g.bgra[i * 4 + 1] = g.rgb[i * 3 + 1];
        g.bgra[i * 4 + 2] = g.rgb[i * 3 + 0];
        g.bgra[i * 4 + 3] = 255;
    }
    g.rendered_px = px;
}

void set_land_label() {
    const bool stale = !same_layer0(g.wp, g.calib_wp);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "land %% mean %.1f  [%s%s]", g.land10 / 10.0,
                  g.calib_today ? "calib: today's noise" : "calib: these settings",
                  stale ? ", STALE" : "");
    SetWindowTextA(g.land_lab, buf);
}

void set_status() {
    // std::snprintf, NOT wsprintfA: the latter supports no %f and mishandles
    // %llu, which silently shifts every argument after it.
    const SeedWorld& w = g.world;
    const bool stale = !same_layer0(g.wp, g.calib_wp);
    char buf[1400];
    std::snprintf(buf, sizeof(buf),
                  "seed %llu   cells %d   rivers %d   %d px   build %.0f ms\r\n"
                  "offset %+.4f (%s), this seed %+.4f\r\n"
                  "\r\n"
                  "land: before knee %.1f%%  after %.1f%%  (build grid %.1f%%)%s\r\n"
                  "knee %s, %.1f ms\r\n"
                  "landmasses %d (cell graph)   spawn landmass holds %.0f%% of land\r\n"
                  "on foot from spawn: %.1f%% of land (%.1f km2)\r\n"
                  "biomes on foot: %d of %d\r\n"
                  "shelf (water < 0.85 m): %.1f km2   land %.1f km2\r\n"
                  "regime: size %.0f m  mean %+.3f  amp %.3f%s  %s\r\n"
                  "spawn (%.0f, %.0f)%s",
                  static_cast<unsigned long long>(g.seed),
                  static_cast<int>(w.pts.size()), static_cast<int>(w.rivers.size()),
                  g.rendered_px, static_cast<double>(g.build_ms),
                  static_cast<double>(g.wp.land_offset),
                  stale ? "raw: the curve is stale"
                        : (g.calib_today ? "from today's curve" : "from this curve"),
                  static_cast<double>(w.l0.land_offset),
                  100.0 * w.knee_raw_land, 100.0 * w.knee_land, 100.0 * w.land_fraction,
                  g.wp.knee ? "" : " [knee off]",
                  g.wp.knee ? (w.knee_land < w.knee_raw_land ? "bit" : "did not bite") : "off",
                  static_cast<double>(w.knee_ms),
                  g.lm.count, 100.0 * g.lm.home_share,
                  100.0 * g.reach.reach_share, static_cast<double>(g.reach.reach_km2),
                  g.reach.biomes_reached, g.reach.biomes_present,
                  static_cast<double>(g.reach.shelf_km2), static_cast<double>(g.reach.land_km2),
                  w.l0.regime_freq > 0.0f ? 1.0 / w.l0.regime_freq : 0.0,
                  static_cast<double>(w.l0.regime_mean), static_cast<double>(w.l0.regime_amp),
                  g.wp.regime_draw ? " (drawn)" : " (fixed)",
                  w.l0.regime_on ? "" : "[regime off]",
                  static_cast<double>(g.spawn.x), static_cast<double>(g.spawn.z),
                  g.spawn.strict ? "" : " (fallback)");
    SetWindowTextA(g.status, buf);
    set_land_label();
}

void redraw(int px) {
    SetCursor(LoadCursor(0, IDC_WAIT));
    render(px);
    set_status();
    SetCursor(LoadCursor(0, IDC_ARROW));
    InvalidateRect(g.hwnd, 0, TRUE);
}

float slider_value(const Slider& s) {
    const int pos = static_cast<int>(SendMessage(s.bar, TBM_GETPOS, 0, 0));
    return static_cast<float>(pos) * s.scale;
}

void set_slider_label(const Slider& s) {
    char v[48], buf[128];
    std::snprintf(v, sizeof(v), s.fmt, static_cast<double>(slider_value(s)));
    std::snprintf(buf, sizeof(buf), "%s  %s", s.name, v);
    SetWindowTextA(s.lab, buf);
}

void read_controls() {
    for (size_t i = 0; i < g.sliders.size(); ++i) {
        const Slider& s = g.sliders[i];
        const float v = slider_value(s);
        *s.target = s.metres_to_freq ? 1.0f / (v > 1.0f ? v : 1.0f) : v;
        set_slider_label(s);
    }
    g.wp.regime_draw = SendMessage(g.draw_box, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.wp.knee = SendMessage(g.knee_box, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.land10 = static_cast<int>(SendMessage(g.land_bar, TBM_GETPOS, 0, 0));
    g.wp.land_offset = g.calib.offset_for_land(static_cast<float>(g.land10) / 1000.0f);
    g.mode = static_cast<ViewMode>(SendMessage(g.mode_box, CB_GETCURSEL, 0, 0));
}

void write_controls() {
    for (size_t i = 0; i < g.sliders.size(); ++i) {
        const Slider& s = g.sliders[i];
        const float v = s.metres_to_freq ? 1.0f / *s.target : *s.target;
        SendMessage(s.bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(v / s.scale + (v >= 0 ? 0.5f : -0.5f)));
        set_slider_label(s);
    }
    SendMessage(g.draw_box, BM_SETCHECK, g.wp.regime_draw ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g.knee_box, BM_SETCHECK, g.wp.knee ? BST_CHECKED : BST_UNCHECKED, 0);
}

// The land fraction curve, at the current layer 0 settings. Wider than the
// startup curve: the regime's mean moves the whole curve sideways.
void recalibrate() {
    SetCursor(LoadCursor(0, IDC_WAIT));
    g.calib.build(g.wp, 16, 192, -0.40f, 0.40f, 32);
    g.calib_wp = g.wp;
    g.calib_today = same_layer0(g.wp, g.defaults);
    g.wp.land_offset = g.calib.offset_for_land(static_cast<float>(g.land10) / 1000.0f);
    SetCursor(LoadCursor(0, IDC_ARROW));
}

void on_paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g.hwnd, &ps);

    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int vx = PANEL, vy = 0;
    const int vw = rc.right - PANEL, vh = rc.bottom;
    const int side = (vw < vh ? vw : vh);

    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 26));
    RECT fill = {PANEL, 0, rc.right, rc.bottom};
    FillRect(dc, &fill, bg);
    DeleteObject(bg);

    if (g.tile > 0 && side > 0) {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g.tile;
        bi.bmiHeader.biHeight = -g.tile;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc,
                      vx + (vw - side) / 2, vy + (vh - side) / 2, side, side,
                      0, 0, g.tile, g.tile,
                      g.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(g.hwnd, &ps);
}

HWND label(HWND parent, const char* text, int x, int y, int w = COL - 16) {
    return CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 16, parent, 0, 0, 0);
}

void add_slider(HWND h, int x, int& y, const char* name, int lo, int hi, float scale,
                float* target, bool metres_to_freq, const char* fmt) {
    Slider s;
    s.name = name; s.lo = lo; s.hi = hi; s.scale = scale; s.target = target;
    s.metres_to_freq = metres_to_freq; s.fmt = fmt;
    s.lab = label(h, name, x + 4, y);
    y += 16;
    const int id = ID_SLIDER0 + static_cast<int>(g.sliders.size());
    s.bar = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                          x, y, COL - 8, 24, h, (HMENU)(INT_PTR)id, 0, 0);
    SendMessage(s.bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    y += 28;
    g.sliders.push_back(s);
}

void build_controls(HWND h) {
    // --- left column: what was here, and the layer 0 shape ----------------
    const int xa = 4;
    int y = 8;
    CreateWindowA("BUTTON", "< seed", WS_CHILD | WS_VISIBLE, xa + 4, y, 70, 24, h, (HMENU)ID_SEED_PREV, 0, 0);
    CreateWindowA("BUTTON", "seed >", WS_CHILD | WS_VISIBLE, xa + 80, y, 70, 24, h, (HMENU)ID_SEED_NEXT, 0, 0);
    CreateWindowA("BUTTON", "defaults", WS_CHILD | WS_VISIBLE, xa + 156, y, 72, 24, h, (HMENU)ID_RESET, 0, 0);
    y += 32;

    g.mode_box = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                               xa, y, COL - 8, 200, h, (HMENU)ID_MODE, 0, 0);
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Biome map");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Shaded relief");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Danger field");
    SendMessage(g.mode_box, CB_SETCURSEL, static_cast<WPARAM>(g.mode), 0);
    y += 32;

    add_slider(h, xa, y, "danger wobble w", 0, 100, 0.01f, &g.wp.danger_w, false, "%.2f");
    add_slider(h, xa, y, "danger feature m", 1500, 14000, 1.0f, &g.wp.danger_freq, true, "%.0f");

    // The islandness lever, in land %, read off the calibration curve.
    g.land_lab = label(h, "land %", xa + 4, y);
    y += 16;
    g.land_bar = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                               xa, y, COL - 8, 24, h, (HMENU)ID_LANDOFF, 0, 0);
    SendMessage(g.land_bar, TBM_SETRANGE, TRUE, MAKELPARAM(LAND_MIN, LAND_MAX));
    SendMessage(g.land_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(g.land10));
    y += 26;
    CreateWindowA("BUTTON", "recalibrate land % to these settings", WS_CHILD | WS_VISIBLE,
                  xa + 4, y, COL - 16, 22, h, (HMENU)ID_RECAL, 0, 0);
    y += 30;

    add_slider(h, xa, y, "centre floor", 0, 30, 0.01f, &g.wp.centre_floor, false, "%.2f");
    add_slider(h, xa, y, "regime feature m", 1500, 14000, 1.0f, &g.wp.regime_freq, true, "%.0f");
    add_slider(h, xa, y, "regime mean (mask)", -150, 150, 0.001f, &g.wp.regime_mean, false, "%+.3f");
    add_slider(h, xa, y, "regime amp (mask/std)", 0, 300, 0.001f, &g.wp.regime_amp, false, "%.3f");
    add_slider(h, xa, y, "fine weight", 0, 100, 0.01f, &g.wp.fine_weight, false, "%.2f");
    add_slider(h, xa, y, "fine from regime (/std)", 0, 100, 0.01f, &g.wp.fine_regime, false, "%.2f");
    add_slider(h, xa, y, "fine feature m", 600, 4000, 1.0f, &g.wp.fine_freq, true, "%.0f");

    // --- right column: per-seed draws, the shelf, the knee -----------------
    const int xb = COL + 4;
    y = 8;
    g.draw_box = CreateWindowA("BUTTON", "draw regime per seed", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               xb + 4, y, COL - 16, 22, h, (HMENU)ID_DRAW, 0, 0);
    y += 26;
    add_slider(h, xb, y, "draw: size lo m", 1500, 14000, 1.0f, &g.wp.regime_size_lo, false, "%.0f");
    add_slider(h, xb, y, "draw: size hi m", 1500, 14000, 1.0f, &g.wp.regime_size_hi, false, "%.0f");
    add_slider(h, xb, y, "draw: mean lo", -150, 150, 0.001f, &g.wp.regime_mean_lo, false, "%+.3f");
    add_slider(h, xb, y, "draw: mean hi", -150, 150, 0.001f, &g.wp.regime_mean_hi, false, "%+.3f");
    add_slider(h, xb, y, "draw: amp lo", 0, 300, 0.001f, &g.wp.regime_amp_lo, false, "%.3f");
    add_slider(h, xb, y, "draw: amp hi", 0, 300, 0.001f, &g.wp.regime_amp_hi, false, "%.3f");
    y += 4;
    add_slider(h, xb, y, "shelf width (mask, 0 off)", 0, 200, 0.001f, &g.wp.shelf_width, false, "%.3f");
    add_slider(h, xb, y, "shelf depth m", 0, 80, 0.01f, &g.wp.shelf_depth, false, "%.2f");
    add_slider(h, xb, y, "shelf from regime (/std)", 0, 100, 0.01f, &g.wp.shelf_regime, false, "%.2f");
    y += 4;
    g.knee_box = CreateWindowA("BUTTON", "land knee", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               xb + 4, y, COL - 16, 22, h, (HMENU)ID_KNEE, 0, 0);
    y += 26;
    add_slider(h, xb, y, "knee k", 250, 450, 0.001f, &g.wp.knee_k, false, "%.3f");
    add_slider(h, xb, y, "knee ceiling c", 300, 550, 0.001f, &g.wp.knee_c, false, "%.3f");

    int ys = 0;
    for (size_t i = 0; i < g.sliders.size(); ++i) {
        RECT r; GetWindowRect(g.sliders[i].bar, &r);
        POINT p = {r.left, r.bottom}; ScreenToClient(h, &p);
        if (p.y > ys) ys = p.y;
    }
    g.status = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                             8, ys + 10, PANEL - 16, 220, h, 0, 0, 0);
    // The GUI font, not the bold system one, so the readouts fit the panel.
    EnumChildWindows(h, [](HWND c, LPARAM) -> BOOL {
        SendMessage(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return TRUE;
    }, 0);
    write_controls();
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE:
            g.hwnd = h;
            build_controls(h);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_SEED_PREV || id == ID_SEED_NEXT) {
                g.seed += (id == ID_SEED_NEXT) ? 1 : static_cast<uint64_t>(-1);
                g.world_dirty = true;
                redraw(FULL_PX);
            } else if (id == ID_MODE && HIWORD(wp) == CBN_SELCHANGE) {
                read_controls();
                redraw(FULL_PX);     // the world is unchanged, only the colours
            } else if (id == ID_DRAW || id == ID_KNEE) {
                read_controls();
                g.world_dirty = true;
                redraw(FULL_PX);
            } else if (id == ID_RECAL) {
                read_controls();
                recalibrate();
                g.world_dirty = true;
                redraw(FULL_PX);
            } else if (id == ID_RESET) {
                const float keep_off = g.wp.land_offset;
                g.wp = g.defaults;
                g.wp.land_offset = keep_off;
                write_controls();
                read_controls();
                g.world_dirty = true;
                redraw(FULL_PX);
            }
            return 0;
        }

        // Dragging rebuilds the world on every step, so preview coarse and
        // only pay for detail once the slider is let go.
        case WM_HSCROLL: {
            const int code = LOWORD(wp);
            read_controls();
            g.world_dirty = true;
            if (code == TB_THUMBTRACK) redraw(PREVIEW_PX);
            else if (code == TB_ENDTRACK || code == SB_ENDSCROLL) redraw(FULL_PX);
            else redraw(PREVIEW_PX);
            return 0;
        }

        case WM_PAINT:   on_paint(); return 0;
        case WM_SIZE:    InvalidateRect(h, 0, TRUE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    try {
        g.table = load_archetypes("worldgen/data/archetypes.json");
    } catch (const std::exception& e) {
        MessageBoxA(0, (std::string("Could not load archetypes.json:\n\n") + e.what() +
                        "\n\nRun this from the repository root.").c_str(),
                    "biomes2 viewer", MB_ICONERROR);
        return 1;
    }

    // The viewer rasterises at 512 rather than the harness's 1024: it changes
    // cell areas slightly and so is NOT what the golden hashes are taken at,
    // but it halves the rebuild and nothing here is pinned.
    g.wp.grid = 512;
    g.defaults = g.wp;

    // Build the land-fraction curve before any control reads it. Two seconds
    // once at startup, against about a second for every slider step if the
    // offset were bisected on demand. Coarse on purpose -- it only has to be
    // accurate enough to place a slider, and the harness is what measures.
    g.calib.build(g.wp, 16, 192, -0.25f, 0.30f, 22);
    g.calib_wp = g.wp;
    g.land10 = static_cast<int>(g.calib.land_for_offset(g.wp.land_offset) * 1000.0f + 0.5f);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(28, 31, 37));
    wc.lpszClassName = "biomes2_view";
    RegisterClassA(&wc);

    // Plain ASCII: this is the ANSI entry point, and an em-dash arrives mangled.
    HWND h = CreateWindowA("biomes2_view", "biomes2 - world viewer",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           PANEL + 860, 940, 0, 0, inst, 0);
    ShowWindow(h, show);
    redraw(FULL_PX);

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
