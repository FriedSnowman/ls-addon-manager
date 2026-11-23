// SPDX-License-Identifier: GPL-3.0-only
#include "dark_keep.h"
#include "dark_keep_shader.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace neural {
void DarkKeep::Apply(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *original, ID3D11UnorderedAccessView *target,
                     unsigned width, unsigned height, float strength, float threshold, bool scRgb, float whiteNits) {
    const float s = std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f;
    if (s <= 0)
        return;
    if (!shader_) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 16;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&d, nullptr, &constants_), "Create dark protection constants");
        Check(device->CreateComputeShader(g_dark_keep_shader, sizeof(g_dark_keep_shader), nullptr, &shader_),
              "Create dark protection shader");
    }
    const float values[4] = {s,
                             std::isfinite(threshold) ? std::clamp(threshold, .01f, 1.f) : .2f,
                             scRgb ? 1.f : 0.f,
                             std::isfinite(whiteNits) ? std::max(whiteNits, 1.f) : 203.f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Update dark protection constants");
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
