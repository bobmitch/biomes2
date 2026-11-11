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
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // windows.h defines min/max as macros, which breaks std::min
#include <windows.h>
#include <commctrl.h>

#include "worldgen/world.h"
#include "tools/shared/render.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace worldgen;
using namespace view;

namespace {

enum { ID_SEED_PREV = 100, ID_SEED_NEXT, ID_W, ID_FEAT, ID_MODE, ID_REPAIR, ID_LANDOFF };

// land_offset maps onto an integer trackbar as (pos - 100) / 1000, so the bar
// spans -0.100 to +0.300. The shipping 0.17 sits near the top of that, which
// is the point: the interesting range is all BELOW it.
const int LANDOFF_BIAS = 100;
const int LANDOFF_MIN  = 0;      // -0.100
const int LANDOFF_MAX  = 400;    // +0.300

const int PANEL = 232;
const int PREVIEW_PX = 220;    // while dragging
const int FULL_PX = 768;       // on release

struct App {
    WorldParams  wp;
    AssignParams ap;
    ArchetypeTable table;
    SeedWorld    world;
    uint64_t     seed = 0;
    ViewMode     mode = VIEW_RELIEF;
    bool         world_dirty = true;
    int          rendered_px = 0;

    std::vector<unsigned char> rgb;   // tile*tile*3
    std::vector<unsigned char> bgra;  // tile*tile*4, what Windows blits
    int tile = 0;

    HWND hwnd = 0, w_bar = 0, feat_bar = 0, mode_box = 0, land_bar = 0, status = 0;

    // Islandness of the world on screen, recomputed on every rebuild.
    int landmasses = 0;
    int biggest_pct = 0;  // share of land cells on the LARGEST landmass
                          // (the harness reports the spawn one; the viewer
                          // has no spawn, and on one continent they agree)
};

App g;

// Connected components of land cells. The same measurement the harness
// prints as "islandness" -- here so the land_offset slider shows its effect
// as a number and not only as a shape. Whether a world should be one
// continent or an archipelago is a judgement made by looking; this is the
// readout that makes looking quantitative.
void measure_islandness() {
    const std::vector<CellRecord>& cells = g.world.cells;
    const CellGraph& graph = g.world.graph;
    const int n = static_cast<int>(cells.size());
    std::vector<int> comp(n, -1);
    std::vector<int> stack;
    int land_total = 0, biggest = 0;
    for (int i = 0; i < n; ++i) if (cells[i].is_land) ++land_total;
    g.landmasses = 0;
    for (int i = 0; i < n; ++i) {
        if (!cells[i].is_land || comp[i] >= 0) continue;
        const int id = g.landmasses++;
        int size = 0;
        comp[i] = id;
        stack.push_back(i);
        while (!stack.empty()) {
            const int c = stack.back(); stack.pop_back();
            ++size;
            for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                const int nb = graph.list[k];
                if (comp[nb] >= 0 || !cells[nb].is_land) continue;
                comp[nb] = id;
                stack.push_back(nb);
            }
        }
        if (size > biggest) biggest = size;
    }
    g.biggest_pct = land_total > 0 ? (100 * biggest) / land_total : 0;
}

void rebuild_world() {
    g.world = build_world(g.seed, g.wp, g.table, g.ap);
    measure_islandness();
    g.world_dirty = false;
}

void render(int px) {
    if (g.world_dirty) rebuild_world();
    g.tile = px;
    g.rgb.assign(static_cast<size_t>(px) * px * 3, 0);
    render_view(g.world, g.wp, g.table, g.mode, g.rgb, px, 0, 0, px);

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

void set_status() {
    // std::snprintf, NOT wsprintfA: the latter supports no %f and mishandles
    // %llu, which silently shifts every argument after it. That cost an
    // access violation the moment a %s was added to this string.
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "seed %llu   w %.2f   feature %d m\r\n"
                  "cells %d   rivers %d   render %d px\r\n"
                  "\r\nland offset %+.3f   land %.0f%%\r\n"
                  "landmasses %d   biggest holds %d%% of land",
                  static_cast<unsigned long long>(g.seed),
                  static_cast<double>(g.wp.danger_w),
                  static_cast<int>(1.0f / g.wp.danger_freq),
                  static_cast<int>(g.world.pts.size()),
                  static_cast<int>(g.world.rivers.size()),
                  g.rendered_px,
                  static_cast<double>(g.wp.land_offset),
                  100.0 * g.world.land_fraction,
                  g.landmasses, g.biggest_pct);
    SetWindowTextA(g.status, buf);
}

void redraw(int px) {
    SetCursor(LoadCursor(0, IDC_WAIT));
    render(px);
    set_status();
    SetCursor(LoadCursor(0, IDC_ARROW));
    InvalidateRect(g.hwnd, 0, TRUE);
}

void read_controls() {
    const int w10 = static_cast<int>(SendMessage(g.w_bar, TBM_GETPOS, 0, 0));
    g.wp.danger_w = static_cast<float>(w10) / 100.0f;
    const int feat = static_cast<int>(SendMessage(g.feat_bar, TBM_GETPOS, 0, 0));
    g.wp.danger_freq = 1.0f / static_cast<float>(feat);
    const int off = static_cast<int>(SendMessage(g.land_bar, TBM_GETPOS, 0, 0));
    g.wp.land_offset = static_cast<float>(off - LANDOFF_BIAS) / 1000.0f;
    g.mode = static_cast<ViewMode>(SendMessage(g.mode_box, CB_GETCURSEL, 0, 0));
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

HWND label(HWND parent, const char* text, int y) {
    return CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE,
                         12, y, PANEL - 24, 16, parent, 0, 0, 0);
}

void build_controls(HWND h) {
    int y = 12;
    CreateWindowA("BUTTON", "< seed", WS_CHILD | WS_VISIBLE,
                  12, y, 90, 26, h, (HMENU)ID_SEED_PREV, 0, 0);
    CreateWindowA("BUTTON", "seed >", WS_CHILD | WS_VISIBLE,
                  118, y, 90, 26, h, (HMENU)ID_SEED_NEXT, 0, 0);
    y += 40;

    label(h, "danger wobble w", y); y += 18;
    g.w_bar = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                            8, y, PANEL - 16, 28, h, (HMENU)ID_W, 0, 0);
    SendMessage(g.w_bar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessage(g.w_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(g.wp.danger_w * 100.0f));
    y += 36;

    label(h, "danger feature size (m)", y); y += 18;
    g.feat_bar = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                               8, y, PANEL - 16, 28, h, (HMENU)ID_FEAT, 0, 0);
    SendMessage(g.feat_bar, TBM_SETRANGE, TRUE, MAKELPARAM(1500, 14000));
    SendMessage(g.feat_bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(1.0f / g.wp.danger_freq));
    y += 36;

    // The islandness lever. Below about +0.08 the land fraction crosses the
    // percolation threshold and the single continent starts breaking up.
    label(h, "land offset (islandness)", y); y += 18;
    g.land_bar = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                               8, y, PANEL - 16, 28, h, (HMENU)ID_LANDOFF, 0, 0);
    SendMessage(g.land_bar, TBM_SETRANGE, TRUE, MAKELPARAM(LANDOFF_MIN, LANDOFF_MAX));
    SendMessage(g.land_bar, TBM_SETPOS, TRUE,
                static_cast<LPARAM>(g.wp.land_offset * 1000.0f) + LANDOFF_BIAS);
    y += 36;

    label(h, "view", y); y += 18;
    g.mode_box = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                               8, y, PANEL - 16, 200, h, (HMENU)ID_MODE, 0, 0);
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Biome map");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Shaded relief");
    SendMessageA(g.mode_box, CB_ADDSTRING, 0, (LPARAM)"Danger field");
    SendMessage(g.mode_box, CB_SETCURSEL, static_cast<WPARAM>(g.mode), 0);
    y += 40;

    g.status = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                             12, y, PANEL - 24, 120, h, 0, 0, 0);
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
                           PANEL + 820, 860, 0, 0, inst, 0);
    ShowWindow(h, show);
    redraw(FULL_PX);

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
