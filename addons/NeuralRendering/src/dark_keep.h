// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <d3d11.h>

namespace neural {
// Dark area protection (dark_keep.hlsl): where the capture is dark, the composed image is blended back
// toward the captured pixel, in place, so the model's oily smoothing of dark grain does not show.
// strength 0 does nothing (no dispatch). Restores the context's compute constant buffer slot 0 and
// leaves no views bound.
class DarkKeep {
  public:
    void Apply(ID3D11DeviceContext *, ID3D11ShaderResourceView *original, ID3D11UnorderedAccessView *target,
               unsigned width, unsigned height, float strength, float threshold, bool scRgb, float whiteNits);

  private:
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};
} // namespace neural
