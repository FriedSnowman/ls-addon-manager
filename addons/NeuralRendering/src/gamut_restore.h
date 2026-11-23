// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <d3d11.h>

namespace neural {
// scRGB only: puts back the negative (outside BT.709) part of each captured pixel that the pinned
// composition clamps away, in place on the composed image. Pixels the capture had in gamut are not
// written. Restores the context's compute constant buffer slot 0.
class GamutRestore {
  public:
    void Apply(ID3D11DeviceContext *, ID3D11ShaderResourceView *original, ID3D11UnorderedAccessView *target,
               unsigned width, unsigned height, float maxRatio);

  private:
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};
} // namespace neural
