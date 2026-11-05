// SPDX-License-Identifier: GPL-3.0-only
// The input is the current original's display-referred proxy, never an enhanced image.
cbuffer Parameters : register(b0) { uint2 size; uint rowPitch; uint reserved; };
Texture2D<float4> original : register(t0);
SamplerState linearClamp : register(s0);
RWByteAddressBuffer packed : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=size))return;
    float3 rgb=original.SampleLevel(linearClamp,(float2(id.xy)+0.5)/float2(size),0).rgb;
    uint3 c=uint3(round(saturate(rgb)*255.0));
    // Explicit BGRA byte order, copied into an NVOFA-supported B8G8R8A8 surface.
    // This avoids relying on unsupported typed BGRA UAV stores.
    packed.Store(id.y*rowPitch+id.x*4,c.b|(c.g<<8)|(c.r<<16)|0xff000000);
}
