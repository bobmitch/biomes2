// ---------------------------------------------------------------------------
// The ray-traced shadow's denoiser (rt_denoise.h): one jittered ray a pixel
// a frame in -- or, on terrain, one every fourth frame -- a soft, stable
// visibility out. SVGF's shape (Schied et al. 2017), for a scalar:
//
//   cs_den_temporal  reprojects last frame's history through TAA's own
//                    clip-to-last-frame matrix and accumulates: a long mean
//                    and second moment (up to kMaxLong samples), and a FAST
//                    mean (kMaxFast) that catches the shadow moving -- a
//                    swaying crown, the clock -- and cuts the long history
//                    short when the two part (anti-lag). Disocclusion: the
//                    distance stored with the history against the distance
//                    the point should have had from last frame's camera.
//                    A pixel the trace skipped this frame (sparse terrain,
//                    rt_shadow.hlsl's -1) carries its history unchanged; one
//                    with no history either is EMPTY, for the a-trous to fill.
//   cs_den_atrous    an edge-aware a-trous wavelet, run four times with
//                    steps 1, 2, 4, 8 (a 61-pixel footprint): weights stop
//                    at depth discontinuities off the local plane and where
//                    the visibility differs by more than its own noise; the
//                    reach shrinks as history builds; empty taps are skipped
//                    and empty pixels filled from their neighbours. The last
//                    pass writes the result into the visibility's x.
// ---------------------------------------------------------------------------

cbuffer DenoiseConstants : register(b0) {
    row_major float4x4 inv_view_proj;   // this frame's jittered, for world positions
    row_major float4x4 clip_to_prev;    // TAA's: this frame's unjittered clip to last frame's
    float3 cam_pos;    uint width;
    float3 prev_cam;   uint height;
    float2 jitter;     float2 prev_jitter;   // NDC
    uint   history_ok; uint frame;  uint sparse; uint pad0;   // frame: the history parity; sparse: read u_raw
};
cbuffer PassConstants : register(b1) {
    uint step;     // a-trous: the tap spacing in pixels
    uint src;      // which of the two ping-pong buffers to read (a-trous) or history to read (temporal)
    uint last;     // a-trous: write the result into the visibility
    uint dbg;      // BIOMES_DEN_DBG: 2 shows each pixel's history length in place of the shadow
};

Texture2D<float>           g_depth : register(t0);   // the scene's depth
RWTexture2D<unorm float2>  u_vis   : register(u0);   // rt_shadow.hlsl's: x raw in (not sparse), denoised out; y the sky, untouched
RWTexture2D<float4>        u_hist[2] : register(u1); // long mean, long second moment, fast mean, samples (0: empty)
RWTexture2D<float>         u_dist[2] : register(u3); // the distance from the camera each history was made at
RWTexture2D<float2>        u_tmp[2]  : register(u5); // a-trous ping-pong: visibility, variance (negative: empty)
RWTexture2D<float>         u_raw     : register(u7); // sparse: rt_shadow.hlsl's trace, -1 where it skipped

static const float kMaxLong = 24.0f, kMaxFast = 3.0f;

float3 world_at(int2 p, float z) {
    const float2 uv = (float2(p) + 0.5f) / float2(width, height);
    const float4 h = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, z, 1.0f), inv_view_proj);
    return h.xyz / h.w;
}

[numthreads(8, 8, 1)]
void cs_den_temporal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const uint dst = 1u - src;
    const float z = g_depth[p];
    const float v = sparse != 0u ? u_raw[p] : u_vis[p].x;
    const bool traced = v >= 0.0f;
    if (z >= 1.0f) {
        u_hist[dst][p] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        u_dist[dst][p] = 0.0f;
        u_tmp[0][p] = float2(1.0f, 0.0f);
        return;
    }
    const float3 P = world_at(p, z);
    const float dist = length(P - cam_pos);
    float4 h = 0.0f;
    float hw = 0.0f;
    if (history_ok != 0u) {
        // Where this point was on last frame's (jittered) pixel grid.
        const float2 ndc = float2((float(p.x) + 0.5f) / float(width) * 2.0f - 1.0f,
                                  1.0f - (float(p.y) + 0.5f) / float(height) * 2.0f) - jitter;
        const float4 q = mul(float4(ndc, z, 1.0f), clip_to_prev);
        const float2 pn = q.xy / q.w + prev_jitter;
        const float2 pp = float2((pn.x * 0.5f + 0.5f) * float(width), (0.5f - pn.y * 0.5f) * float(height)) - 0.5f;
        const float expect = length(P - prev_cam);
        const int2 b = int2(floor(pp));
        const float2 f = pp - float2(b);
        [unroll] for (int k = 0; k < 4; ++k) {
            const int2 o = int2(k & 1, k >> 1);
            const int2 s = b + o;
            if (any(s < 0) || s.x >= int(width) || s.y >= int(height)) continue;
            const float ds = u_dist[src][s];
            // Disoccluded: a different surface was there. Loose enough for
            // a crown, whose leaves sway and whose neighbouring pixels hit
            // leaves metres apart in depth; a trunk against what is behind
            // it is still far outside.
            if (ds <= 0.0f || abs(ds - expect) > 0.03f * expect + 0.3f) continue;
            const float4 hs = u_hist[src][s];
            if (hs.w <= 0.0f) continue;   // an empty history holds nothing
            const float w = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y);
            h += w * hs;
            hw += w;
        }
    }
    float4 o;
    if (hw > 1e-3f) {
        h /= hw;
        if (traced) {
            // Anti-lag: where the fast mean has left the long one, the
            // shadow moved; trust far fewer of the long samples.
            const float part = saturate((abs(h.x - h.z) - 0.08f) / 0.2f);
            const float n = min(lerp(h.w, 2.0f, part), kMaxLong - 1.0f) + 1.0f;
            const float nf = min(h.w, kMaxFast - 1.0f) + 1.0f;
            o.x = lerp(h.x, v, 1.0f / n);
            o.y = lerp(h.y, v * v, 1.0f / n);
            o.z = lerp(h.z, v, 1.0f / nf);
            o.w = n;
        } else {
            o = h;   // not traced this frame: the history stands
        }
    } else {
        o = traced ? float4(v, v * v, v, 1.0f) : float4(0.5f, 0.25f, 0.5f, 0.0f);   // w 0: empty
    }
    u_hist[dst][p] = o;
    u_dist[dst][p] = dist;
    // Variance of the MEAN, not of one sample: the sample's variance over n
    // samples. A converged pixel is then trusted -- the a-trous only
    // averages values within that much of it -- and a fresh one (assume the
    // worst a coin can do) is blurred hard. Negative marks an empty pixel.
    const float var = o.w <= 0.0f ? -1.0f : (o.w >= 4.0f ? max(o.y - o.x * o.x, 0.0f) / o.w : 0.25f);
    u_tmp[0][p] = dbg == 2u ? float2(o.w / kMaxLong, 0.0f) : float2(o.x, var);
}

static const float kAtrous[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};

[numthreads(8, 8, 1)]
void cs_den_atrous(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const uint dst = 1u - src;
    // The distances this frame's temporal pass stored: history buffer `frame & 1` ^ 1.
    const uint dcur = 1u - (frame & 1u);
    const float dp = u_dist[dcur][p];
    const float2 cp = u_tmp[src][p];
    const bool empty = cp.y < 0.0f;
    // The spatial reach shrinks as history builds (ReBLUR's idea): a pass
    // runs only while its step is within 8 / sqrt(samples) pixels -- all
    // four on a fresh pixel, the first alone at 24.
    const float frames = u_hist[dcur][p].w;
    if (dp <= 0.0f || (!empty && float(step) > 8.0f / sqrt(max(frames, 1.0f)))) {
        u_tmp[dst][p] = cp;
        if (last != 0u) u_vis[p] = float2(saturate(cp.x), u_vis[p].y);
        return;
    }
    const int2 mx = int2(int(width) - 1, int(height) - 1);
    // The local plane in screen space: the gentler one-sided slope each way,
    // so an edge beside the pixel does not tilt it.
    const float dl = u_dist[dcur][clamp(p - int2(1, 0), 0, mx)], dr = u_dist[dcur][clamp(p + int2(1, 0), 0, mx)];
    const float du = u_dist[dcur][clamp(p - int2(0, 1), 0, mx)], dd = u_dist[dcur][clamp(p + int2(0, 1), 0, mx)];
    const float gx = abs(dp - dl) < abs(dr - dp) ? dp - dl : dr - dp;
    const float gy = abs(dp - du) < abs(dd - dp) ? dp - du : dd - dp;
    // The visibility's own noise, blurred 3x3 as SVGF does (empty taps out).
    float vb = 0.0f, vw = 0.0f;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float vy = u_tmp[src][clamp(p + int2(i, j), 0, mx)].y;
            const float w = (vy >= 0.0f ? 1.0f : 0.0f) * (i == 0 ? 0.5f : 0.25f) * (j == 0 ? 0.5f : 0.25f);
            vb += w * vy;
            vw += w;
        }
    }
    // 1.5 standard deviations, not SVGF's 4: under a crown the visibility
    // is a leaf-by-leaf pattern that is real, not noise.
    const float sig_v = 1.5f * sqrt(max(vw > 0.0f ? vb / vw : 0.25f, 0.0f)) + 1e-3f;
    float sum = 0.0f, sum_var = 0.0f, wsum = 0.0f;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const int2 off = int2(x, y) * int(step);
            const int2 q = p + off;
            if (any(q < 0) || q.x > mx.x || q.y > mx.y) continue;
            const float dq = u_dist[dcur][q];
            if (dq <= 0.0f) continue;
            const float2 cq = u_tmp[src][q];
            if (cq.y < 0.0f) continue;   // empty
            const float k = kAtrous[abs(x)] * kAtrous[abs(y)];
            // Off the local plane: the prediction is exact on a real
            // surface, so its slack only covers curvature -- 5% of what the
            // slope predicts, and a few millimetres a metre.
            const float pred = dp + gx * float(off.x) + gy * float(off.y);
            const float tol = 0.004f * dp + 0.05f * (abs(gx * float(off.x)) + abs(gy * float(off.y))) + 0.01f;
            const float wz = exp(-abs(dq - pred) / tol);
            // An empty pixel has no value of its own to stop at.
            const float wv = empty ? 1.0f : exp(-abs(cq.x - cp.x) / sig_v);
            const float w = k * wz * wv;
            sum += w * cq.x;
            sum_var += w * w * cq.y;
            wsum += w;
        }
    }
    const float2 o = wsum > 1e-6f ? float2(sum / wsum, sum_var / (wsum * wsum)) : cp;
    u_tmp[dst][p] = o;
    if (last != 0u) u_vis[p] = float2(saturate(o.x), u_vis[p].y);
}
