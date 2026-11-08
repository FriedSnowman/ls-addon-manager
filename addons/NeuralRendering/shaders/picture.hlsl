// SPDX-License-Identifier: GPL-3.0-only
// Stateless adjustments to the finished NR image, at capture resolution before LS.
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Picture : register(b0) {
    float3 Gain; float ReferenceWhite;
    float Contrast, Midtones, Shadows, Highlights;
    float Saturation, Vibrance, Sharpen; uint Hdr;
};
static const float3 Luma = float3(.2126, .7152, .0722);
float3 Decode(float3 v) {
    if (Hdr) return v / ReferenceWhite;
    return float3(v.r <= .04045 ? v.r / 12.92 : pow(max((v.r + .055) / 1.055, 0), 2.4),
                  v.g <= .04045 ? v.g / 12.92 : pow(max((v.g + .055) / 1.055, 0), 2.4),
                  v.b <= .04045 ? v.b / 12.92 : pow(max((v.b + .055) / 1.055, 0), 2.4));
}
float3 Encode(float3 v) {
    if (Hdr) return clamp(v * ReferenceWhite, -65504, 65504); // FP16 storage bounds, not SDR white.
    v = saturate(v);
    return float3(v.r <= .0031308 ? 12.92 * v.r : 1.055 * pow(v.r, 1.0 / 2.4) - .055,
                  v.g <= .0031308 ? 12.92 * v.g : 1.055 * pow(v.g, 1.0 / 2.4) - .055,
                  v.b <= .0031308 ? 12.92 * v.b : 1.055 * pow(v.b, 1.0 / 2.4) - .055);
}
float NeighbourLuma(int2 p, int2 size) {
    return dot(Decode(Source.Load(int3(clamp(p, int2(0,0), size - 1), 0)).rgb), Luma);
}
float3 Tone(float3 rgb) {
    if (Contrast == 0 && Midtones == 0 && Shadows == 0 && Highlights == 0) return rgb;
    float y = dot(rgb, Luma);
    // Signed scRGB channels remain signed; do not clamp wide-gamut colours to RGB zero.
    // A nonpositive luminance has no useful log exposure; leave that rare case unchanged.
    if (y <= 1e-6 || (!Hdr && y >= 1 - 1e-6)) return rgb;
    // SDR uses log odds so these curves preserve both black and white. HDR uses log
    // luminance relative to reference white, without imposing an upper white boundary.
    float pivot = Hdr ? log2(.18) : log2(.18 / .82);
    float original = Hdr ? log2(y) : log2(y / (1 - y));
    float t = original - pivot;
    // Each stage is monotonic over its complete slider range. Bounded changes in stops
    // keep HDR highlights finite and avoid a hard contrast clip.
    // Beyond +/-1, continue with a second monotonic stage instead of multiplying
    // the slope until it reverses. The original range retains its exact curve.
    float contrastFirst = clamp(Contrast, -1, 1);
    t += 1.5 * contrastFirst * t / (2 + abs(t));
    [branch] if (abs(Contrast) > 1)
        t += 1.5 * (Contrast - contrastFirst) * t / (2 + abs(t));
    float midtonesFirst = clamp(Midtones, -1, 1);
    t += midtonesFirst * exp2(-.5 * t * t);
    [branch] if (abs(Midtones) > 1)
        t += (Midtones - midtonesFirst) * exp2(-.5 * t * t);
    t += 1.5 * Shadows / (1 + exp2(clamp(t + 1, -80, 80)));
    t += 1.5 * Highlights / (1 + exp2(clamp(2 - t, -80, 80)));
    float output = exp2(clamp(t + pivot, -80, 80));
    if (!Hdr) output /= 1 + output;
    return rgb * (output / y);
}
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h; Destination.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 source = Source.Load(int3(id.xy, 0));
    float3 rgb = Decode(source.rgb);
    [branch] if (Sharpen > 0) {
        float c = dot(rgb, Luma);
        float n = NeighbourLuma(int2(id.xy) + int2(0,-1), int2(w,h));
        float s = NeighbourLuma(int2(id.xy) + int2(0, 1), int2(w,h));
        float e = NeighbourLuma(int2(id.xy) + int2(1, 0), int2(w,h));
        float west = NeighbourLuma(int2(id.xy) + int2(-1,0), int2(w,h));
        float lo = min(c, min(min(n,s), min(e,west)));
        float hi = max(c, max(max(n,s), max(e,west)));
        // Keep the existing 0..1 behavior. Higher settings deliberately allow
        // ringing: up to 75% of the local range at 4, with bounded luminance gain.
        [branch] if (Sharpen > 1) {
            float overshoot = .25 * (Sharpen - 1) * (hi - lo);
            lo = max(0, lo - overshoot);
            hi += overshoot;
        }
        float sharpened = clamp(c + Sharpen * (c - .25 * (n+s+e+west)), lo, hi);
        float gainLimit = Sharpen > 1 ? 2 * Sharpen : 2;
        if (c > 1e-6) rgb *= clamp(sharpened / c, 1 / gainLimit, gainLimit);
    }
    rgb = Tone(rgb * Gain);
    float y = dot(rgb, Luma);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float trough = min(rgb.r, min(rgb.g, rgb.b));
    float chroma = saturate((peak - trough) / max(max(abs(peak), abs(trough)), 1e-6));
    rgb = y + (rgb - y) * (Saturation * (1 + Vibrance * (1 - chroma)));
    if (!Hdr) {
        // Compress out-of-gamut chroma toward the same luminance rather than clipping
        // individual colour channels. Exposure outside the SDR range still clips.
        y = saturate(y);
        float3 delta = rgb - y;
        float scale = 1;
        [unroll] for (uint i = 0; i < 3; ++i) {
            if (delta[i] > 0) scale = min(scale, (1-y) / delta[i]);
            if (delta[i] < 0) scale = min(scale, -y / delta[i]);
        }
        rgb = y + delta * scale;
    }
    Destination[id.xy] = float4(Encode(rgb), source.a);
}
