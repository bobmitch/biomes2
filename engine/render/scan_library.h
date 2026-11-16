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
// mesh.hlsl, 64 bytes.
struct ScanMaterialGpu {
    float ground_layer, rock_layer, ground_inv_tile, rock_inv_tile;
    float ground_tint[3], ground_rough;   // authored / scan mean; authored roughness
    float rock_tint[3], rock_rough;
    float ground_scan_rough, rock_scan_rough;   // the scans' own mean roughness
    float ground_depth, rock_depth;             // parallax depth, metres (scans.json depth_m)
};
static_assert(sizeof(ScanMaterialGpu) == 64, "ScanMaterialGpu must match mesh.hlsl");

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
    UINT layers() const { return layers_; }
    // What init found, for startup.txt: sets loaded, missing, MB uploaded.
    const std::string& note() const { return note_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    UINT layers_ = 0;
    std::string note_;
    struct Info { int layer; float mean[3]; float tile; float depth; };
    std::map<std::string, Info> info_;
    ComPtr<ID3D12Resource> albedo_, nra_, table_;
};

}  // namespace engine
