// SPDX-License-Identifier: GPL-3.0-only
#include "gpu.h"
#include <cstdio>
#include <cstring>

namespace neural {
void Check(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr))
        return;
    char message[512];
    snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(hr));
    throw std::runtime_error(message);
}
void Resource::Transition(ID3D12GraphicsCommandList *list, D3D12_RESOURCE_STATES next) {
    if (state == next)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = object.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = next;
    list->ResourceBarrier(1, &barrier);
    state = next;
}
void Resource::UavBarrier(ID3D12GraphicsCommandList *list) const {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = object.Get();
    list->ResourceBarrier(1, &barrier);
}
void Recording::Initialize(ID3D12Device *device) {
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "Create list");
    Check(list->Close(), "Close new list");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 256;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "Create descriptor arena");
    descriptorStride = device->GetDescriptorHandleIncrementSize(hd.Type);
}
void Recording::Begin(ID3D12Fence *fence) {
    const uint64_t done = fence->GetCompletedValue();
    if (done == UINT64_MAX || done < completion)
        throw std::runtime_error("Command allocator is still in flight or device removed");
    Check(allocator->Reset(), "Reset allocator");
    Check(list->Reset(allocator.Get(), nullptr), "Reset list");
    nextDescriptor = 0;
}
D3D12_CPU_DESCRIPTOR_HANDLE Recording::Cpu(uint32_t index) const {
    auto h = descriptors->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * descriptorStride;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Recording::Gpu(uint32_t index) const {
    auto h = descriptors->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * descriptorStride;
    return h;
}
uint32_t Recording::Allocate(uint32_t count) {
    if (nextDescriptor + count > 256)
        throw std::runtime_error("Descriptor arena exhausted");
    const uint32_t first = nextDescriptor;
    nextDescriptor += count;
    return first;
}
void Kernel::Initialize(ID3D12Device *device, const void *code, size_t bytes) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 5};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 24};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized, error;
    Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error),
          "Serialize compute signature");
    Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&signature)),
          "Create compute signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = signature.Get();
    pd.CS = {code, bytes};
    Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)), "Create compute pipeline");
}
void Kernel::Dispatch(ID3D12Device *device, Recording &rec, const void *constants, uint32_t bytes,
                      const std::array<Resource *, 5> &inputs, const std::array<Resource *, 2> &outputs, uint32_t gx,
                      uint32_t gy) const {
    if (bytes > 96 || bytes % 4)
        throw std::runtime_error("Invalid shader constants");
    const uint32_t base = rec.Allocate(7);
    for (uint32_t i = 0; i < 5; ++i) {
        Resource *r = inputs[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (r && r->format == DXGI_FORMAT_UNKNOWN) {
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.NumElements = uint32_t(r->object->GetDesc().Width / 4);
            d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        } else {
            d.Format = r ? r->format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
        }
        device->CreateShaderResourceView(r ? r->object.Get() : nullptr, &d, rec.Cpu(base + i));
    }
    for (uint32_t i = 0; i < 2; ++i) {
        Resource *r = outputs[i];
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        if (r && r->format == DXGI_FORMAT_UNKNOWN) {
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = uint32_t(r->object->GetDesc().Width / 4);
            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        } else {
            d.Format = r ? r->format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        device->CreateUnorderedAccessView(r ? r->object.Get() : nullptr, nullptr, &d, rec.Cpu(base + 5 + i));
    }
    auto *heap = rec.descriptors.Get();
    rec.list->SetDescriptorHeaps(1, &heap);
    rec.list->SetComputeRootSignature(signature.Get());
    rec.list->SetPipelineState(pipeline.Get());
    std::array<uint32_t, 24> padded{};
    memcpy(padded.data(), constants, bytes);
    rec.list->SetComputeRoot32BitConstants(0, 24, padded.data(), 0);
    rec.list->SetComputeRootDescriptorTable(1, rec.Gpu(base));
    rec.list->Dispatch(gx, gy, 1);
}
void Gpu::Initialize(const LUID *luid, bool elevatedPriority) {
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    if (luid)
        Check(factory->EnumAdapterByLuid(*luid, IID_PPV_ARGS(&adapter)), "Find capture adapter");
    else
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            candidate->GetDesc1(&d);
            if (d.VendorId == 0x10de && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                break;
            }
        }
    if (!adapter)
        throw std::runtime_error("No NVIDIA adapter");
    DXGI_ADAPTER_DESC1 d{};
    Check(adapter->GetDesc1(&d), "Read adapter");
    if (d.VendorId != 0x10de)
        throw std::runtime_error("NR requires the NVIDIA capture adapter");
    adapterName = d.Description;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
          "Create NVIDIA D3D12 device");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // LS raises its D3D11 GPU priority. Its frame generation now depends on this
    // queue, so do not leave NR at normal priority under competing game work.
    // DXGI's -7..7 values do not map numerically to D3D12 priority classes.
    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY support{q.Type, D3D12_COMMAND_QUEUE_PRIORITY_HIGH, FALSE};
    if (elevatedPriority &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &support, sizeof(support))) &&
        support.PriorityForTypeIsSupported)
        q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    HRESULT created = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    if (FAILED(created) && q.Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL) {
        q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        created = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    }
    Check(created, "Create NR queue");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "Create queue fence");
}
Resource Gpu::Texture(uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    Resource r;
    r.width = w;
    r.height = h;
    r.format = format;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, r.state, nullptr, IID_PPV_ARGS(&r.object)),
          "Create GPU texture");
    return r;
}
Resource Gpu::Buffer(uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags) {
    Resource r;
    r.state = type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
              : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                 : D3D12_RESOURCE_STATE_COMMON;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, r.state, nullptr, IID_PPV_ARGS(&r.object)),
          "Create GPU buffer");
    return r;
}
uint64_t Gpu::Signal() {
    const uint64_t value = ++nextFence_;
    Check(queue->Signal(fence.Get(), value), "Signal NR queue");
    return value;
}
uint64_t Gpu::Submit(Recording &rec) {
    Check(rec.list->Close(), "Close command list");
    ID3D12CommandList *list = rec.list.Get();
    queue->ExecuteCommandLists(1, &list);
    rec.completion = Signal();
    return rec.completion;
}
bool Gpu::Complete(uint64_t value) const {
    const auto done = fence->GetCompletedValue();
    return done != UINT64_MAX && done >= value;
}
bool Gpu::Wait(uint64_t value, DWORD timeout) const {
    if (Complete(value))
        return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    const bool done = SUCCEEDED(hr) && WaitForSingleObject(event, timeout) == WAIT_OBJECT_0 && Complete(value);
    CloseHandle(event);
    return done;
}
} // namespace neural
