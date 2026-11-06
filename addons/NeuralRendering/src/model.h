// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"

namespace neural {
struct ModelSettings {
    uint32_t preset = 0, style = 0;
    float intensity = 1, localStructure = 0.8f, localTone = 0.3f, skinStructure = -1;
    bool autoMask = true;
};
class Model {
  public:
    ~Model();
    // Feature creation records work: release only after the containing queue retires.
    void Initialize(Gpu &, Recording &, const std::wstring &forwarder, const std::wstring &model,
                    const std::wstring &cache, uint32_t width, uint32_t height, const ModelSettings &);
    void Evaluate(Recording &, Resource &colour, Resource &depth, Resource &motion, Resource &output, bool reset);
    bool Shutdown();
    int FloatSlot() const { return floatSlot_; }

  private:
    using Create = void *(*)(const wchar_t *, const wchar_t *, ID3D12Device *, ID3D12GraphicsCommandList *, void *,
                             unsigned, unsigned, int, float, int, float, float, float, int, int);
    using EvaluateFn = int (*)(ID3D12GraphicsCommandList *, void *, void *, ID3D12Resource *, ID3D12Resource *,
                               ID3D12Resource *, ID3D12Resource *, unsigned, unsigned, unsigned, unsigned, int, int,
                               float, int, float, float, float, int, float, float);
    using Extras = void (*)(void *, float, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *, unsigned, unsigned,
                            unsigned, unsigned);
    HMODULE forwarder_ = nullptr, core_ = nullptr;
    void *params_ = nullptr;
    void *feature_ = nullptr;
    Gpu *gpu_ = nullptr;
    ModelSettings settings_{};
    uint32_t width_ = 0, height_ = 0;
    int floatSlot_ = -1;
    Create create_ = nullptr;
    EvaluateFn evaluate_ = nullptr;
    Extras extras_ = nullptr;
    void (*release_)(void *) = nullptr;
    int (*shutdownSnippet_)(ID3D12Device *) = nullptr;
    int (*unloadSnippet_)() = nullptr;
    int (*destroyParams_)(void *) = nullptr;
};
} // namespace neural
