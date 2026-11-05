// SPDX-License-Identifier: GPL-3.0-only
// Two scalars only: histogram distance and average same-position RGB change.
// A single group compares fixed 32x32 samples of current and previous ORIGINAL proxies.
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
RWByteAddressBuffer Summary : register(u0);
groupshared uint a[48];
groupshared uint b[48];
groupshared uint difference[256];
[numthreads(256,1,1)]
void CSMain(uint i : SV_GroupIndex) {
    if (i < 48) { a[i]=0; b[i]=0; }
    difference[i]=0;
    GroupMemoryBarrierWithGroupSync();
    uint w,h; Current.GetDimensions(w,h);
    for (uint n=i;n<1024;n+=256) {
        uint2 xy=min(uint2(((n%32)+0.5)*w/32,((n/32)+0.5)*h/32),uint2(w-1,h-1));
        float3 c=saturate(Current.Load(int3(xy,0)).rgb);
        float3 p=saturate(Previous.Load(int3(xy,0)).rgb);
        for(uint ch=0;ch<3;ch++) {
            InterlockedAdd(a[ch*16+min(15,(uint)(c[ch]*16))],1);
            InterlockedAdd(b[ch*16+min(15,(uint)(p[ch]*16))],1);
        }
        difference[i]+=(uint)(dot(abs(c-p),float3(1,1,1))*10000);
    }
    GroupMemoryBarrierWithGroupSync();
    if (i==0) {
        uint distance=0,delta=0;
        for(uint k=0;k<48;k++) distance+=abs((int)a[k]-(int)b[k]);
        for(uint t=0;t<256;t++) delta+=difference[t];
        Summary.Store(0,(uint)(distance*(1000000.0/6144.0)));
        Summary.Store(4,(uint)(delta*(100.0/3072.0)));
    }
}
