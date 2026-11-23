// SPDX-License-Identifier: GPL-3.0-only
// In place on the composed image: where the HUD confidence is high, put the captured pixel back so
// the interface keeps its look instead of the model's. The mask is widened by one pixel to cover
// anti-aliased glyph edges, which change with the background and never build confidence themselves.
Texture2D<float> Confidence : register(t0);
Texture2D<float4> Original : register(t1);
RWTexture2D<float4> Target : register(u0);
cbuffer HudBlend : register(b0) {
    float Strength; // 0..1, how much of the original a fully detected pixel gets back
    float ShowMask; // nonzero: tint the detected area instead of restoring it
    float Low, High; // confidence range mapped to 0..1
    uint2 Size;
    uint2 Unused;
};
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size))
        return;
    const int2 p = int2(id.xy);
    float c = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            c = max(c, Confidence.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(Size) - 1), 0)));
    const float mask = smoothstep(Low, High, c);
    if (mask <= 0)
        return;
    const float4 t = Target[p];
    if (ShowMask != 0) {
        Target[p] = float4(lerp(t.rgb, float3(1, 0, 1), .6 * mask), t.a);
        return;
    }
    const float m = mask * Strength;
    if (m <= 0)
        return;
    const float3 o = Original.Load(int3(p, 0)).rgb;
    Target[p] = float4(m >= 1 ? o : lerp(t.rgb, o, m), t.a);
}
