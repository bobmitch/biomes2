// ---------------------------------------------------------------------------
// RmlUi's render interface on the engine's D3D12 device.
//
// RmlUi ships a DX12 backend, but it is 10,000 lines that own a device and a
// swapchain. The engine already has both, and a menu needs very little: one
// pipeline (pixels, premultiplied colour, one texture), scissor, transforms.
// Clip masks, layers, filters and shaders are left at RmlUi's defaults, which
// draw nothing for them -- RCSS that relies on those (filter, mask-image,
// box-shadow, gradients) will not show.
//
// Drawn last, straight onto the back buffer, through its own descriptor heap.
// ---------------------------------------------------------------------------
#ifndef ENGINE_UI_RENDER_D3D12_H
#define ENGINE_UI_RENDER_D3D12_H

#include <RmlUi/Core/RenderInterface.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace ui {

using Microsoft::WRL::ComPtr;

class RenderD3D12 : public Rml::RenderInterface {
public:
    bool init(ID3D12Device* device, DXGI_FORMAT target_format, std::string& error);
    // The caller has waited for the GPU.
    void shutdown();

    // Brackets one frame's Context::Render().
    void begin_frame(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height);
    void end_frame();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Geometry {
        ComPtr<ID3D12Resource> buf;    // upload heap: vertices, then indices
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        UINT index_count = 0;
    };
    struct Texture {
        ComPtr<ID3D12Resource> tex;
        ComPtr<ID3D12Resource> upload;   // until the copy has been recorded
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT slot = 0;                    // in heap_
        bool uploaded = false;
    };

    Texture* make_texture(const uint8_t* rgba, int w, int h);
    void record_upload(Texture* t);
    void retire(ComPtr<ID3D12Resource> r);
    void apply_scissor();

    ID3D12Device* device_ = nullptr;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT heap_stride_ = 0;
    std::vector<UINT> free_slots_;
    Texture* white_ = nullptr;
    std::vector<Texture*> pending_uploads_;

    // Resources released while a frame that used them may still be in
    // flight. Freed this many frames later.
    struct Retired { uint64_t frame; ComPtr<ID3D12Resource> r; };
    std::vector<Retired> retired_;
    uint64_t frame_ = 0;

    ID3D12GraphicsCommandList* cmd_ = nullptr;
    UINT width_ = 1, height_ = 1;
    bool scissor_on_ = false;
    Rml::Rectanglei scissor_;
    float transform_[16];
};

}  // namespace ui
}  // namespace engine

#endif
