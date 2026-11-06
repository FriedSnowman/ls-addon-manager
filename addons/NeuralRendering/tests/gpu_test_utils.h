// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <cstring>
namespace neural::test {
inline void Finish(Gpu &gpu, Recording &rec) {
    if (!gpu.Wait(gpu.Submit(rec), 10000))
        throw std::runtime_error("GPU test timed out");
}
inline void Upload(Gpu &gpu, Recording &rec, Resource &target, const void *bytes, size_t rowBytes) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    const auto desc = target.object->GetDesc();
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto upload = gpu.Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
    void *mapped = nullptr;
    D3D12_RANGE none{0, 0};
    Check(upload.object->Map(0, &none, &mapped), "Map test upload");
    for (uint32_t y = 0; y < target.height; ++y)
        memcpy(static_cast<char *>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
               static_cast<const char *>(bytes) + size_t(y) * rowBytes, rowBytes);
    upload.object->Unmap(0, nullptr);
    rec.Begin(gpu.fence.Get());
    target.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.object.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    to.pResource = target.object.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rec.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    target.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Finish(gpu, rec);
}
inline std::vector<uint8_t> Read(Gpu &gpu, Recording &rec, Resource &texture, uint32_t bytesPerPixel) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    const auto desc = texture.object->GetDesc();
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK);
    rec.Begin(gpu.fence.Get());
    const auto previous = texture.state;
    texture.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = texture.object.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.object.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    rec.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    texture.Transition(rec.list.Get(), previous);
    Finish(gpu, rec);
    void *mapped = nullptr;
    Check(readback.object->Map(0, nullptr, &mapped), "Map test readback");
    const size_t row = size_t(texture.width) * bytesPerPixel;
    std::vector<uint8_t> result(row * texture.height);
    for (uint32_t y = 0; y < texture.height; ++y)
        memcpy(result.data() + y * row,
               static_cast<const char *>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, row);
    D3D12_RANGE none{0, 0};
    readback.object->Unmap(0, &none);
    return result;
}
} // namespace neural::test
