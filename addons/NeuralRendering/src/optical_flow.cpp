// SPDX-License-Identifier: GPL-3.0-only
// D3D12 fence protocol informed by video2dlssnr (MIT); see licenses and manifest.
#include "optical_flow.h"
#include "flow_convert.h"
#include "flow_pack.h"
#include <algorithm>

namespace neural {
namespace {
void OfCheck(NV_OF_STATUS result, const char *operation) {
    if (result != NV_OF_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (NVOFA " + std::to_string(result) + ")");
}
bool FenceWait(ID3D12Fence *fence, uint64_t value, DWORD timeout) {
    if (!fence || value == 0)
        return true;
    auto done = fence->GetCompletedValue();
    if (done == UINT64_MAX)
        return false;
    if (done >= value)
        return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    bool ok = SUCCEEDED(hr) && WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
    done = fence->GetCompletedValue();
    ok = ok && done != UINT64_MAX && done >= value;
    CloseHandle(event);
    return ok;
}
} // namespace
std::vector<uint32_t> OpticalFlow::Caps(NV_OF_CAPS cap) {
    uint32_t count = 0;
    OfCheck(api_.nvOFGetCaps(session_, cap, nullptr, &count), "Query optical flow capability count");
    if (!count || count > 64)
        throw std::runtime_error("Invalid NVOFA capability count");
    std::vector<uint32_t> result(count);
    OfCheck(api_.nvOFGetCaps(session_, cap, result.data(), &count), "Query optical flow capabilities");
    return result;
}
std::vector<DXGI_FORMAT> OpticalFlow::Formats(NV_OF_BUFFER_USAGE usage) {
    uint32_t count = 0;
    OfCheck(api_.nvOFGetSurfaceFormatCountD3D12(session_, usage, NV_OF_MODE_OPTICALFLOW, &count),
            "Query NVOFA format count");
    if (!count || count > 64)
        throw std::runtime_error("No supported NVOFA formats");
    std::vector<DXGI_FORMAT> result(count);
    OfCheck(api_.nvOFGetSurfaceFormatD3D12(session_, usage, NV_OF_MODE_OPTICALFLOW, result.data()),
            "Query NVOFA formats");
    return result;
}
void OpticalFlow::Register(Gpu &gpu, Resource &texture, NvOFGPUBufferHandle &handle) {
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
    p.resource = texture.object.Get();
    p.hOFGpuBuffer = &handle;
    p.inputFencePoint = {gpu.fence.Get(), gpu.LastSubmitted()};
    p.outputFencePoint = {readyFence_.Get(), progress_ + 1};
    OfCheck(api_.nvOFRegisterResourceD3D12(session_, &p), "Register optical flow resource");
    ++progress_;
    if (!handle)
        throw std::runtime_error("NVOFA returned an empty resource handle");
}
void OpticalFlow::Initialize(Gpu &gpu, uint32_t w, uint32_t h, uint32_t grid, NV_OF_PERF_LEVEL quality) {
    if (session_)
        throw std::runtime_error("Optical flow session is already initialized");
    gpu_ = &gpu;
    // The driver owns worker threads; retain one process-lifetime module reference.
    static HMODULE driver = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!driver)
        throw std::runtime_error("NVIDIA Optical Flow driver is unavailable");
    using Create = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST *);
    auto create = reinterpret_cast<Create>(GetProcAddress(driver, "NvOFAPICreateInstanceD3D12"));
    if (!create)
        throw std::runtime_error("NVIDIA Optical Flow D3D12 entry point is unavailable");
    OfCheck(create(NV_OF_API_VERSION, &api_), "Create NVOFA API");
    if (!api_.nvCreateOpticalFlowD3D12 || !api_.nvOFGetCaps || !api_.nvOFInit || !api_.nvOFExecuteD3D12 ||
        !api_.nvOFRegisterResourceD3D12 || !api_.nvOFUnregisterResourceD3D12 || !api_.nvOFDestroy ||
        !api_.nvOFGetSurfaceFormatCountD3D12 || !api_.nvOFGetSurfaceFormatD3D12)
        throw std::runtime_error("Incomplete NVIDIA Optical Flow API");
    OfCheck(api_.nvCreateOpticalFlowD3D12(gpu.device.Get(), &session_), "Create optical flow session");
    grids_ = Caps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
    if (std::find(grids_.begin(), grids_.end(), grid) == grids_.end())
        throw std::runtime_error("Requested optical flow grid is unsupported");
    if (w < Caps(NV_OF_CAPS_WIDTH_MIN)[0] || w > Caps(NV_OF_CAPS_WIDTH_MAX)[0] || h < Caps(NV_OF_CAPS_HEIGHT_MIN)[0] ||
        h > Caps(NV_OF_CAPS_HEIGHT_MAX)[0])
        throw std::runtime_error("Optical flow resolution is outside driver capabilities");
    auto inputFormats = Formats(NV_OF_BUFFER_USAGE_INPUT), outputFormats = Formats(NV_OF_BUFFER_USAGE_OUTPUT),
         costFormats = Formats(NV_OF_BUFFER_USAGE_COST);
    auto has = [](const auto &list, DXGI_FORMAT f) { return std::find(list.begin(), list.end(), f) != list.end(); };
    if (!has(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM) || !has(outputFormats, DXGI_FORMAT_R16G16_SINT))
        throw std::runtime_error("Required optical flow formats are unsupported");
    const DXGI_FORMAT costFormat = has(costFormats, DXGI_FORMAT_R32_UINT)  ? DXGI_FORMAT_R32_UINT
                                   : has(costFormats, DXGI_FORMAT_R8_UINT) ? DXGI_FORMAT_R8_UINT
                                                                           : DXGI_FORMAT_UNKNOWN;
    if (costFormat == DXGI_FORMAT_UNKNOWN)
        throw std::runtime_error("No supported optical flow confidence format");
    NV_OF_INIT_PARAMS p{};
    p.width = w;
    p.height = h;
    p.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid);
    p.mode = NV_OF_MODE_OPTICALFLOW;
    p.perfLevel = quality;
    p.enableOutputCost = NV_OF_TRUE;
    p.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    p.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    OfCheck(api_.nvOFInit(session_, &p), "Initialize optical flow");
    width_ = w;
    height_ = h;
    grid_ = grid;
    rowPitch_ = (w * 4 + 255) & ~255u;
    Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readyFence_)), "Create optical flow fence");
    lastQueueFence_ = gpu.fence;
    for (uint32_t i = 0; i < kSlots; ++i) {
        inputs_[i] = gpu.Texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        packed_[i] =
            gpu.Buffer(uint64_t(rowPitch_) * h, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Register(gpu, inputs_[i], inputHandles_[i]);
    }
    flow_ = gpu.Texture((w + grid - 1) / grid, (h + grid - 1) / grid, DXGI_FORMAT_R16G16_SINT);
    cost_ = gpu.Texture(flow_.width, flow_.height, costFormat);
    Register(gpu, flow_, flowHandle_);
    Register(gpu, cost_, costHandle_);
    Check(gpu.queue->Wait(readyFence_.Get(), progress_), "Wait for NVOFA registration");
    lastQueueValue_ = gpu.Signal();
    packKernel_.Initialize(gpu.device.Get(), g_flow_pack, sizeof(g_flow_pack));
    convertKernel_.Initialize(gpu.device.Get(), g_flow_convert, sizeof(g_flow_convert));
}
void OpticalFlow::Prepare(Gpu &gpu, Recording &rec, Resource &original, uint32_t slot) {
    if (slot >= kSlots)
        throw std::runtime_error("Invalid optical flow slot");
    auto &packed = packed_[slot];
    auto &input = inputs_[slot];
    original.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    packed.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t c[] = {width_, height_, rowPitch_, 0};
    packKernel_.Dispatch(gpu.device.Get(), rec, c, sizeof(c), {&original}, {&packed}, (width_ + 7) / 8,
                         (height_ + 7) / 8);
    packed.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    input.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = packed.object.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, width_, height_, 1, rowPitch_};
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = input.object.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rec.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    input.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COMMON);
}
void OpticalFlow::Execute(Gpu &gpu, uint32_t current, uint32_t previous, bool reset, uint64_t ready) {
    lastQueueValue_ = ready;
    if (reset) {
        hintsReady_ = false;
        return;
    }
    if (current >= kSlots || previous >= kSlots || current == previous)
        throw std::runtime_error("Invalid optical flow frame association");
    NV_OF_FENCE_POINT inputPoint{gpu.fence.Get(), ready}, outputPoint{readyFence_.Get(), progress_ + 1};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame = inputHandles_[current];
    in.referenceFrame = inputHandles_[previous];
    in.disableTemporalHints = hintsReady_ ? NV_OF_FALSE : NV_OF_TRUE;
    in.numFencePoints = 1;
    in.fencePoint = &inputPoint;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = flowHandle_;
    out.outputCostBuffer = costHandle_;
    out.fencePoint = &outputPoint;
    const auto result = api_.nvOFExecuteD3D12(session_, &in, &out);
    if (result != NV_OF_SUCCESS) {
        hintsReady_ = false;
        OfCheck(result, "Execute optical flow");
    }
    ++progress_;
    hintsReady_ = true;
    Check(gpu.queue->Wait(readyFence_.Get(), progress_), "Wait for current optical flow");
    lastQueueValue_ = gpu.Signal();
}
void OpticalFlow::Convert(Gpu &gpu, Recording &rec, Resource &motion, bool reset) {
    flow_.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    motion.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t c[] = {motion.width, motion.height, width_, height_, grid_, uint32_t(reset), 0, 0};
    convertKernel_.Dispatch(gpu.device.Get(), rec, c, sizeof(c), {&flow_}, {&motion}, (motion.width + 7) / 8,
                            (motion.height + 7) / 8);
    flow_.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COMMON);
}
bool OpticalFlow::Shutdown(DWORD timeout) {
    if (!session_)
        return true;
    if (gpu_)
        lastQueueValue_ = gpu_->LastSubmitted();
    if (!FenceWait(lastQueueFence_.Get(), lastQueueValue_, timeout) ||
        !FenceWait(readyFence_.Get(), progress_, timeout))
        return false;
    auto unregister = [&](NvOFGPUBufferHandle &h) {
        if (h) {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{h};
            if (api_.nvOFUnregisterResourceD3D12(&p) != NV_OF_SUCCESS)
                return false;
            h = nullptr;
        }
        return true;
    };
    for (auto &h : inputHandles_)
        if (!unregister(h))
            return false;
    if (!unregister(flowHandle_) || !unregister(costHandle_))
        return false;
    // NVIDIA's D3D12 resource wrappers retain session state even after unregister.
    // Release registered textures before destroying their session.
    for (auto &r : inputs_)
        r = {};
    flow_ = {};
    cost_ = {};
    api_.nvOFDestroy(session_);
    session_ = nullptr;
    return true;
}
OpticalFlow::~OpticalFlow() {
    if (Shutdown())
        return;
    // Device/driver failure: do not destroy registrations or free resources still
    // owned by the optical-flow engine. The containing pipeline also quarantines its queue.
    for (auto &r : inputs_)
        r.object.Detach();
    for (auto &r : packed_)
        r.object.Detach();
    flow_.object.Detach();
    cost_.object.Detach();
    readyFence_.Detach();
    lastQueueFence_.Detach();
    packKernel_.signature.Detach();
    packKernel_.pipeline.Detach();
    convertKernel_.signature.Detach();
    convertKernel_.pipeline.Detach();
}
} // namespace neural
