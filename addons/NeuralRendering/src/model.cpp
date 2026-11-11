// SPDX-License-Identifier: GPL-3.0-only
#include "model.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace neural {
namespace {
template <class Fn> Fn Export(HMODULE module, const char *name) {
    auto f = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!f)
        throw std::runtime_error(std::string("Missing runtime export: ") + name);
    return f;
}
void NgxCheck(int result, const char *operation) {
    if (result == 1)
        return;
    char text[256];
    snprintf(text, sizeof(text), "%s failed (NGX 0x%08X)", operation, unsigned(result));
    throw std::runtime_error(text);
}
std::wstring DriverCorePath() {
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    const LSTATUS result = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                                        L"FullPath", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &bytes);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("NVIDIA NGX driver location is unavailable");
    auto core = std::filesystem::path(path) / L"_nvngx.dll";
    if (!core.is_absolute())
        throw std::runtime_error("Invalid NVIDIA NGX driver path");
    return core.wstring();
}
// OptiScaler's runtime parameter ABI: integer/resource writes and a probed float
// setter. No proprietary NGX static library is linked or distributed by this addon.
void SetUInt(void *p, const char *key, unsigned value) {
    using Fn = void (*)(void *, const char *, unsigned);
    auto table = *reinterpret_cast<void ***>(p);
    reinterpret_cast<Fn>(table[3])(p, key, value);
}
int GetFloat(void *p, const char *key, float *value) {
    // MSVC's overload order puts the typed float getter at slot 14.
    using Fn = int (*)(void *, const char *, float *);
    auto table = *reinterpret_cast<void ***>(p);
    return reinterpret_cast<Fn>(table[14])(p, key, value);
}
} // namespace
void Model::Initialize(Gpu &gpu, Recording &rec, const std::wstring &forwarder, const std::wstring &model,
                       const std::wstring &cache, uint32_t w, uint32_t h, const ModelSettings &settings) {
    if (feature_ || params_)
        throw std::runtime_error("Model already initialized");
    const auto modelFile = std::filesystem::absolute(model);
    if (!std::filesystem::is_regular_file(modelFile))
        throw std::runtime_error("User-provided nvngx_dlssnr.dll was not found");
    auto strength = [](float v) { return std::isfinite(v) && v >= 0 && v <= 2.f; };
    if (settings.preset > 3 || settings.style > 2 || !strength(settings.intensity) ||
        !strength(settings.localStructure) || !strength(settings.localTone) ||
        !strength(settings.skinStructure))
        throw std::runtime_error("Invalid model settings");
    floatSlot_ = -1;
    gpu_ = &gpu;
    settings_ = settings;
    width_ = w;
    height_ = h;
    std::filesystem::create_directories(cache);
    const auto corePath = DriverCorePath();
    static HMODULE sharedCore =
        LoadLibraryExW(corePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    core_ = sharedCore;
    if (!core_)
        throw std::runtime_error("Cannot load the NVIDIA NGX driver core");
    // Allocate a parameter map without initializing the process-wide NGX core.
    // NR calls its own snippet directly. Core Init/GetCapability would establish
    // global feature search paths and can hide an independently loaded DLSS scaler.
    destroyParams_ = Export<decltype(destroyParams_)>(core_, "NVSDK_NGX_D3D12_DestroyParameters");
    using Allocate = int (*)(void **);
    NgxCheck(Export<Allocate>(core_, "NVSDK_NGX_D3D12_AllocateParameters")(&params_), "Allocate NR parameters");
    if (!params_)
        throw std::runtime_error("NR parameter allocation returned no map");
    forwarder_ =
        LoadLibraryExW(forwarder.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!forwarder_)
        throw std::runtime_error("Cannot load NeuralRendering_nvngx.dll");
    create_ = Export<Create>(forwarder_, "dlssnr_call_create");
    evaluate_ = Export<EvaluateFn>(forwarder_, "dlssnr_call_evaluate");
    release_ = Export<decltype(release_)>(forwarder_, "dlssnr_call_release");
    shutdownSnippet_ = Export<decltype(shutdownSnippet_)>(forwarder_, "dlssnr_call_shutdown");
    unloadSnippet_ = Export<decltype(unloadSnippet_)>(forwarder_, "dlssnr_call_unload");
    extras_ = Export<Extras>(forwarder_, "dlssnr_call_set_extras");
    auto probe = Export<void (*)(void *, const char *, float, int)>(forwarder_, "dlssnr_call_probe_float");
    auto setSlot = Export<void (*)(int)>(forwarder_, "dlssnr_call_set_float_slot");
    for (int candidate : {1, 2, 5, 6, 7, 4, 3, 0}) {
        float got = 0;
        probe(params_, "DLSSNR.FloatRoundTrip", .375f, candidate);
        if (GetFloat(params_, "DLSSNR.FloatRoundTrip", &got) == 1 && got == .375f) {
            floatSlot_ = candidate;
            setSlot(candidate);
            break;
        }
    }
    if (floatSlot_ < 0)
        throw std::runtime_error("NR float-parameter ABI could not be verified");
    SetUInt(params_, "Width", w);
    SetUInt(params_, "Height", h);
    SetUInt(params_, "OutWidth", w);
    SetUInt(params_, "OutHeight", h);
    SetUInt(params_, "PerfQualityValue", 2);
    extras_(params_, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    feature_ = create_(modelFile.c_str(), cache.c_str(), gpu.device.Get(), rec.list.Get(), params_, w, h,
                       int(settings.preset), settings.intensity, int(settings.style), settings.localStructure,
                       settings.localTone, settings.skinStructure, settings.autoMask ? 1 : 0, 1);
    if (!feature_) {
        const auto error = Export<const char *(*)()>(forwarder_, "dlssnr_call_last_error")();
        if (error && *error)
            throw std::runtime_error(error);
        auto initResult = Export<const int *>(forwarder_, "dlssnr_call_last_init");
        auto createResult = Export<const int *>(forwarder_, "dlssnr_call_last_create");
        char text[256];
        snprintf(text, sizeof(text), "NR feature creation failed (init=0x%08X create=0x%08X)", unsigned(*initResult),
                 unsigned(*createResult));
        throw std::runtime_error(text);
    }
    const std::pair<const char *, float> expected[] = {{"DLSSNR.Intensity", settings.intensity},
                                                       {"DLSSNR.LocalStructureStrength", settings.localStructure},
                                                       {"DLSSNR.LocalToneStrength", settings.localTone},
                                                       {"DLSSNR.SkinStructureStrength", settings.skinStructure}};
    for (const auto &parameter : expected) {
        float actual = 0;
        if (GetFloat(params_, parameter.first, &actual) != 1 || actual != parameter.second)
            throw std::runtime_error(std::string("Model parameter round-trip failed: ") + parameter.first);
    }
}
void Model::Evaluate(Recording &rec, Resource &colour, Resource &depth, Resource &motion, Resource &output,
                     bool reset) {
    if (!feature_)
        throw std::runtime_error("NR model is not ready");
    colour.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    motion.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    output.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    extras_(params_, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    const auto &s = settings_;
    const int result =
        evaluate_(rec.list.Get(), feature_, params_, colour.object.Get(), depth.object.Get(), motion.object.Get(),
                  output.object.Get(), width_, height_, width_, height_, 0, reset ? 1 : 0, s.intensity, int(s.style),
                  s.localStructure, s.localTone, s.skinStructure, s.autoMask ? 1 : 0, 1.f, 1.f);
    NgxCheck(result, "Evaluate NR");
    output.UavBarrier(rec.list.Get());
}
bool Model::ReleaseFeature() {
    if (gpu_ && !gpu_->Wait(gpu_->LastSubmitted()))
        return false;
    if (feature_ && release_) {
        release_(feature_);
        feature_ = nullptr;
    }
    return true;
}
bool Model::Shutdown() {
    if (!ReleaseFeature())
        return false;
    if (shutdownSnippet_ && gpu_ && shutdownSnippet_(gpu_->device.Get()) != 1)
        return false;
    if (params_ && destroyParams_) {
        destroyParams_(params_);
        params_ = nullptr;
    }
    if (unloadSnippet_ && unloadSnippet_() != 1)
        return false;
    if (forwarder_) {
        FreeLibrary(forwarder_);
        forwarder_ = nullptr;
    }
    // The NVIDIA core, like NVOFA, owns worker threads; keep the driver module loaded.
    core_ = nullptr;
    gpu_ = nullptr;
    shutdownSnippet_ = nullptr;
    unloadSnippet_ = nullptr;
    destroyParams_ = nullptr;
    release_ = nullptr;
    return true;
}
Model::~Model() {
    Shutdown();
}
} // namespace neural
