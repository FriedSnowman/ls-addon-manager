// SPDX-License-Identifier: GPL-3.0-only
#include "composition.h"
#include "optiscaler.h"
#include <cmath>

namespace neural {
void Composition::Initialize(ID3D12Device *device) {
    kernel_.Initialize(device, g_optiscaler, sizeof(g_optiscaler));
}
CompositionConstants Composition::Constants(uint32_t w, uint32_t h, Encoding encoding,
                                            const CompositionSettings &settings) const {
    if (!std::isfinite(settings.whiteNits) || settings.whiteNits <= 0 || !std::isfinite(settings.detail) ||
        !std::isfinite(settings.colour) || !std::isfinite(settings.maxRatio) || settings.maxRatio < 1 ||
        settings.reversible > 4 || settings.reconstruction > 1)
        throw std::runtime_error("Invalid composition settings");
    CompositionConstants c;
    c.width = w;
    c.height = h;
    c.guideWidth = w;
    c.guideHeight = h;
    c.whitePoint = encoding == Encoding::ScRgb ? settings.whiteNits / 80.f : 1.f;
    c.passthrough = encoding == Encoding::Sdr709 ? 1 : 0;
    c.detail = settings.detail;
    c.colour = settings.colour;
    c.maxRatio = settings.maxRatio;
    c.reversible = settings.reversible;
    c.transfer = settings.reconstruction;
    c.applyModel = settings.apply ? 1 : 0;
    return c;
}
void Composition::Encode(Gpu &gpu, Recording &rec, Resource &original, Resource &proxy, Resource &keep,
                         Encoding encoding, const CompositionSettings &settings) {
    auto c = Constants(original.width, original.height, encoding, settings);
    original.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    proxy.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    keep.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    kernel_.Dispatch(gpu.device.Get(), rec, &c, sizeof(c), {&original}, {&proxy, &keep}, (c.width + 7) / 8,
                     (c.height + 7) / 8);
    proxy.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    keep.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void Composition::Downsample(Gpu &gpu, Recording &rec, Resource &full, Resource &working) {
    CompositionConstants c;
    c.mode = 2;
    c.width = working.width;
    c.height = working.height;
    full.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    working.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    kernel_.Dispatch(gpu.device.Get(), rec, &c, sizeof(c), {&full}, {&working}, (c.width + 7) / 8, (c.height + 7) / 8);
    working.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void Composition::Resolve(Gpu &gpu, Recording &rec, Resource &proxy, Resource &model, Resource &original,
                          Resource &output, Encoding encoding, const CompositionSettings &settings) {
    auto c = Constants(output.width, output.height, encoding, settings);
    c.mode = 1;
    proxy.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    model.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    original.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    output.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    kernel_.Dispatch(gpu.device.Get(), rec, &c, sizeof(c), {&proxy, &model, &original}, {&output}, (c.width + 7) / 8,
                     (c.height + 7) / 8);
    output.UavBarrier(rec.list.Get());
}
} // namespace neural
