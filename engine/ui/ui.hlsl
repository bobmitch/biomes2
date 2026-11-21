// RmlUi's geometry, drawn over the finished frame (engine/ui/ui_render_d3d12.cpp).
//
// Vertices are in pixels, colours premultiplied, and the target is the back
// buffer after the tonemap: display-encoded, so the UI blends in the space
// its authored sRGB colours are already in -- what RmlUi expects.

cbuffer UiConstants : register(b0) {
    float4x4 ui_transform;   // RmlUi's, column-major; identity when unset
    float2   ui_translate;   // pixels
    float2   ui_inv_size;    // 1 / target size
};

Texture2D    ui_tex : register(t0);
SamplerState ui_smp : register(s0);

struct UiIn {
    float2 pos : POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD;
};

struct UiOut {
    float4 pos : SV_Position;
    float4 col : COLOR;
    float2 uv  : TEXCOORD;
};

UiOut vs_ui(UiIn i) {
    UiOut o;
    const float4 p = mul(ui_transform, float4(i.pos + ui_translate, 0.0, 1.0));
    const float2 px = p.xy / p.w;
    o.pos = float4(px.x * 2.0 * ui_inv_size.x - 1.0, 1.0 - px.y * 2.0 * ui_inv_size.y, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

float4 ps_ui(UiOut i) : SV_Target {
    return i.col * ui_tex.Sample(ui_smp, i.uv);
}
