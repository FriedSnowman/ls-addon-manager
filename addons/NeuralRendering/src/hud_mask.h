// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <d3d11.h>

namespace neural {
// Experimental HUD protection. The capture has the interface drawn in, so there is no UI layer to
// give the model; instead this estimates where the HUD is from how pixels change between captures
// (still while their surroundings move) and puts the captured pixels back there. Restores the
// context's compute constant buffer slot 0 and leaves no views bound.
class HudMask {
  public:
    struct Tuning {
        float rise = 1.f / 45, fall = .25f, staticLimit = .01f, movingLimit = .05f;
        unsigned minMoving = 4;
        float low = .5f, high = .9f;
    };
    // Updates the confidence from this capture and the previous one, then blends the original into
    // target. The first call after a reset only records the capture. originalTexture is copied as the
    // next call's previous capture, so its format and size must stay the same until Reset.
    void Apply(ID3D11DeviceContext *, ID3D11Texture2D *originalTexture, ID3D11ShaderResourceView *original,
               ID3D11UnorderedAccessView *target, unsigned width, unsigned height, float strength, bool showMask);
    void Reset() { valid_ = false; }
    void SetTuning(const Tuning &t) { tuning_ = t; }

  private:
    void Allocate(ID3D11Device *, DXGI_FORMAT, unsigned width, unsigned height);
    Tuning tuning_;
    ComPtr<ID3D11ComputeShader> update_, blend_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Texture2D> previous_, confidence_;
    ComPtr<ID3D11ShaderResourceView> previousView_, confidenceView_;
    ComPtr<ID3D11UnorderedAccessView> confidenceUav_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    unsigned width_ = 0, height_ = 0;
    bool valid_ = false;
};
} // namespace neural
