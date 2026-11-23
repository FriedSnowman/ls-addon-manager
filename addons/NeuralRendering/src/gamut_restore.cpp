// SPDX-License-Identifier: GPL-3.0-only
#include "gamut_restore.h"
#include "gamut_restore_shader.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace neural {
void GamutRestore::Apply(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *original,
                         ID3D11UnorderedAccessView *target, unsigned width, unsigned height, float maxRatio) {
    if (!shader_) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 16;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&d, nullptr, &constants_), "Create gamut restore constants");
        Check(device->CreateComputeShader(g_gamut_restore_shader, sizeof(g_gamut_restore_shader), nullptr, &shader_),
              "Create gamut restore shader");
    }
    const float values[4] = {std::isfinite(maxRatio) ? std::max(maxRatio, 1.f) : 1.f, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Update gamut restore constants");
    memcpy(mapped.pData, values, sizeof(values));
    ctx->Unmap(constants_.Get(), 0);
    ComPtr<ID3D11Buffer> previous;
    ctx->CSGetConstantBuffers(0, 1, &previous);
    auto *cb = constants_.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShader(shader_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &original);
    ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    // Unbound before anything reads the target: D3D11 drops an SRV bound while its resource is a UAV.
    ID3D11ShaderResourceView *none = nullptr;
    ID3D11UnorderedAccessView *empty = nullptr;
    ctx->CSSetShaderResources(0, 1, &none);
    ctx->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
    cb = previous.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
}
} // namespace neural
