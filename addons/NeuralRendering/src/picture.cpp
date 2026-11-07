// SPDX-License-Identifier: GPL-3.0-only
#include "picture.h"
#include "picture_shader.h"
#include <cstring>

namespace neural {
bool Picture::Apply(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *source,
                    ID3D11UnorderedAccessView *target, unsigned width, unsigned height, Encoding encoding,
                    float whiteNits, const PictureSettings &requested) {
    if (!requested.enabled)
        return false;
    const auto s = requested.Bounded();
    if (!s.Active())
        return false;
    struct Constants {
        float gain[3], referenceWhite;
        float contrast, midtones, shadows, highlights;
        float saturation, vibrance, sharpen;
        uint32_t hdr;
    } values{};
    static_assert(sizeof(Constants) == 48);
    const float balance[] = {std::exp2(.35f * s.temperature + .15f * s.tint), std::exp2(-.3f * s.tint),
                             std::exp2(-.35f * s.temperature + .15f * s.tint)};
    const float normalizer = .2126f * balance[0] + .7152f * balance[1] + .0722f * balance[2];
    for (unsigned c = 0; c < 3; ++c)
        values.gain[c] = std::exp2(s.exposure) * balance[c] / normalizer;
    values.hdr = encoding == Encoding::ScRgb;
    values.referenceWhite =
        values.hdr && std::isfinite(whiteNits) ? std::clamp(whiteNits, 80.f, 1000.f) / 80.f : 1.f;
    values.contrast = s.contrast;
    values.midtones = s.midtones;
    values.shadows = s.shadows;
    values.highlights = s.highlights;
    values.saturation = s.saturation;
    values.vibrance = s.vibrance;
    values.sharpen = s.sharpen;
    if (!shader_) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = sizeof(Constants);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&d, nullptr, &constants_), "Create picture constants");
        Check(device->CreateComputeShader(g_picture_shader, sizeof(g_picture_shader), nullptr, &shader_),
              "Create picture shader");
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Update picture controls");
    memcpy(mapped.pData, &values, sizeof(values));
    ctx->Unmap(constants_.Get(), 0);
    // The pipeline guards shaders/views. Only the active path touches a constant buffer.
    ComPtr<ID3D11Buffer> previous;
    ctx->CSGetConstantBuffers(0, 1, &previous);
    auto *cb = constants_.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShader(shader_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &source);
    ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView *none = nullptr;
    ID3D11UnorderedAccessView *empty = nullptr;
    ctx->CSSetShaderResources(0, 1, &none);
    ctx->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
    cb = previous.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    return true;
}
} // namespace neural
