#include "engine/field/world_gpu.h"

#include "worldgen/generator_version.h"
#include "worldgen/assign/json_lite.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

namespace engine {
namespace {

std::string hr_text(const char* what, HRESULT hr) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

bool make_default(ID3D12Device* d, UINT64 bytes, ComPtr<ID3D12Resource>& out,
                  std::string& error, const char* what) {
    if (bytes == 0) bytes = 16;   // a zero-sized buffer is not a legal resource
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // The kernel reads these through root UAVs, and a root descriptor is not
    // bounds checked: without this flag the binding is invalid, and what it
    // cost was a hung GPU with no validation available to say why.
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

}  // namespace

// One buffer: make it, stage it, copy it, leave it readable by a shader.
#define UPLOAD(res, data, bytes, what)                                              \
    do {                                                                            \
        if (!make_default(device, (bytes), res, error, what)) return false;          \
        ComPtr<ID3D12Resource> stage;                                               \
        D3D12_HEAP_PROPERTIES uhp = {};                                             \
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;                                          \
        D3D12_RESOURCE_DESC urd = {};                                               \
        urd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;                            \
        urd.Width = (bytes) ? (bytes) : 16;                                         \
        urd.Height = 1;                                                             \
        urd.DepthOrArraySize = 1;                                                   \
        urd.MipLevels = 1;                                                          \
        urd.Format = DXGI_FORMAT_UNKNOWN;                                           \
        urd.SampleDesc.Count = 1;                                                   \
        urd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;                                \
        if (FAILED(device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd,\
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,\
                                                   nullptr, IID_PPV_ARGS(&stage)))) {\
            error = std::string("staging for ") + what;                             \
            return false;                                                           \
        }                                                                           \
        void* mapped = nullptr;                                                     \
        D3D12_RANGE none = {0, 0};                                                  \
        stage->Map(0, &none, &mapped);                                              \
        if ((bytes) > 0) std::memcpy(mapped, (data), (bytes));                      \
        stage->Unmap(0, nullptr);                                                   \
        cmd->CopyBufferRegion(res.Get(), 0, stage.Get(), 0, (bytes) ? (bytes) : 16); \
        D3D12_RESOURCE_BARRIER b = {};                                              \
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;                            \
        b.Transition.pResource = res.Get();                                         \
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;         \
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;                  \
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;            \
        cmd->ResourceBarrier(1, &b);                                                \
        staging_.push_back(stage);                                                  \
    } while (0)

// Floats per biome in the archetype buffer: 3 float4 of shape, 2 of
// material. real_field.hlsl's kArchRows must agree.
static const size_t kArchFloats = 24;

bool WorldGpu::diag_no_overhang = false;
uint64_t WorldGpu::world_seed = 0;

bool WorldGpu::build(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                     uint64_t seed, std::string& error) {
    // Throws on a malformed table, naming every violated invariant at once.
    try {
        table_ = worldgen::load_archetypes("worldgen/data/archetypes.json");
    } catch (const std::exception& e) {
        error = std::string("archetypes.json: ") + e.what();
        return false;
    }
    if (table_.size() == 0) {
        error = "could not read worldgen/data/archetypes.json "
                "(is the working directory the repository root?)";
        return false;
    }
    // M2 made the version a compile-time constant in worldgen's own header,
    // so there is nothing to read and nothing that can fail. Before that this
    // parsed golden.json at startup, which put a JSON parse and a file open on
    // the path between the engine and its own world.
    {
        uint64_t h = 1469598103934665603ull;
        const uint64_t parts[2] = {
            static_cast<uint64_t>(worldgen::kGeneratorVersion), seed};
        for (int i = 0; i < 2; ++i) {
            h ^= parts[i];
            h *= 1099511628211ull;
        }
        field_id_ = static_cast<uint32_t>(h ^ (h >> 32));
    }

    // PROTOTYPE: BIOMES_L0="key=value,..." sets the layer 0 prototype's
    // WorldParams (docs/worldgen-design.md, "Layer 0, proposed") so the engine
    // can walk a prototype world and --probe-field can check its GPU parity.
    // Unset, the world is the shipped one.
    if (const char* l0env = std::getenv("BIOMES_L0")) {
        worldgen::apply_param_spec(params_, l0env);
    }

    worldgen::AssignParams ap;
    world_ = worldgen::build_world(seed, params_, table_, ap);
    caves_ = worldgen::caves_enabled(table_);
    if (diag_no_overhang) {
        for (size_t i = 0; i < table_.size(); ++i) table_.biomes[i].overhang_strength = 0.0f;
    }

    const int ncells = static_cast<int>(world_.pts.size());
    const int nbiomes = static_cast<int>(table_.size());

    // --- cells: position and biome, one float4 each -----------------------
    std::vector<float> cells(static_cast<size_t>(ncells) * 4, 0.0f);
    for (int i = 0; i < ncells; ++i) {
        cells[size_t(i) * 4 + 0] = world_.pts[i].first;
        cells[size_t(i) * 4 + 1] = world_.pts[i].second;
        cells[size_t(i) * 4 + 2] = static_cast<float>(world_.biome[i]);
    }
    UPLOAD(cells_, cells.data(), cells.size() * 4, "cell buffer");

    // --- the bucket grid, flattened ---------------------------------------
    // A vector of vectors cannot cross to the GPU, so the same structure
    // becomes a prefix-sum offset array and one flat item array -- the shape
    // every variable-length index takes here.
    {
        const int B = world_.lookup.B;
        std::vector<unsigned> offsets(static_cast<size_t>(B) * B + 1, 0u);
        std::vector<unsigned> items;
        for (size_t i = 0; i < static_cast<size_t>(B) * B; ++i) {
            offsets[i + 1] = offsets[i] +
                static_cast<unsigned>(world_.lookup.buckets[i].size());
            for (size_t j = 0; j < world_.lookup.buckets[i].size(); ++j) {
                items.push_back(static_cast<unsigned>(world_.lookup.buckets[i][j]));
            }
        }
        UPLOAD(cell_offsets_, offsets.data(), offsets.size() * 4, "cell offsets");
        UPLOAD(cell_items_, items.data(), items.size() * 4, "cell items");
    }

    // --- archetypes: the shaping half, three float4 each, then the surface
    // material (ADR-004 L1), two more: (ground rgb linear, roughness), (rock
    // rgb linear, roughness). One extra row set after the last biome holds
    // the seabed, which is what an ocean cell's share of the blend becomes.
    // The materials are presentation and come from archetypes.json's
    // "materials" block, which worldgen never reads.
    {
        std::vector<float> a(static_cast<size_t>(nbiomes + 1) * kArchFloats, 0.0f);
        auto lin = [](double c) {
            const float x = c < 0.0 ? 0.0f : float(c);
            return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
        };
        auto put_material = [&](float* p, const jsonlite::Value& m) {
            const jsonlite::Array& g = m.array("ground");
            const jsonlite::Array& r = m.array("rock");
            for (int k = 0; k < 3; ++k) { p[12 + k] = lin(g[k].num); p[16 + k] = lin(r[k].num); }
            p[15] = float(m.number("ground_roughness"));
            p[19] = float(m.number("rock_roughness"));
        };
        try {
            const jsonlite::Value root = jsonlite::parse_file("worldgen/data/archetypes.json");
            const jsonlite::Array& mats = root.array("materials");
            for (int i = 0; i < nbiomes; ++i) {
                const uint32_t id = table_[i].material_id;
                for (size_t m = 0; m < mats.size(); ++m)
                    if (uint32_t(mats[m].number("id")) == id) put_material(&a[size_t(i) * kArchFloats], mats[m]);
            }
            put_material(&a[size_t(nbiomes) * kArchFloats], root.at("seabed"));
            // The seabed's row 5: how far over sea level its share of the
            // blend survives (surface_nets.hlsl terrain_material). Without
            // it an island smaller than the blend width is sand all over,
            // whatever its height -- see the comment there.
            {
                float* p = &a[size_t(nbiomes) * kArchFloats];
                p[20] = 1.0f; p[21] = 10.0f;
                const jsonlite::Value& sb = root.at("seabed");
                if (sb.has("above_sea")) {
                    const jsonlite::Array& h = sb.array("above_sea");
                    for (size_t k = 0; k < 2 && k < h.size(); ++k) p[20 + k] = float(h[k].num);
                }
            }
        } catch (const std::exception& e) {
            error = std::string("archetypes.json materials: ") + e.what();
            return false;
        }
        for (int i = 0; i < nbiomes; ++i) {
            float* p = &a[size_t(i) * kArchFloats];
            p[0] = table_[i].base_elevation;
            p[1] = table_[i].amplitude;
            p[2] = static_cast<float>(table_[i].octaves);
            p[3] = table_[i].lacunarity;
            p[4] = table_[i].gain;
            p[5] = table_[i].ridge_blend;
            p[6] = table_[i].flatness;
            p[7] = table_[i].overhang_strength;
            p[8] = table_[i].cave_density;
            p[9] = table_[i].cave_depth_min;
            p[10] = table_[i].cave_depth_max;
            p[11] = table_[i].detail_amp;
            // Row 5: shape that did not fit in rows 0-2 (rows 3-4 are the
            // surface material, which the mesher reads at a fixed offset).
            p[20] = table_[i].detail_fine_amp;
        }
        UPLOAD(archetypes_, a.data(), a.size() * 4, "archetype buffer");
    }

    // --- rivers, flat sites, and their bins ------------------------------
    // Layer 5b's flat sites ride in the river buffers, after the segments, as
    // entries with a NEGATIVE width: (x, z, radius, shoulder) and (-1, level).
    // The mesher's root signature has 60 of its 64 DWORDs spent, so three
    // more root UAVs would not fit, and the sites are binned on the river
    // grid's geometry anyway. real_nearest_river skips them; real_terrain_
    // height applies the ones in the point's own bin, as world.cpp does.
    {
        const size_t nr = world_.rivers.size();
        const size_t nf = world_.flats.size();
        std::vector<float> seg((nr + nf) * 8, 0.0f);
        for (size_t i = 0; i < nr; ++i) {
            float* p = &seg[i * 8];
            p[0] = world_.rivers[i].ax; p[1] = world_.rivers[i].az;
            p[2] = world_.rivers[i].bx; p[3] = world_.rivers[i].bz;
            p[4] = world_.rivers[i].width; p[5] = world_.rivers[i].depth;
        }
        for (size_t i = 0; i < nf; ++i) {
            float* p = &seg[(nr + i) * 8];
            p[0] = world_.flats[i].x; p[1] = world_.flats[i].z;
            p[2] = world_.flats[i].radius; p[3] = world_.flats[i].shoulder;
            p[4] = -1.0f; p[5] = world_.flats[i].level;
        }
        if (seg.empty()) seg.assign(8, 0.0f);
        UPLOAD(rivers_, seg.data(), seg.size() * 4, "river buffer");

        const int N = world_.rgrid_n;
        std::vector<unsigned> offsets(static_cast<size_t>(N) * N + 1, 0u);
        std::vector<unsigned> items;
        const bool have_fgrid = world_.fgrid.size() == static_cast<size_t>(N) * N;
        for (size_t i = 0; i < static_cast<size_t>(N) * N; ++i) {
            size_t count = world_.rgrid[i].size();
            for (size_t j = 0; j < world_.rgrid[i].size(); ++j) {
                items.push_back(static_cast<unsigned>(world_.rgrid[i][j]));
            }
            if (have_fgrid) {
                for (size_t j = 0; j < world_.fgrid[i].size(); ++j) {
                    items.push_back(static_cast<unsigned>(nr + static_cast<size_t>(world_.fgrid[i][j])));
                }
                count += world_.fgrid[i].size();
            }
            offsets[i + 1] = offsets[i] + static_cast<unsigned>(count);
        }
        if (items.empty()) items.push_back(0u);
        UPLOAD(river_offsets_, offsets.data(), offsets.size() * 4, "river offsets");
        UPLOAD(river_items_, items.data(), items.size() * 4, "river items");
    }

    // --- entrance shafts and their bins ------------------------------------
    {
        std::vector<float> seg(world_.entrances.size() * 8, 0.0f);
        for (size_t i = 0; i < world_.entrances.size(); ++i) {
            float* p = &seg[i * 8];
            p[0] = world_.entrances[i].ax; p[1] = world_.entrances[i].ay;
            p[2] = world_.entrances[i].az; p[3] = world_.entrances[i].ra;
            p[4] = world_.entrances[i].bx; p[5] = world_.entrances[i].by;
            p[6] = world_.entrances[i].bz; p[7] = world_.entrances[i].rb;
        }
        UPLOAD(entrances_, seg.data(), seg.size() * 4, "entrance buffer");

        const int N = world_.egrid_n;
        std::vector<unsigned> offsets(static_cast<size_t>(N) * N + 1, 0u);
        std::vector<unsigned> items;
        for (size_t i = 0; i < static_cast<size_t>(N) * N; ++i) {
            offsets[i + 1] = offsets[i] + static_cast<unsigned>(world_.egrid[i].size());
            for (size_t j = 0; j < world_.egrid[i].size(); ++j) {
                items.push_back(static_cast<unsigned>(world_.egrid[i][j]));
            }
        }
        UPLOAD(entrance_offsets_, offsets.data(), offsets.size() * 4, "entrance offsets");
        UPLOAD(entrance_items_, items.data(), items.size() * 4, "entrance items");
    }

    // --- the scalars -------------------------------------------------------
    std::memset(&constants_, 0, sizeof(constants_));
    constants_.radius = params_.radius;
    constants_.continent_freq = params_.continent_freq;
    constants_.continent_octaves = params_.continent_octaves;
    constants_.centre_floor = world_.l0.centre_floor;
    constants_.edge_drop = params_.edge_drop;
    constants_.sea_level = params_.sea_level;
    constants_.land_offset = world_.l0.land_offset;   // the seed's: the knee may have moved it
    constants_.danger_w = params_.danger_w;
    constants_.danger_freq = params_.danger_freq;
    constants_.danger_octaves = params_.danger_octaves;
    constants_.warp_amp = params_.warp_amp;
    constants_.warp_freq = params_.warp_freq;
    constants_.continent_scale = params_.continent_scale;
    constants_.biome_freq = params_.biome_freq;
    constants_.blend_width = params_.blend_width;
    constants_.overhang_freq = params_.overhang_freq;
    constants_.overhang_scale = params_.overhang_scale;
    constants_.cave_freq = params_.cave_freq;
    constants_.cave_carve = params_.cave_carve;
    constants_.entrance_falloff = params_.entrance_falloff;
    constants_.s_cont = world_.s_cont;
    constants_.s_dang = world_.s_dang;
    constants_.s_wx = world_.s_wx;
    constants_.s_wz = world_.s_wz;
    constants_.s_height = world_.s_height;
    constants_.s_overhang = world_.s_overhang;
    constants_.s_cave = world_.s_cave;
    constants_.s_entrance = world_.s_entrance;
    constants_.cave_wall_amp = world_.cave_wall_amp;
    constants_.cave_wall_freq = world_.cave_wall_freq;
    constants_.cell_buckets = world_.lookup.B;
    constants_.cell_bucket_size = world_.lookup.bucket;
    constants_.river_grid = world_.rgrid_n;
    constants_.river_bin_size = world_.rgrid_cell;
    constants_.entrance_grid = world_.egrid_n;
    constants_.entrance_bin_size = world_.egrid_cell;
    constants_.cell_count = ncells;
    constants_.biome_count = nbiomes;
    constants_.detail_period = table_.detail.period;
    constants_.detail_octaves = table_.detail.octaves;
    constants_.detail_gain = table_.detail.gain;
    constants_.s_detail = world_.s_detail;
    constants_.detail_fine_period = table_.detail.fine_period;
    constants_.detail_fine_octaves = table_.detail.fine_octaves;
    constants_.s_detail_fine = world_.s_detail_fine;
    {
        const ::Layer0Params& l0 = world_.l0;
        constants_.fine_freq = l0.fine_freq;
        constants_.fine_octaves = l0.fine_octaves;
        constants_.fine_weight = l0.fine_weight;
        constants_.fine_regime = l0.fine_regime;
        constants_.regime_freq = l0.regime_freq;
        constants_.regime_octaves = l0.regime_octaves;
        constants_.regime_mean = l0.regime_mean;
        constants_.regime_amp = l0.regime_amp;
        constants_.regime_on = l0.regime_on;
        constants_.shelf_depth = l0.shelf_depth;
        constants_.shelf_width = l0.shelf_width;
        constants_.shelf_regime = l0.shelf_regime;
        constants_.s_fine = l0.seed_fine;
        constants_.s_regime = l0.seed_regime;
    }

    // A constant buffer's size must be a multiple of 256 bytes.
    const UINT cb_bytes = (sizeof(WorldConstants) + 255u) & ~255u;
    std::vector<unsigned char> cb(cb_bytes, 0);
    std::memcpy(cb.data(), &constants_, sizeof(constants_));
    {
        if (!make_default(device, cb_bytes, world_cb_, error, "world constants")) return false;
        ComPtr<ID3D12Resource> stage;
        D3D12_HEAP_PROPERTIES uhp = {};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC urd = {};
        urd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        urd.Width = cb_bytes;
        urd.Height = 1;
        urd.DepthOrArraySize = 1;
        urd.MipLevels = 1;
        urd.Format = DXGI_FORMAT_UNKNOWN;
        urd.SampleDesc.Count = 1;
        urd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&stage)))) {
            error = "staging for world constants";
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE none = {0, 0};
        stage->Map(0, &none, &mapped);
        std::memcpy(mapped, cb.data(), cb_bytes);
        stage->Unmap(0, nullptr);
        cmd->CopyBufferRegion(world_cb_.Get(), 0, stage.Get(), 0, cb_bytes);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = world_cb_.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        cmd->ResourceBarrier(1, &b);
        staging_.push_back(stage);
    }
    return true;
}

void WorldGpu::release_staging() { staging_.clear(); }

void WorldGpu::column_band(float x0, float z0, float x1, float z1,
                           float& lo, float& hi, float* core_lo, float* core_hi) const {
    // Columns tile at whole metres (chunk sizes are 16 * 2^L), so the corner
    // and the span identify one exactly.
    const unsigned long long key =
        (static_cast<unsigned long long>(static_cast<unsigned>(int(std::floor(x0))) & 0xFFFFFu)) |
        (static_cast<unsigned long long>(static_cast<unsigned>(int(std::floor(z0))) & 0xFFFFFu) << 20) |
        (static_cast<unsigned long long>(static_cast<unsigned>(int(x1 - x0)) & 0xFFFFFu) << 40);
    float mn = 1.0e9f, mx = -1.0e9f, cave = 3.4e38f;
    std::unordered_map<unsigned long long, ColumnSpan>::const_iterator hit = column_cache_.find(key);
    if (hit != column_cache_.end()) {
        mn = hit->second.mn;
        mx = hit->second.mx;
        cave = hit->second.cave;
    } else {
        // The FULL height, not layer 0 alone. Sampling only the continent and
        // then padding by the tallest biome amplitude is what made this band
        // 610 m tall over a column whose surface moves 1.6 m -- 388 times what
        // it needed, and 37 chunks of air per column at LOD 0 against a pool
        // of 768. Measured by --band-report, which is also where the margin
        // below comes from.
        for (int j = 0; j <= 4; ++j) {
            for (int i = 0; i <= 4; ++i) {
                const float x = x0 + (x1 - x0) * float(i) * 0.25f;
                const float z = z0 + (z1 - z0) * float(j) * 0.25f;
                const float h = height(x, z);
                if (h < mn) mn = h;
                if (h > mx) mx = h;
            }
        }
        // Bounded: a long flight visits columns without end. 200k is ~5 MB.
        if (column_cache_.size() > 200000) column_cache_.clear();
        // Placed caves pad only the columns that hold one (2 m for the
        // feathering and the mesher's ghost cells), where the old global
        // 64 m padded every column in the world.
        cave = worldgen::cave_bottom_in_box(world_, x0, z0, x1, z1, 2.0f);
        ColumnSpan c;
        c.mn = mn; c.mx = mx; c.cave = cave;
        column_cache_[key] = c;
    }

    // What the samples can miss between themselves, which grows with the
    // column: a coarse column is sampled every 128 m and the terrain moves in
    // between. Proportional to the relief the samples DO see, because that is
    // the only local evidence of how rough this ground is, plus a floor for
    // the columns that look flat at this spacing. --band-report checks the
    // rule against a dense grid at every level and prints what it misses.
    const float margin = 4.0f + 0.75f * (mx - mn);

    // Below: enough for the overhang displacement and, when the world has
    // caves, the shallow ones an entrance reaches. The full cave band runs to
    // cave_depth_max, several hundred metres, and covering it needs a real
    // acceleration structure rather than a wider guess -- noted here because
    // it is what caves will cost when they come back. With caves off
    // (generator_version 11) the 64 m was solid rock: chunks meshed to
    // nothing, every column, at every LOD.
    lo = mn - margin - params_.overhang_scale - (caves_ ? 64.0f : 0.0f);
    if (cave < lo) lo = cave;
    hi = mx + margin + params_.overhang_scale;
    if (core_lo) *core_lo = mn - margin;
    if (core_hi) *core_hi = mx + margin;
}

void WorldGpu::band(float& lo, float& hi) const {
    // Conservative, from the pieces that set the height: the continent mask
    // scaled, plus the tallest biome profile, plus the overhang displacement.
    // Loose on purpose -- a band that is too tight hides terrain, while one
    // that is too wide only costs empty chunks.
    float amp = 0.0f;
    for (size_t i = 0; i < table_.size(); ++i) {
        const float a = table_[i].base_elevation + table_[i].amplitude;
        if (a > amp) amp = a;
        const float b = -table_[i].base_elevation - table_[i].amplitude;
        if (b > amp) amp = b;
    }
    // layer0_height runs roughly -edge_drop .. +1 before scaling.
    const float top = params_.continent_scale * 1.2f + amp + params_.overhang_scale;
    const float bottom = -params_.continent_scale * (params_.edge_drop + 0.6f) - amp -
                         params_.overhang_scale;
    lo = bottom;
    hi = top;
}

}  // namespace engine
