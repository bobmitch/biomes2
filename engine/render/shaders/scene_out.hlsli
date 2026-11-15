// ---------------------------------------------------------------------------
// What every scene shader writes: the sun's direct light and everything else
// (sky, bounce, fog) in two targets, so the post chain can darken each with
// the right thing -- screen-space contact shadows multiply the direct light,
// ambient occlusion the ambient. Multiplying one combined colour by both
// would darken sunlit ground in creases that the sky cannot reach, which is
// the classic AO look of everything sitting in dirt.
//
// Fog belongs to the ambient side: it is light scattered in from the sky, and
// a fogged pixel should not be shadowed by the geometry it hides.
// ---------------------------------------------------------------------------
#ifndef ENGINE_SCENE_OUT_HLSLI
#define ENGINE_SCENE_OUT_HLSLI

static const float3 kSceneSky = float3(0.40f, 0.50f, 0.66f);

struct SceneOut {
    float4 direct  : SV_Target0;
    float4 ambient : SV_Target1;
};

// The terrain's fog: (distance / far plane) squared towards the sky colour.
// `contact` (the direct target's alpha) says how much screen-space contact
// shadows may darken this surface: 1 for ground and bark, little or none for
// leaves and grass, which shadow each other everywhere within the march and
// are already shadowed at their own scale by the cascades. Without it the
// first version turned every crown nearly black.
// `ao` (the ambient target's alpha) does the same for ambient occlusion:
// dense grass occludes itself in depth everywhere and went to a dark carpet
// at full strength.
SceneOut scene_out(float3 direct, float3 ambient, float3 world, float3 cam, float far_plane,
                   float contact = 1.0f, float ao = 1.0f) {
    const float d = length(world - cam);
    const float f = saturate(d / far_plane);
    const float fog = f * f;
    SceneOut o;
    o.direct = float4(direct * (1.0f - fog), contact);
    o.ambient = float4(ambient * (1.0f - fog) + kSceneSky * fog, ao);
    return o;
}

// A debug colour, shown as is: all in the ambient target, so contact shadows
// never touch it (AO still darkens it where AO is on; --no-ao for a pure tint).
SceneOut scene_flat(float3 c) {
    SceneOut o;
    o.direct = float4(0.0f, 0.0f, 0.0f, 1.0f);
    o.ambient = float4(c, 1.0f);
    return o;
}

#endif
