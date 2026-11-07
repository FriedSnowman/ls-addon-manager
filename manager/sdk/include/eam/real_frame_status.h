#pragma once
#include "real_frame.h"

// Keep the tested runtime and the human-readable guidance together. This is a
// supported version, not an online claim about the latest Steam release.
inline constexpr char EamSupportedLsVersion[] = "3.2.2.0";
inline constexpr char EamSupportedLsSha256[] = "626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb";

inline EamFrameSupport EamProfileSupport(bool verified, bool configured, int fg, int capture, int scaler) {
    if (!verified) return EamFrameSupport::UnsupportedRuntime;
    if (!configured) return EamFrameSupport::SettingsChanging;
    if (capture != 1) return EamFrameSupport::UnsupportedCapture;
    if (fg != 0 && fg != 1) return EamFrameSupport::UnsupportedFrameGeneration;
    if (fg == 0 && scaler != 0 && scaler != 3) return EamFrameSupport::UnsupportedScaler;
    return EamFrameSupport::Ready;
}

inline const char* EamSupportText(EamFrameSupport support) {
    switch (support) {
    case EamFrameSupport::Ready: return nullptr;
    case EamFrameSupport::Waiting: return "Waiting for a real capture; start scaling a game";
    case EamFrameSupport::UnsupportedCapture:
        return "Capture paused: DXGI/other capture is unsupported. Disable NR, select WGC, then restart scaling";
    case EamFrameSupport::UnsupportedFrameGeneration:
        return "Capture paused: unsupported frame generation. Disable NR, select LSFG 3 or FG Off, then restart scaling";
    case EamFrameSupport::UnsupportedScaler:
        return "Capture paused: this scaler is unsupported with FG Off. Disable NR, select Scaling Off or NIS, then restart scaling";
    case EamFrameSupport::UnrecognizedShader:
        return "Capture paused: LS compute shader could not be identified. Disable NR, restart LS; check the manager log";
    case EamFrameSupport::UnsupportedTexture:
        return "Capture paused: unsupported capture texture or allocation failed. Disable NR; use SDR or scRGB and check capture size/VRAM";
    case EamFrameSupport::SettingsChanging:
        return "Waiting for Lossless Scaling to finish applying settings; if stuck, disable NR and restart scaling";
    case EamFrameSupport::UnsupportedRuntime:
        return "Lossless Scaling version may be different. Use supported version 3.2.2.0 and repair the manager installation; disable NR to continue";
    case EamFrameSupport::HookUnavailable:
        return "Capture hook unavailable. Restart Lossless Scaling; repair the manager installation and check the log";
    case EamFrameSupport::CallbackFailed:
        return "Capture paused: the NR callback failed. Disable NR and restart Lossless Scaling; check the log";
    }
    return "Capture state unavailable; restart Lossless Scaling and check the log";
}
