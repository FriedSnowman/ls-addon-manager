// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include "nvOpticalFlowD3D12.h"

namespace neural {
class OpticalFlow {
  public:
    static constexpr uint32_t kSlots = 3;
    ~OpticalFlow();
    void Initialize(Gpu &, uint32_t width, uint32_t height, uint32_t grid = 4,
                    NV_OF_PERF_LEVEL quality = NV_OF_PERF_LEVEL_MEDIUM);
    void Prepare(Gpu &, Recording &, Resource &originalProxy, uint32_t slot);
    // The engine waits for the exact submitted original. The graphics queue then
    // waits for this execution before converting/consuming its output. No CPU wait.
    void Execute(Gpu &, uint32_t current, uint32_t previous, bool reset, uint64_t originalsReady);
    void Convert(Gpu &, Recording &, Resource &motion, bool reset);
    void Invalidate() { hintsReady_ = false; }
    bool Shutdown(DWORD timeoutMs = 3000);
    Resource &Grid() { return flow_; }
    Resource &Cost() { return cost_; }
    uint32_t GridSize() const { return grid_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    std::vector<uint32_t> SupportedGrids() const { return grids_; }

  private:
    NV_OF_D3D12_API_FUNCTION_LIST api_{};
    NvOFHandle session_ = nullptr;
    std::array<NvOFGPUBufferHandle, kSlots> inputHandles_{};
    NvOFGPUBufferHandle flowHandle_ = nullptr, costHandle_ = nullptr;
    std::array<Resource, kSlots> inputs_, packed_;
    Resource flow_, cost_;
    Kernel packKernel_, convertKernel_;
    ComPtr<ID3D12Fence> readyFence_, lastQueueFence_;
    uint64_t progress_ = 0, lastQueueValue_ = 0;
    uint32_t width_ = 0, height_ = 0, grid_ = 0, rowPitch_ = 0;
    Gpu *gpu_ = nullptr; // containing queue outlives this session
    bool hintsReady_ = false;
    std::vector<uint32_t> grids_;
    std::vector<uint32_t> Caps(NV_OF_CAPS);
    std::vector<DXGI_FORMAT> Formats(NV_OF_BUFFER_USAGE);
    void Register(Gpu &, Resource &, NvOFGPUBufferHandle &);
};
} // namespace neural
