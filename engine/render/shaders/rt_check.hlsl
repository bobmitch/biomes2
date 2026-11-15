// ---------------------------------------------------------------------------
// --check-rt (ADR-004 L0): the smallest thing that proves hardware ray
// tracing works end to end on this machine -- DXC compiling SM 6.5, a BLAS and
// a TLAS built on the device, and inline RayQuery returning the right hits.
//
// One triangle at z = 5, vertices (-1,-1), (1,-1), (0,1). An 8 x 8 grid of
// rays from z = 0 straight down +z, spanning x and y in [-1.2, 1.2]. The CPU
// knows which of them should hit and where; this reports what the GPU says.
// ---------------------------------------------------------------------------

RaytracingAccelerationStructure g_scene : register(t0);
RWStructuredBuffer<float4>      g_out   : register(u0);

[numthreads(64, 1, 1)]
void cs_rays(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= 64u) return;
    const float x = -1.2f + 2.4f * (float(i % 8u) + 0.5f) / 8.0f;
    const float y = -1.2f + 2.4f * (float(i / 8u) + 0.5f) / 8.0f;
    RayDesc r;
    r.Origin = float3(x, y, 0.0f);
    r.Direction = float3(0.0f, 0.0f, 1.0f);
    r.TMin = 0.0f;
    r.TMax = 100.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_scene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const float2 b = q.CommittedTriangleBarycentrics();
        g_out[i] = float4(1.0f, q.CommittedRayT(), b.x, b.y);
    } else {
        g_out[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
