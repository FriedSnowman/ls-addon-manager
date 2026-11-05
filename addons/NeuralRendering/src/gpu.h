// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace neural {
using Microsoft::WRL::ComPtr;
void Check(HRESULT result, const char *operation);

struct Resource {
    ComPtr<ID3D12Resource> object;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    void Transition(ID3D12GraphicsCommandList *, D3D12_RESOURCE_STATES);
    void UavBarrier(ID3D12GraphicsCommandList *) const;
};

// Each in-flight recording owns its descriptors and allocator. Reuse is allowed only
// after its completion fence. Descriptors are never edited underneath submitted work.
struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    uint32_t nextDescriptor = 0, descriptorStride = 0;
    uint64_t completion = 0;
    void Initialize(ID3D12Device *);
    void Begin(ID3D12Fence *);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const;
    uint32_t Allocate(uint32_t count);
};

struct Kernel {
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pipeline;
    void Initialize(ID3D12Device *, const void *bytecode, size_t bytes);
    // Uniform table: t0..t4, u0..u1; up to 24 DWORD root constants (OptiScaler layout).
    // A buffer Resource is viewed as raw; textures use their declared typed format.
    void Dispatch(ID3D12Device *, Recording &, const void *constants, uint32_t constantBytes,
                  const std::array<Resource *, 5> &inputs, const std::array<Resource *, 2> &outputs, uint32_t groupsX,
                  uint32_t groupsY) const;
};

class Gpu {
  public:
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::wstring adapterName;
    void Initialize(const LUID *requiredAdapter = nullptr);
    Resource Texture(uint32_t width, uint32_t height, DXGI_FORMAT,
                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Resource Buffer(uint64_t bytes, D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    uint64_t Submit(Recording &);
    uint64_t Signal();
    bool Complete(uint64_t value) const;
    bool Wait(uint64_t value, DWORD timeoutMs = 3000) const; // init/test/retirement only
    uint64_t LastSubmitted() const { return nextFence_; }

  private:
    uint64_t nextFence_ = 0;
};
} // namespace neural
