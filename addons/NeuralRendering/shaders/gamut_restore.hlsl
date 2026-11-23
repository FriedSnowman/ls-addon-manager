// SPDX-License-Identifier: GPL-3.0-only
// scRGB only, in place on the composed image. The pinned composition clamps every negative channel
// of the frame to zero on the way in and out, which clips colours outside BT.709 one channel at a
// time -- a hue shift -- even where the model changed nothing. This adds back the part of the
// captured pixel that was clamped away, scaled by how much the model changed the pixel's light, so a
// pixel the model left alone comes back exactly as captured.
Texture2D<float4> Original : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Restore : register(b0) { float MaxRatio; float3 Unused; };
static const float3 Luma = float3(.2126, .7152, .0722);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h; Target.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float3 clipped = min(Original.Load(int3(id.xy, 0)).rgb, 0); // what the composition removed
    if (all(clipped == 0)) return;                                // in gamut: untouched
    float3 kept = max(Original.Load(int3(id.xy, 0)).rgb, 0);      // what the composition worked from
    float4 result = Target[id.xy];
    // Untouched by the composition: the capture exactly (GPU division need not give x / x == 1).
    if (all(result.rgb == kept)) {
        Target[id.xy] = float4(kept + clipped, result.a);
        return;
    }
    // The model's change in light, with a floor so a near-black pixel does not take a huge ratio,
    // bounded by the same guard the composition uses. Exactly 1 for an unchanged pixel.
    const float floorY = 1.0 / 1024.0;
    float ratio = (dot(max(result.rgb, 0), Luma) + floorY) / (dot(kept, Luma) + floorY);
    Target[id.xy] = float4(result.rgb + clipped * clamp(ratio, 1.0 / MaxRatio, MaxRatio), result.a);
}
