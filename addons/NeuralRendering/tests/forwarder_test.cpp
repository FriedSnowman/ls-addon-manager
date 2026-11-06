// SPDX-License-Identifier: GPL-3.0-only
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class Fn> Fn Export(HMODULE module, const char *name) {
    auto fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
    Require(fn != nullptr, name);
    return fn;
}
struct Params {
    void **table;
    unsigned preset = 99;
    float intensity = -1;
};
void SetUInt(void *p, const char *name, unsigned value) {
    if (strcmp(name, "DLSSNR.Hint.Render.Preset") == 0)
        static_cast<Params *>(p)->preset = value;
}
void SetFloat(void *p, const char *name, float value) {
    if (strcmp(name, "DLSSNR.Intensity") == 0)
        static_cast<Params *>(p)->intensity = value;
}
}
int wmain() {
    try {
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        const auto folder = std::filesystem::path(exe).parent_path();
        const auto forwarderPath = folder / L"NeuralRendering_nvngx.dll";
        auto module = LoadLibraryW(forwarderPath.c_str());
        Require(module != nullptr, "Load forwarder");
        using Create = void *(*)(const wchar_t *, const wchar_t *, ID3D12Device *, ID3D12GraphicsCommandList *,
                                void *, unsigned, unsigned, int, float, int, float, float, float, int, int);
        auto create = Export<Create>(module, "dlssnr_call_create");
        auto error = Export<const char *(*)()>(module, "dlssnr_call_last_error");
        auto release = Export<void (*)(void *)>(module, "dlssnr_call_release");
        auto shutdown = Export<int (*)(ID3D12Device *)>(module, "dlssnr_call_shutdown");
        auto unload = Export<int (*)()>(module, "dlssnr_call_unload");
        Export<void (*)(int)>(module, "dlssnr_call_set_float_slot")(1);
        void *table[4] = {nullptr, reinterpret_cast<void *>(&SetFloat), nullptr,
                          reinterpret_cast<void *>(&SetUInt)};
        Params params{table};
        auto start = [&](const std::filesystem::path &path, int preset) {
            return create(path.c_str(), folder.c_str(), nullptr, nullptr, &params, 640, 360, preset, 1.f, 0,
                          .8f, .3f, -1.f, 1, 1);
        };
        const auto model = folder / L"neural_runtime_fixture.dll";
        // An unlisted runtime without a version resource is accepted by its interface.
        // Recreate A -> B -> default: an explicit zero must clear the previous hint.
        for (int preset : {1, 2, 0}) {
            auto feature = start(model, preset);
            Require(feature != nullptr && !*error(), "Compatible runtime was rejected");
            Require(params.preset == unsigned(preset), "Model selection did not reach feature creation");
            Require(params.intensity == 1.f, "Float controls changed while selecting the model");
            Require(unload() == 0 && shutdown(nullptr) == 0, "Live feature was retired too early");
            release(feature);
            Require(shutdown(nullptr) == 1 && unload() == 1, "Feature retirement failed");
        }
        puts("PASS compatible runtime and Original/Fast/default recreation");

        const std::pair<const wchar_t *, const char *> invalid[] = {
            {L"missing-runtime-for-test.dll", "Cannot load NR model DLL"},
            {L"NeuralRendering_nvngx.dll", "NVSDK_NGX_D3D12_Init_Ext"},
            {L"neural_runtime_no_release.dll", "NVSDK_NGX_D3D12_ReleaseFeature"},
            {L"neural_runtime_no_shutdown.dll", "NVSDK_NGX_D3D12_Shutdown1"},
        };
        for (const auto &bad : invalid) {
            // Retry the same incompatible DLL without unload: a cached partial
            // export table must never let a second creation slip through.
            for (int attempt = 0; attempt < 2; ++attempt) {
                Require(start(folder / bad.first, 2) == nullptr, "Incompatible DLL was accepted");
                Require(strstr(error(), bad.second) != nullptr, "Missing load failure diagnostic");
            }
            Require(shutdown(nullptr) == 1 && unload() == 1, "Failed load prevented retirement");
            auto feature = start(model, 2);
            Require(feature != nullptr && !*error(), "Failed load prevented recovery");
            release(feature);
            Require(shutdown(nullptr) == 1 && unload() == 1, "Recovery retirement failed");
        }
        puts("PASS missing/incompatible DLL rejection and recovery");
        FreeLibrary(module);
        puts("FORWARDER TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FORWARDER TEST ERROR: %s\n", e.what());
        return 1;
    }
}
