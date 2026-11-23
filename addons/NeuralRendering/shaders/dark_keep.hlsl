// SPDX-License-Identifier: GPL-3.0-only
// Dark area protection, in place on the composed image. On dark, grainy captures (night, unlit caves)
// the model smooths the grain into an oily, painted look, on every frame by itself: resetting its
// history every frame does not change it, and neither does cleaner motion. Where the capture is dark,
// this blends the result back toward the captured pixel.
//
// Darkness is the capture's 3x3 average luma, display-referred and sRGB-encoded (scRGB relative to the
// reference white), so a lone bright speck does not punch a hole and the threshold means the same in SDR
// and HDR. The blend is Strength below half the Threshold, fading to nothing at the Threshold.
Texture2D<float4> Original : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer DarkKeep : register(b0) {
    float Strength;  // 0..1
    float Threshold; // sRGB-encoded luma at which protection has faded out
    float ScRgb;     // nonzero: the capture is linear scRGB (1 = 80 nits)
    float WhiteNits; // scRGB reference white
};
static const float3 Luma = float3(.2126, .7152, .0722);
float Encode(float y) { y = saturate(y); return y <= .0031308 ? y * 12.92 : 1.055 * pow(y, 1.0 / 2.4) - .055; }
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h; Target.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy), last = int2(w, h) - 1;
    float y = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            y += dot(max(Original.Load(int3(clamp(p + int2(i, j), 0, last), 0)).rgb, 0), Luma);
    y *= 1.0 / 9.0;
    if (ScRgb != 0) y = Encode(y * 80.0 / max(WhiteNits, 1.0));
    const float m = Strength * (1.0 - smoothstep(Threshold * .5, Threshold, y));
    if (m <= 0) return;
    const float4 t = Target[p];
    const float3 o = Original.Load(int3(p, 0)).rgb;
    Target[p] = float4(m >= 1 ? o : lerp(t.rgb, o, m), t.a);
}
