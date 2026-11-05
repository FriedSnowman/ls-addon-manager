// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"

namespace neural {
enum class Encoding : uint32_t { Sdr709, ScRgb };
struct CompositionSettings {
    float detail = 1.f;
    float colour = 1.f;
    float maxRatio = 2.f;
    float whiteNits = 203.f;     // scRGB unit = 80 nits; SDR bypasses this bridge
    uint32_t reversible = 0;     // upstream: knee, Neutwo composed/replace, hybrid composed/replace
    uint32_t reconstruction = 1; // upstream matched residual, same frame only
    bool apply = true;
};
// The exact pinned OptiScaler cbuffer layout, including fields deliberately disabled
// for captured images: no game exposure or UI layer is invented.
struct CompositionConstants {
    uint32_t mode = 0;
    float whitePoint = 1;
    uint32_t width = 0, height = 0;
    float detail = 1, colour = 1;
    uint32_t debugView = 0;
    float maxRatio = 2;
    uint32_t passthrough = 1;
    float mvScaleX = 1, mvScaleY = 1;
    uint32_t guideWidth = 0, guideHeight = 0, compareMode = 0;
    float compareSplit = .5f, compareZoom = 1;
    uint32_t compareSwap = 0, transfer = 1;
    float debugScale = 1;
    uint32_t reversible = 0, applyModel = 1, useGameExposure = 0;
    float exposurePreMul = 1;
};
static_assert(sizeof(CompositionConstants) == 92);

class Composition {
  public:
    void Initialize(ID3D12Device *);
    void Encode(Gpu &, Recording &, Resource &original, Resource &proxy, Resource &keep, Encoding,
                const CompositionSettings &);
    void Downsample(Gpu &, Recording &, Resource &fullProxy, Resource &workingProxy);
    void Resolve(Gpu &, Recording &, Resource &workingProxy, Resource &model, Resource &original, Resource &output,
                 Encoding, const CompositionSettings &);

  private:
    Kernel kernel_;
    CompositionConstants Constants(uint32_t width, uint32_t height, Encoding, const CompositionSettings &) const;
};
} // namespace neural
