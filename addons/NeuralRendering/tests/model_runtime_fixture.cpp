// SPDX-License-Identifier: GPL-3.0-only
// Test-only NGX interface. No model, driver or GPU work is performed.
#include <d3d12.h>

namespace {
int handle;
}
extern "C" {
__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t *, ID3D12Device *, int,
                                                const void *) {
    return 1;
}
__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *, int feature, const void *,
                                                     void **out) {
    if (feature != 18)
        return 0;
    *out = &handle;
    return 1;
}
__declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *, const void *feature,
                                                       const void *, void *) {
    return feature == &handle ? 1 : 0;
}
#ifndef FIXTURE_MISSING_RELEASE
__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(void *feature) {
    return feature == &handle ? 1 : 0;
}
#endif
#ifndef FIXTURE_MISSING_SHUTDOWN
__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *) {
    return 1;
}
#endif
}
