// SPDX-License-Identifier: GPL-3.0-only
// NVOFA's vectors, cleaned where NVOFA itself is unsure, before NR sees them.
//
// In smooth, grainy darkness (night, an unlit cave) NVOFA has nothing to lock on to: its vectors are off
// by a pixel or more and change every frame, a still picture included, and NR drags its history along
// them, an oily, swimming look. NVOFA reports this as a high cost (about 1-5 where it tracks, 200-300
// there; see flow_test). A cell whose cost is above kTrusted is blended, fully by kUntrusted, toward the
// average of its neighbourhood (about 64 input pixels each way, sampled 9x9), each neighbour weighted by
// its own confidence and its distance: confident neighbours (a lit edge, a moving lamp) dominate, and
// where there are none the noise averages out. NVOFA's errors there are correlated over tens of pixels,
// so a smaller neighbourhood barely helped (16 px: p95 3.6 -> 2.8 px; 64 px: -> 2.1 px; 128 px no
// better); a second, outlier-rejecting round did not help either. Confident cells -- nearly all of a lit
// picture -- pass through untouched.
cbuffer Parameters : register(b0) { uint2 size; uint spacing; uint enabled; };
Texture2D<int2> flowGrid : register(t0);
Texture2D<uint> cost : register(t1);
RWTexture2D<float2> filtered : register(u0);
static const float kTrusted = 24.0, kUntrusted = 96.0;
static const int kRadius = 4;   // taps each way; `spacing` cells apart
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=size))return;
    const float2 own=float2(flowGrid.Load(int3(id.xy,0)));
    const float c=float(cost.Load(int3(id.xy,0)));
    if(enabled==0||c<=kTrusted){filtered[id.xy]=own;return;}
    const int2 last=int2(size)-1;
    float2 sum=0;float total=0;
    [loop]for(int j=-kRadius;j<=kRadius;++j)
        [loop]for(int i=-kRadius;i<=kRadius;++i){
            const int2 p=clamp(int2(id.xy)+int2(i,j)*int(spacing),0,last);
            const float near=exp(-float(i*i+j*j)/(0.5*kRadius*kRadius));
            const float w=near/(1.0+float(cost.Load(int3(p,0)))/kTrusted);
            sum+=w*float2(flowGrid.Load(int3(p,0)));total+=w;
        }
    filtered[id.xy]=lerp(own,sum/max(total,1e-6),saturate((c-kTrusted)/(kUntrusted-kTrusted)));
}
