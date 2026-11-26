#pragma once
// ---------------------------------------------------------------------------
// CC0 surface scans for the terrain (and later bark and placed rocks).
//
// assets/textures/scans.json lists the sets; tools/textures/fetch_scans.py
// downloads and packs them into assets/textures/packed/<id>.bin (not in git).
// Every set is uploaded into one layer of two Texture2DArrays:
//   A (sRGB RGBA8)  albedo, height in alpha
//   B (UNORM RGBA8) tangent normal x, y (0.5 + 0.5n), roughness, occlusion
// and archetypes.json's materials block says which layers each biome's
// ground and rock take, as a table the terrain shader reads per biome index
// (the seabed after the last biome, as in g_arch). A layer of -1 means no
// scan: the shader keeps the old noise detail for that material, so a
// checkout without the packs renders exactly as before.
//
// The scan's albedo is RETINTED to the authored colour: the table carries
// authored / scan mean (both linear), so the biome keeps the colour it was
// tuned to -- and that the GI, the RT scene and the heightfield's top-down
// pass still see -- and the scan brings structure, not a new palette.
//
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace engine {

// One row per biome (and one for the seabed): matches ScanMaterial in
// scan.hlsli, 80 bytes.
struct ScanMaterialGpu {
    float ground_layer, rock_layer, ground_inv_tile, rock_inv_tile;
    float ground_tint[3], ground_rough;   // authored / scan mean; authored roughness
    float rock_tint[3], rock_rough;
    float ground_scan_rough, rock_scan_rough;   // the scans' own mean roughness
    float ground_depth, rock_depth;             // parallax depth, metres (scans.json depth_m)
};
static_assert(sizeof(ScanMaterialGpu) == 64, "ScanMaterialGpu must match scan.hlsli");

// How bare ground looks (archetypes.json `ground_look`, presentation only;
// live in the tuning panel's Terrain group). Against the scans' tiling: a
// 2.5 m scan repeated over a clearing read as a grid from ~10 m out.
struct GroundLook {
    float variation = 1.0f;      // macro variation's strength, 0 off
    float fade[2] = {350.0f, 700.0f};   // the scan fades to its own mean over this, metres
    bool  hex = true;            // the ground scan through a hex lattice (scan.hlsli)
    float far_detail = 1.2f;     // the scan at far_scale x its tile, over the near one, 0 off
    float far_scale = 5.3f;
};

// How rock looks (archetypes.json `rock_look`, presentation only; live in
// the tuning panel's Terrain group). Against the 5 m rock scans repeating
// along a cliff: the heaviest rock candidate hex-tiled in every triplanar
// plane, and cross-faded with distance into the same scan at far_scale x
// its tile.
struct RockLook {   // not "far": a Windows macro
    bool  hex = true;            // rock through the hex lattice (scan.hlsli)
    float hex_turn_deg = 15.0f;  // the most a hex turns either way; 0 offsets only
    float far_share = 0.7f;      // the far layer's share at full distance, 0 off
    float far_scale = 5.0f;      // its tile over the scan's
    float far_fade[2] = {15.0f, 60.0f};   // metres from the camera it fades in over
};

class ScanLibrary {
public:
    // Heap: [67] A, [68] B, [69] the per-biome table (t44..t46 in the
    // terrain's lit table).
    static const UINT kSrvAlbedo = 67, kSrvNra = 68, kSrvTable = 69;

    // `material_ids` is each biome's material_id, in biome order. Never
    // fails for missing packs -- it notes them and leaves every layer at -1.
    // `extra` names further sets to load beside the materials' (bark).
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap,
              const std::vector<uint32_t>& material_ids, const std::vector<std::string>& extra,
              std::string& error);
    // A loaded set's layer, 1 / its tile in metres, and the retint that takes
    // its mean albedo to `authored_srgb`. False (layer -1) if not loaded.
    bool lookup(const std::string& name, const float authored_srgb[3], float& layer, float& inv_tile,
                float tint[3], float& depth_m) const;
    bool ready() const { return layers_ > 0; }
    // Each biome's authored rock albedo, linear, rgb per biome (the seabed
    // last): what the heightfield GI's top-down pass sees of a rock.
    const std::vector<float>& rock_albedo() const { return rock_albedo_; }
    // archetypes.json's ground_look, read at init (defaults when absent).
    const GroundLook& ground_look() const { return look_; }
    const RockLook& rock_look() const { return rock_look_; }
    UINT layers() const { return layers_; }
    // What init found, for startup.txt: sets loaded, missing, MB uploaded.
    const std::string& note() const { return note_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    UINT layers_ = 0;
    std::string note_;
    struct Info { int layer; float mean[3]; float tile; float depth; };
    std::map<std::string, Info> info_;
    std::vector<float> rock_albedo_;
    GroundLook look_;
    RockLook rock_look_;
    ComPtr<ID3D12Resource> albedo_, nra_, table_;
};

}  // namespace engine
