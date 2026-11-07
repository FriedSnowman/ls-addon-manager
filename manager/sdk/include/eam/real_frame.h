// API 1.3. A verified real capture, never an interpolated/presented frame.
#pragma once
#include <cstdint>
enum class EamFrameEncoding : uint32_t { Sdr709 = 0, ScRgb = 1 };
enum class EamFrameSupport : uint32_t {
    Waiting, Ready, UnsupportedCapture, UnsupportedFrameGeneration,
    UnsupportedScaler, UnrecognizedShader, UnsupportedTexture, SettingsChanging, UnsupportedRuntime
};
struct EamRealFrame {
    uint32_t size = sizeof(EamRealFrame);
    void* context = nullptr;       // ID3D11DeviceContext*, borrowed for this call
    void* original = nullptr;      // ID3D11Texture2D*, read-only, unenhanced
    void* output = nullptr;        // owned texture, initially an exact original copy
    uint32_t width = 0, height = 0;
    EamFrameEncoding encoding = EamFrameEncoding::Sdr709;
    uint64_t sequence = 0, epoch = 0;
};
// Render-thread callback. Queue GPU completion on context before returning true.
// Restore modified D3D11 state. False/fault preserves the original only when processing
// is not required (API 1.4 RequireRealFrameProcessing); otherwise capture pauses.
// Sequence counts admissions, not presents. Epoch changes with settings/device/size.
using EamRealFrameCallback = bool(*)(const EamRealFrame&, void*);
