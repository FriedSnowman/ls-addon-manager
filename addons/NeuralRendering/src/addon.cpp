// SPDX-License-Identifier: GPL-3.0-only
#include "service.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <commdlg.h>
#include <cstdio>
#include <eam/addon_sdk.h>
#include <eam/widgets.h>
#include <filesystem>
namespace {
constexpr const char *Id = "DLSSNR02";
IHost *host = nullptr;
std::unique_ptr<neural::Service> service;
neural::PipelineSettings settings;
bool enabled = true, rebuildPending = false;
char modelPath[32768]{};
std::atomic<ULONGLONG> lastStatus{0};
std::atomic<double> lastCallbackMs{0};
// Render-thread-only samples. Report periodically so a gameplay session can be
// diagnosed without a screenshot or a Present hook changing the render path.
struct FrameDiagnostics {
    std::array<double, 512> intervals{}, cpu{};
    size_t count = 0;
    uint64_t epoch = 0;
    std::chrono::steady_clock::time_point report{};
    void Record(uint64_t currentEpoch, double interval, double elapsed) {
        const auto now = std::chrono::steady_clock::now();
        if (epoch != currentEpoch || report.time_since_epoch().count() == 0) {
            epoch = currentEpoch;
            count = 0;
            report = now;
        }
        if (interval <= 0)
            return;
        intervals[count % intervals.size()] = interval;
        cpu[count++ % cpu.size()] = elapsed;
        if (now - report < std::chrono::seconds(5))
            return;
        const size_t n = std::min(count, cpu.size());
        auto stats = [n](std::array<double, 512> values) {
            std::sort(values.begin(), values.begin() + n);
            double mean = 0;
            for (size_t i = 0; i < n; ++i)
                mean += values[i];
            return std::array<double, 3>{mean / n, values[size_t((n - 1) * .95)], values[size_t((n - 1) * .99)]};
        };
        const auto c = stats(intervals), t = stats(cpu);
        const auto snapshot = service->Snapshot();
        const auto &m = snapshot.metrics;
        char text[768];
        snprintf(text, sizeof(text),
                 "NR timing: capture %ux%u, model %ux%u, observed %.1f fps; capture mean/p95/p99 %.2f/%.2f/%.2f ms; "
                 "callback CPU mean/p95/p99 %.2f/%.2f/%.2f ms; last GPU prepare/flow/model/compose "
                 "%.2f/%.2f/%.2f/%.2f ms; epoch processed %llu, cuts/resets %llu/%llu, "
                 "slot/summary CPU wait %.2f/%.2f ms; GPU priority capture/NR %d/%d; %s",
                 m.width, m.height, m.workingWidth, m.workingHeight, c[0] > 0 ? 1000 / c[0] : 0, c[0], c[1], c[2], t[0],
                 t[1], t[2], m.prepareMs, m.flowMs, m.modelMs, m.compositionMs, m.accepted, m.cuts,
                 m.resets, m.slotWaitMs, m.summaryWaitMs, m.captureGpuPriority, m.nrQueuePriority, snapshot.status.c_str());
        host->Log(EAM_LOG_INFO, text);
        count = 0;
        report = now;
    }
};
void ModuleAnchor() {}
std::wstring Wide(const char *p) {
    int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
    std::wstring v(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, v.data(), n);
    if (!v.empty())
        v.pop_back();
    return v;
}
std::string Utf8(const std::wstring &p) {
    int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string v(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, v.data(), n, nullptr, nullptr);
    if (!v.empty())
        v.pop_back();
    return v;
}
float Number(const char *key, float value, float low, float high) {
    char def[48];
    snprintf(def, sizeof(def), "%.5g", value);
    const float got = strtof(host->GetConfig(Id, key, def), nullptr);
    return std::isfinite(got) ? std::clamp(got, low, high) : value;
}
void Put(const char *key, double value) {
    char text[64];
    snprintf(text, sizeof(text), "%.8g", value);
    host->SetConfig(Id, key, text);
}
void Save() {
    Put("schema", 1);
    Put("enabled", enabled);
    Put("scalePercent", settings.scalePercent);
    Put("flowGrid", settings.flowGrid);
    Put("flowQuality", settings.flowQuality);
    Put("preset", settings.model.preset);
    Put("passes", settings.passes);
    Put("style", settings.model.style);
    Put("intensity", settings.model.intensity);
    Put("localStructure", settings.model.localStructure);
    Put("localTone", settings.model.localTone);
    Put("skinStructure", settings.model.skinStructure);
    Put("autoMask", settings.model.autoMask);
    Put("detail", settings.composition.detail);
    Put("colour", settings.composition.colour);
    Put("maxRatio", settings.composition.maxRatio);
    Put("whiteNits", settings.composition.whiteNits);
    Put("proxy", settings.composition.reversible);
    Put("reconstruction", settings.composition.reconstruction);
    Put("apply", settings.composition.apply);
    host->SetConfig(Id, "modelPath", modelPath);
    host->SaveConfig();
}
void Configure(bool rebuild) {
    settings.modelPath = Wide(modelPath);
    service->Configure(settings, enabled, rebuild);
    host->RequireRealFrameProcessing(enabled);
}
const char *Support(EamFrameSupport s) {
    switch (s) {
    case EamFrameSupport::Ready:
        return nullptr;
    case EamFrameSupport::Waiting:
        return "Waiting for a real capture";
    case EamFrameSupport::UnsupportedCapture:
        return "Capture paused: disable NR, then select WGC capture in Lossless Scaling";
    case EamFrameSupport::UnsupportedFrameGeneration:
        return "Capture paused: disable NR, then select LSFG 3 or turn frame generation off";
    case EamFrameSupport::UnsupportedScaler:
        return "Capture paused: disable NR, then select Scaling Off or NIS with FG off";
    case EamFrameSupport::UnrecognizedShader:
        return "Capture paused: unrecognized LS shader; disable NR to continue";
    case EamFrameSupport::UnsupportedTexture:
        return "Capture paused: unsupported texture; disable NR and use SDR or scRGB";
    case EamFrameSupport::SettingsChanging:
        return "Lossless Scaling settings are changing";
    case EamFrameSupport::UnsupportedRuntime:
        return "Capture paused: unsupported Lossless Scaling build; disable NR to continue";
    default:
        return "Capture unavailable";
    }
}
void Status(bool force = false) {
    if (!host || !service)
        return;
    const ULONGLONG now = GetTickCount64();
    if (!force && now - lastStatus.load() < 500)
        return;
    lastStatus = now;
    const auto snapshot = service->Snapshot();
    const char *support = Support(host->GetRealFrameSupport());
    const char *message = !snapshot.enabled ? "Disabled"
                          : snapshot.failed ? snapshot.status.c_str()
                          : support         ? support
                                            : snapshot.status.c_str();
    host->SetStatus(Id, message, snapshot.failed ? 3 : snapshot.ready && !support ? 1 : 0);
    const auto &m = snapshot.metrics;
    host->PublishMetric(Id, "prepare_ms", m.prepareMs, "ms");
    host->PublishMetric(Id, "flow_ms", m.flowMs, "ms");
    host->PublishMetric(Id, "model_ms", m.modelMs, "ms");
    host->PublishMetric(Id, "composition_ms", m.compositionMs, "ms");
    host->PublishMetric(Id, "summary_wait_ms", m.summaryWaitMs, "ms");
    host->PublishMetric(Id, "nr_total_ms", m.prepareMs + m.flowMs + m.modelMs + m.compositionMs, "ms");
    host->PublishMetric(Id, "callback_cpu_ms", lastCallbackMs.load(), "ms");
    host->PublishMetric(Id, "slot_wait_ms", m.slotWaitMs, "ms");
    host->PublishMetric(Id, "accepted", double(m.accepted), "frames");
    host->PublishMetric(Id, "model_passes", m.passes, "passes");
}
bool Frame(const EamRealFrame &frame, void *) {
    static std::chrono::steady_clock::time_point last;
    static uint64_t epoch = 0;
    static FrameDiagnostics diagnostics;
    const auto now = std::chrono::steady_clock::now();
    const double interval = epoch == frame.epoch && last.time_since_epoch().count()
                                ? std::chrono::duration<double, std::milli>(now - last).count()
                                : 0;
    if (host && interval > 0)
        host->PublishMetric(Id, "frame_ms", interval, "ms");
    last = now;
    epoch = frame.epoch;
    const bool result = service && service->Process(frame);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
    lastCallbackMs = cpuMs;
    if (host && service)
        diagnostics.Record(frame.epoch, interval, cpuMs);
    Status();
    return result;
}
bool Slider(const char *label, float *value, float low, float high) {
    return ImGui::SliderFloat(label, value, low, high, "%.2f");
}
} // namespace
EAM_EXPORT void AddonInitialize(IHost *supplied, ImGuiContext *context, void *alloc, void *freeFn, void *user) {
    ImGui::SetCurrentContext(context);
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(alloc), reinterpret_cast<ImGuiMemFreeFunc>(freeFn),
                                 user);
    eam::ui::InitAddonImGui();
    host = supplied;
    if (host->GetHostVersion() < 0x010400) {
        host->SetStatus(Id, "Manager API 1.4 or newer is required", 3);
        return;
    }
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    auto folder = std::filesystem::path(path).parent_path();
    settings.forwarderPath = (folder / L"NeuralRendering_nvngx.dll").wstring();
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    settings.cachePath = (std::filesystem::path(local) / L"LSAddonManager" / L"DLSSNR02" / L"cache").wstring();
    GetModuleFileNameW(nullptr, path, 32768);
    auto model = std::filesystem::path(path).parent_path() / L"nvngx_dlssnr.dll";
    const std::string defaultPath = Utf8(model.wstring());
    snprintf(modelPath, sizeof(modelPath), "%s", host->GetConfig(Id, "modelPath", defaultPath.c_str()));
    enabled = Number("enabled", 1, 0, 1) != 0;
    settings.scalePercent = unsigned(Number("scalePercent", 100, 25, 100));
    settings.flowGrid = unsigned(Number("flowGrid", 4, 1, 4));
    if (settings.flowGrid == 3)
        settings.flowGrid = 4;
    settings.flowQuality = unsigned(Number("flowQuality", 2, 1, 3));
    settings.model.preset = unsigned(Number("preset", 0, 0, 3));
    settings.passes = unsigned(Number("passes", 1, 1, neural::PipelineSettings::MaxPasses));
    settings.model.style = unsigned(Number("style", 0, 0, 2));
    const neural::ModelSettings modelDefaults;
    settings.model.intensity = Number("intensity", modelDefaults.intensity, 0, 2);
    settings.model.localStructure = Number("localStructure", modelDefaults.localStructure, 0, 2);
    settings.model.localTone = Number("localTone", modelDefaults.localTone, 0, 2);
    settings.model.skinStructure = Number("skinStructure", modelDefaults.skinStructure, -1, 2);
    settings.model.autoMask = Number("autoMask", 1, 0, 1) != 0;
    settings.composition.detail = Number("detail", 1, 0, 4);
    settings.composition.colour = Number("colour", 1, 0, 4);
    settings.composition.maxRatio = Number("maxRatio", 2, 1, 8);
    settings.composition.whiteNits = Number("whiteNits", 203, 80, 1000);
    settings.composition.reversible = unsigned(Number("proxy", 0, 0, 4));
    settings.composition.reconstruction = unsigned(Number("reconstruction", 1, 0, 1));
    settings.composition.apply = Number("apply", 1, 0, 1) != 0;
    service = std::make_unique<neural::Service>();
    if (!host->SetRealFrameCallback(Frame)) {
        service.reset();
        host->SetStatus(Id, "Another addon owns real-frame processing", 3);
        host->Log(EAM_LOG_ERROR, "NR could not acquire the real-frame stage");
        return;
    }
    Configure(true);
    host->Log(EAM_LOG_INFO, "DLSS NR started: original capture -> NVIDIA flow -> NR -> composition -> LSFG");
    Status(true);
}
EAM_EXPORT void AddonShutdown() {
    if (host) {
        if (service) {
            service->Configure(settings, false, false);
            host->RequireRealFrameProcessing(false);
        }
        host->SetRealFrameCallback(nullptr);
        service.reset();
        host->SetStatus(Id, "", 0);
    }
    host = nullptr;
}
EAM_EXPORT uint32_t GetAddonCapabilities() {
    return EAM_CAP_HAS_SETTINGS | EAM_CAP_D3D11_DEVICE_ACCESS | EAM_CAP_DISPATCH_HOOK;
}
EAM_EXPORT const char *GetAddonName() {
    return "DLSS Neural Rendering";
}
EAM_EXPORT const char *GetAddonVersion() {
    return "1.1.1";
}
EAM_EXPORT const char *GetAddonAuthor() {
    return "FriedSnowman";
}
EAM_EXPORT const char *GetAddonDescription() {
    return "NVIDIA neural rendering on original captures before LSFG, with independent NVIDIA optical flow.";
}
EAM_EXPORT void AddonRenderSettings() {
    if (!host || !service)
        return;
    bool changed = false, rebuild = false;
    if (ImGui::Checkbox("Neural rendering", &enabled)) {
        changed = true;
        rebuild = true;
    }
    ImGui::TextWrapped("Processes real captures before LSFG. Requires NVIDIA, WGC, and a user-provided "
                       "model. Every capture waits for processing; GPU load can reduce the frame rate. "
                       "Captures contain no game depth or motion data.");
    eam::ui::SectionLabel("Model file");
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##model", modelPath, sizeof(modelPath), ImGuiInputTextFlags_EnterReturnsTrue)) {
        changed = true;
        rebuild = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        wchar_t chosen[32768]{};
        auto current = Wide(modelPath);
        wcsncpy_s(chosen, current.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.lpstrFile = chosen;
        dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"NVIDIA NR runtime\0nvngx_dlssnr.dll\0DLL files\0*.dll\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            snprintf(modelPath, sizeof(modelPath), "%s", Utf8(chosen).c_str());
            changed = true;
            rebuild = true;
        }
    }
    ImGui::TextDisabled("The model DLL is not included.");
    eam::ui::SectionLabel("Quality and performance");
    int scale = int(settings.scalePercent);
    if (ImGui::SliderInt("Working resolution", &scale, 25, 100, "%d%%")) {
        settings.scalePercent = unsigned(scale);
        rebuild = true;
    }
    ImGui::TextDisabled("Percentage per axis; no hidden resolution cap.");
    int grid = settings.flowGrid == 1 ? 0 : settings.flowGrid == 2 ? 1 : 2;
    if (ImGui::Combo("Flow grid", &grid, "1 pixel\0 2 pixels\0 4 pixels (default)\0")) {
        settings.flowGrid = 1u << grid;
        rebuild = true;
    }
    int quality = int(settings.flowQuality) - 1;
    if (ImGui::Combo("Flow quality", &quality, "Slow / higher quality\0Medium (default)\0Fast\0")) {
        settings.flowQuality = quality + 1;
        rebuild = true;
    }
    eam::ui::SectionLabel("Model controls");
    if (ImGui::Button("Restore defaults")) {
        settings.model = neural::ModelSettings{};
        settings.composition = neural::CompositionSettings{};
        settings.passes = 1;
        changed = true;
        rebuild = true;
    }
    int preset = int(settings.model.preset);
    if (ImGui::Combo("Model", &preset, "Default\0Original (A)\0Fast (B)\0Preset 3\0")) {
        settings.model.preset = preset;
        rebuild = true;
    }
    int style = int(settings.model.style);
    if (ImGui::Combo("Style", &style, "Default\0Natural\0Cinematic\0")) {
        settings.model.style = style;
        rebuild = true;
    }
    rebuild |= Slider("Intensity", &settings.model.intensity, 0, 2);
    rebuild |= Slider("Local structure", &settings.model.localStructure, 0, 2);
    rebuild |= Slider("Local tone", &settings.model.localTone, 0, 2);
    rebuild |= Slider("Skin structure", &settings.model.skinStructure, -1, 2);
    ImGui::TextDisabled("Skin -1 follows local structure.");
    rebuild |= ImGui::Checkbox("Automatic skin detection", &settings.model.autoMask);
    ImGui::TextWrapped("Detects skin so Skin structure can adjust its detail separately.");
    int passes = int(settings.passes);
    if (ImGui::SliderInt("Multipass", &passes, 1, neural::PipelineSettings::MaxPasses,
                         passes == 1 ? "Off" : "%d", ImGuiSliderFlags_AlwaysClamp)) {
        settings.passes = unsigned(passes);
        rebuild = true;
    }
    ImGui::TextWrapped("Do not use multipass other than to make bad memes.");
    if (ImGui::CollapsingHeader("Composition (advanced)")) {
        changed |= Slider("Detail", &settings.composition.detail, 0, 4);
        changed |= Slider("Colour", &settings.composition.colour, 0, 4);
        changed |= Slider("Maximum luminance ratio", &settings.composition.maxRatio, 1, 8);
        int proxy = int(settings.composition.reversible);
        if (ImGui::Combo("HDR proxy", &proxy,
                         "Knee (default)\0Neutwo composed\0Neutwo replace\0Hybrid composed\0Hybrid replace\0")) {
            settings.composition.reversible = proxy;
            rebuild = true;
        }
        rebuild |= ImGui::SliderFloat("scRGB reference white", &settings.composition.whiteNits, 80, 1000, "%.0f nits");
        int reconstruction = int(settings.composition.reconstruction);
        if (ImGui::Combo("Reconstruction", &reconstruction, "Classic\0Matched residual (default)\0")) {
            settings.composition.reconstruction = reconstruction;
            changed = true;
        }
        changed |= ImGui::Checkbox("Apply model result", &settings.composition.apply);
        ImGui::TextWrapped(
            "Detail and colour default to 1. Replace proxies use the model image directly. Apply-off keeps "
            "processing active for comparison. Working resolution and model changes rebuild the pipeline "
            "after release; capture waits during rebuilding.");
    }
    if (ImGui::Button("Retry initialization")) {
        rebuild = true;
        changed = true;
    }
    rebuildPending |= rebuild;
    if (changed || rebuild)
        Save();
    if (changed && !rebuildPending)
        Configure(false);
    if (rebuildPending && !ImGui::IsAnyItemActive()) {
        Configure(true);
        rebuildPending = false;
    }
    Status(true);
    const auto snapshot = service->Snapshot();
    const auto &m = snapshot.metrics;
    eam::ui::SectionLabel("Pipeline status");
    const char *support = Support(host->GetRealFrameSupport());
    ImGui::TextWrapped("%s", support ? support : snapshot.status.c_str());
    if (support && snapshot.failed)
        ImGui::TextWrapped("%s", snapshot.status.c_str());
    ImGui::Text("Capture %u x %u; model %u x %u", m.width, m.height, m.workingWidth, m.workingHeight);
    if (snapshot.ready && m.passes > 1)
        ImGui::Text("Multipass: %u passes; model timing includes the full chain", m.passes);
    ImGui::Text("NR GPU time %.2f ms; callback CPU %.2f ms", m.prepareMs + m.flowMs + m.modelMs + m.compositionMs,
                lastCallbackMs.load());
    ImGui::TextWrapped("GPU: prepare %.2f / flow wait+convert %.2f / model %.2f / compose %.2f ms", m.prepareMs,
                       m.flowMs, m.modelMs, m.compositionMs);
    ImGui::TextWrapped("CPU wait: frame slot %.2f ms / scene summary %.2f ms", m.slotWaitMs, m.summaryWaitMs);
    ImGui::Text("Epoch: %llu processed / %llu cuts / %llu resets", m.accepted, m.cuts, m.resets);
}
