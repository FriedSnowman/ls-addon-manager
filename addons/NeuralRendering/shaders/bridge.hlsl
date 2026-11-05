// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w,h; Destination.GetDimensions(w,h);
    if (id.x < w && id.y < h) Destination[id.xy] = Source.Load(int3(id.xy,0));
}
