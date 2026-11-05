// SPDX-License-Identifier: GPL-3.0-only
// Captures have no game depth. The constant plane is a compatibility input, not inferred geometry.
cbuffer Parameters : register(b0) { uint2 size; uint2 reserved; };
RWTexture2D<float> depth : register(u0);
RWTexture2D<float2> motion : register(u1);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=size))return;
    depth[id.xy]=1.0;motion[id.xy]=0.0;
}
