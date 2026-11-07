// SPDX-License-Identifier: GPL-3.0-only
// Test-only NGX interface. Copies colour when evaluated on a real command list;
// the forwarder smoke tests also use creation/retirement without a driver/GPU.
#include "model_runtime_fixture.h"
#include <array>

namespace {
std::array<bool, 32> handles{};
unsigned creates = 0, evaluates = 0, shutdowns = 0;
unsigned failCreate = 0, failEvaluate = 0;
std::array<FixtureEvaluation, 64> trace{};
unsigned Live() {
    unsigned live = 0;
    for (bool active : handles) live += active ? 1 : 0;
    return live;
}
}
extern "C" {
__declspec(dllexport) int neural_fixture_configure(unsigned creation, unsigned evaluation) {
    if (Live()) return 0;
    creates = evaluates = shutdowns = 0;
    failCreate = creation;
    failEvaluate = evaluation;
    return 1;
}
__declspec(dllexport) unsigned neural_fixture_live() { return Live(); }
__declspec(dllexport) unsigned neural_fixture_shutdowns() { return shutdowns; }
__declspec(dllexport) unsigned neural_fixture_evaluates() { return evaluates; }
__declspec(dllexport) const FixtureEvaluation *neural_fixture_trace() { return trace.data(); }
__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t *, ID3D12Device *, int,
                                                const void *) {
    return 1;
}
__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *, int feature, const void *,
                                                     void **out) {
    if (feature != 18)
        return 0;
    *out = nullptr;
    if (++creates == failCreate) return 0;
    for (auto &handle : handles) {
        if (!handle) {
            handle = true;
            *out = &handle;
            return 1;
        }
    }
    return 0;
}
__declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *cmd, const void *feature,
                                                       const void *parameters, void *) {
    bool live = false;
    for (auto &handle : handles) live |= feature == &handle && handle;
    if (!live || !parameters || evaluates >= trace.size()) return 0;
    auto &call = trace[evaluates++];
    call = {};
    call.feature = feature;
    void *params = const_cast<void *>(parameters);
    auto table = *reinterpret_cast<void ***>(params);
    auto resource = [&](const char *name, ID3D12Resource *&value) {
        using Get = int (*)(void *, const char *, unsigned long long *);
        unsigned long long pointer = 0;
        const auto result = reinterpret_cast<Get>(table[8])(params, name, &pointer);
        value = reinterpret_cast<ID3D12Resource *>(pointer);
        return result == 1 && value;
    };
    using GetUInt = int (*)(void *, const char *, unsigned *);
    using GetFloat = int (*)(void *, const char *, float *);
    if (!resource("DLSSNR.Color", call.colour) || !resource("DLSSNR.Output", call.output) ||
        !resource("DLSSNR.Depth", call.depth) || !resource("DLSSNR.MVec", call.motion) ||
        reinterpret_cast<GetUInt>(table[11])(params, "DLSSNR.Reset", &call.reset) != 1 ||
        reinterpret_cast<GetFloat>(table[14])(params, "DLSSNR.LocalToneStrength", &call.localTone) != 1 ||
        call.colour == call.output || evaluates == failEvaluate)
        return 0;
    if (cmd) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for (auto &b : barriers) {
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = call.colour;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource = call.output;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(2, barriers);
        cmd->CopyResource(call.output, call.colour);
        for (auto &b : barriers) {
            auto before = b.Transition.StateBefore;
            b.Transition.StateBefore = b.Transition.StateAfter;
            b.Transition.StateAfter = before;
        }
        cmd->ResourceBarrier(2, barriers);
    }
    return 1;
}
#ifndef FIXTURE_MISSING_RELEASE
__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(void *feature) {
    for (auto &handle : handles)
        if (feature == &handle && handle) {
            handle = false;
            return 1;
        }
    return 0;
}
#endif
#ifndef FIXTURE_MISSING_SHUTDOWN
__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *) {
    if (Live()) return 0;
    ++shutdowns;
    return 1;
}
#endif
}
