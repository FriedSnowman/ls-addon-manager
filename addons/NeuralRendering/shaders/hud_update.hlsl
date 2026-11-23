// SPDX-License-Identifier: GPL-3.0-only
// HUD detection, one step per capture. Lossless Scaling captures the finished frame, so the game's
// interface is already drawn in and the model has no UI layer to work from. A pixel looks like HUD
// when it stays the same while the picture around it (two rings of samples) changes: text and icons
// sit still while the camera moves. Each pixel keeps a confidence that rises slowly on that evidence,
// falls quickly when the pixel itself changes, and holds when there is no evidence either way (a
// still scene, a repeated frame, the inside of a large panel).
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
RWTexture2D<float> Confidence : register(u0);
cbuffer HudUpdate : register(b0) {
    float Rise;        // added per frame of evidence
    float Fall;        // removed per frame the pixel itself changed
    float StaticLimit; // relative change still counted as unchanged
    float MovingLimit; // relative change counted as the surroundings moving
    int Radius;        // inner ring radius in pixels; the outer ring is twice this
    uint MinMoving;    // of the 16 ring samples
    uint2 Size;
};
static const float2 kDirections[8] = {float2(1, 0),       float2(.7071, .7071),  float2(0, 1),  float2(-.7071, .7071),
                                      float2(-1, 0),      float2(-.7071, -.7071), float2(0, -1), float2(.7071, -.7071)};
// Largest channel change relative to the brighter of the two values, with a floor so noise in the
// dark does not count as motion. Works for sRGB-encoded SDR and linear scRGB alike.
float Change(int2 p) {
    p = clamp(p, int2(0, 0), int2(Size) - 1);
    float3 a = Current.Load(int3(p, 0)).rgb, b = Previous.Load(int3(p, 0)).rgb;
    float3 d = abs(a - b);
    float3 m = max(abs(a), abs(b));
    return max(d.r, max(d.g, d.b)) / (max(m.r, max(m.g, m.b)) + 1.0 / 16.0);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size))
        return;
    const int2 p = int2(id.xy);
    const float c = Confidence[p];
    if (Change(p) > StaticLimit) {
        Confidence[p] = max(c - Fall, 0);
        return;
    }
    uint moving = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        moving += Change(p + int2(round(kDirections[i] * Radius))) > MovingLimit ? 1 : 0;
        moving += Change(p + int2(round(kDirections[i] * 2 * Radius))) > MovingLimit ? 1 : 0;
    }
    if (moving >= MinMoving)
        Confidence[p] = min(c + Rise, 1);
}
