// Stable identity of a D3D11 compute shader created while the manager is attached.
// Metadata is attached to the COM object, so pointer reuse cannot reuse an old identity.
#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
namespace eam {
inline constexpr GUID kShaderSha256 = {0x3ba4dd2a,0xbfd5,0x47b2,{0x9d,0x2f,0x8a,0xd0,0xd5,0xc6,0xee,0x31}};
using ShaderSha256 = std::array<uint8_t,32>;
inline bool ReadShaderSha256(ID3D11ComputeShader* shader, ShaderSha256& digest) {
    UINT size=static_cast<UINT>(digest.size());
    return shader && SUCCEEDED(shader->GetPrivateData(kShaderSha256,&size,digest.data())) && size==digest.size();
}
}
