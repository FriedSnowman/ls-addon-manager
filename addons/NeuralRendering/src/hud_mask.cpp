// SPDX-License-Identifier: GPL-3.0-only
#include "hud_mask.h"
#include "hud_blend_shader.h"
#include "hud_update_shader.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace neural {
namespace {
struct UpdateConstants {
    float rise, fall, staticLimit, movingLimit;
    int32_t radius;
    uint32_t minMoving, width, height;
};
struct BlendConstants {
    float strength, showMask, low, high;
    uint32_t width, height, unused[2];
};
static_assert(sizeof(UpdateConstants) == 32 && sizeof(BlendConstants) == 32);
} // namespace

void HudMask::Allocate(ID3D11Device *device, DXGI_FORMAT format, unsigned width, unsigned height) {
    if (!update_) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 32;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&d, nullptr, &constants_), "Create HUD mask constants");
        Check(device->CreateComputeShader(g_hud_update_shader, sizeof(g_hud_update_shader), nullptr, &update_),
              "Create HUD update shader");
        Check(device->CreateComputeShader(g_hud_blend_shader, sizeof(g_hud_blend_shader), nullptr, &blend_),
              "Create HUD blend shader");
    }
    if (previous_ && format == format_ && width == width_ && height == height_)
        return;
    previous_.Reset();
    previousView_.Reset();
    confidence_.Reset();
    confidenceView_.Reset();
    confidenceUav_.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Check(device->CreateTexture2D(&d, nullptr, &previous_), "Create HUD previous capture");
    Check(device->CreateShaderResourceView(previous_.Get(), nullptr, &previousView_), "View HUD previous capture");
    d.Format = DXGI_FORMAT_R32_FLOAT; // the one format every D3D11 GPU can read and write as a typed UAV
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Check(device->CreateTexture2D(&d, nullptr, &confidence_), "Create HUD confidence");
    Check(device->CreateShaderResourceView(confidence_.Get(), nullptr, &confidenceView_), "View HUD confidence");
    Check(device->CreateUnorderedAccessView(confidence_.Get(), nullptr, &confidenceUav_), "Write HUD confidence");
    format_ = format;
    width_ = width;
    height_ = height;
    valid_ = false;
}

void HudMask::Apply(ID3D11DeviceContext *ctx, ID3D11Texture2D *originalTexture, ID3D11ShaderResourceView *original,
                    ID3D11UnorderedAccessView *target, unsigned width, unsigned height, float strength,
                    bool showMask) {
    D3D11_TEXTURE2D_DESC od{};
    originalTexture->GetDesc(&od);
    if (od.Width != width || od.Height != height)
        return;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    Allocate(device.Get(), od.Format, width, height);
    if (!valid_) {
        const float zero[4] = {};
        ctx->ClearUnorderedAccessViewFloat(confidenceUav_.Get(), zero);
        ctx->CopyResource(previous_.Get(), originalTexture);
        valid_ = true;
        return;
    }
    ComPtr<ID3D11Buffer> saved;
    ctx->CSGetConstantBuffers(0, 1, &saved);
    auto *cb = constants_.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    const auto upload = [&](const void *data) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Update HUD mask constants");
        memcpy(mapped.pData, data, 32);
        ctx->Unmap(constants_.Get(), 0);
    };
    const unsigned groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;
    ID3D11ShaderResourceView *none[2] = {};
    ID3D11UnorderedAccessView *empty = nullptr;

    // Ring radius follows the picture height: about 24 pixels at 1080p.
    const UpdateConstants u{tuning_.rise,
                            tuning_.fall,
                            tuning_.staticLimit,
                            tuning_.movingLimit,
                            int32_t(std::clamp(height / 45u, 8u, 64u)),
                            tuning_.minMoving,
                            width,
                            height};
    upload(&u);
    ID3D11ShaderResourceView *inputs[2] = {original, previousView_.Get()};
    auto *uav = confidenceUav_.Get();
    ctx->CSSetShader(update_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, inputs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch(groupsX, groupsY, 1);
    // Unbound before the confidence is read: D3D11 drops an SRV bound while its resource is a UAV.
    ctx->CSSetShaderResources(0, 2, none);
    ctx->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);

    const float s = std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f;
    const BlendConstants b{s, showMask ? 1.f : 0.f, tuning_.low, tuning_.high, width, height, {0, 0}};
    upload(&b);
    ID3D11ShaderResourceView *blendInputs[2] = {confidenceView_.Get(), original};
    ctx->CSSetShader(blend_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, blendInputs);
    ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    ctx->Dispatch(groupsX, groupsY, 1);
    ctx->CSSetShaderResources(0, 2, none);
    ctx->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);

    cb = saved.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CopyResource(previous_.Get(), originalTexture);
}
} // namespace neural
