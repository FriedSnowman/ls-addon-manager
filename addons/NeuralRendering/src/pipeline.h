// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "composition.h"
#include "model.h"
#include "optical_flow.h"
#include "picture.h"
#include "progress.h"
#include <chrono>
#include <d3d11_4.h>
#include <eam/real_frame.h>
#include <memory>
#include <vector>

namespace neural {
struct PipelineSettings {
    static constexpr unsigned MaxPasses = 10;
    ModelSettings model;
    CompositionSettings composition;
    PictureSettings picture;
    unsigned passes = 1;
    unsigned scalePercent = 100, flowGrid = 4, flowQuality = 2;
    std::wstring modelPath, forwarderPath, cachePath;
};
struct PipelineMetrics {
    uint64_t accepted = 0, cuts = 0, resets = 0;
    unsigned width = 0, height = 0, workingWidth = 0, workingHeight = 0;
    unsigned passes = 1;
    int captureGpuPriority = 0, nrQueuePriority = 0;
    double slotWaitMs = 0, summaryWaitMs = 0, prepareMs = 0, flowMs = 0, modelMs = 0, compositionMs = 0;
    float sceneDistance = 0;
};
// One device/size/settings epoch. Creation/retirement belong to the service worker.
// The host serializes Process on the capture render thread. The service keeps this
// epoch alive until Process returns, while status/configuration remain accessible.
class Pipeline {
  public:
    void SetProgress(Progress* progress) { progress_ = progress; }
    void Initialize(ID3D11Device *, unsigned width, unsigned height, Encoding, const PipelineSettings &);
    void Process(const EamRealFrame &, const CompositionSettings &, const PictureSettings & = {});
    bool Retire();
    PipelineMetrics Metrics() const { return metrics_; }
    void Invalidate() {
        reset_ = true;
        flow_.Invalidate();
    }

  private:
    Progress* progress_ = nullptr;
    void Mark(Stage stage) { if (progress_) progress_->Set(stage); }
    struct Shared {
        Resource texture;
        ComPtr<ID3D11Texture2D> texture11;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };
    struct Slot {
        Shared input, output;
        Resource proxy, keep, working, model, motion, summary, readback, timings;
        Resource modelScratch; // Allocated only for multipass; working stays the resolve anchor.
        Recording prepare, finish;
        uint64_t consumed = 0;
        bool hasTimings = false;
    };
    Gpu gpu_; // Must outlive all resources/engines.
    ComPtr<ID3D11Device5> device11_;
    ComPtr<ID3D11Fence> capture11_, consumed11_, complete11_;
    ComPtr<ID3D12Fence> capture12_, consumed12_;
    ComPtr<ID3D11ComputeShader> bridge_;
    ComPtr<ID3D12QueryHeap> queries_;
    uint64_t frequency_ = 0, transferValue_ = 0, lastConsumed_ = 0;
    std::array<Slot, 3> slots_;
    Resource depth_;
    Kernel scene_, guides_;
    Composition composition_;
    Picture picture_;
    OpticalFlow flow_;
    Model model_;
    std::vector<std::unique_ptr<Model>> extraModels_;
    PipelineSettings settings_;
    PipelineMetrics metrics_;
    Encoding encoding_ = Encoding::Sdr709;
    int previous_ = -1;
    unsigned cursor_ = 0;
    bool reset_ = true, initialized_ = false;
    uint64_t lastSequence_ = 0, lastFrameTick_ = 0;
    Shared Share(unsigned, unsigned);
    void SharedFence(ComPtr<ID3D11Fence> &, ComPtr<ID3D12Fence> &);
    void Copy(ID3D11DeviceContext *, ID3D11ShaderResourceView *, ID3D11UnorderedAccessView *);
    void ReadTimings(Slot &);
};
} // namespace neural
