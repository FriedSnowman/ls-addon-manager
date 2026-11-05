// SPDX-License-Identifier: GPL-3.0-only
cbuffer Parameters : register(b0) {
    uint2 targetSize; uint2 inputSize;
    uint gridSize; uint reset; uint2 reserved;
};
Texture2D<int2> flowGrid : register(t0);
RWTexture2D<float2> motion : register(u0);
float2 Read(int2 p,int2 maxIndex) {return float2(flowGrid.Load(int3(clamp(p,0,maxIndex),0)));}
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=targetSize))return;
    if(reset!=0){motion[id.xy]=0;return;}
    uint2 dims;flowGrid.GetDimensions(dims.x,dims.y);
    // A partially filled final grid cell must not change the physical grid spacing.
    float2 p=(float2(id.xy)+0.5)*float2(inputSize)/float2(targetSize)/gridSize-0.5;
    int2 base=int2(floor(p));float2 f=frac(p);int2 maxIndex=int2(dims)-1;
    float2 v=lerp(lerp(Read(base,maxIndex),Read(base+int2(1,0),maxIndex),f.x),
                  lerp(Read(base+int2(0,1),maxIndex),Read(base+1,maxIndex),f.x),f.y);
    // Current -> previous, S10.5 / 32, independently scaled to NR working pixels.
    motion[id.xy]=v*(float2(targetSize)/float2(inputSize))/32.0;
}
