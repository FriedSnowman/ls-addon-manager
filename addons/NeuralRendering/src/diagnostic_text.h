// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
namespace neural {
inline std::string FailureGuidance(const std::string& error) {
    const auto has=[&](const char* word){return error.find(word)!=std::string::npos;};
    if(has("nvngx_dlssnr.dll was not found")) return "Select your nvngx_dlssnr.dll with Browse";
    if(has("NeuralRendering_nvngx.dll")) return "Repair/reinstall the matching manager and NR addon";
    if(has("NVIDIA adapter")||has("NVIDIA capture adapter")) return "Select a compatible NVIDIA GPU for Lossless Scaling";
    if(has("grid is unsupported")) return "Select Flow grid 4 pixels, then Retry initialization";
    if(has("resolution is outside")) return "Reduce the game/capture resolution (Working resolution does not reduce optical-flow input)";
    if(has("0x8007000E")) return "GPU/system allocation failed; reduce capture/working resolution and turn Multipass off";
    if(has("0x887A0005")||has("0x887A0006")||has("0x887A0007")||has("retirement failed")||has("device removed"))
        return "Restart Lossless Scaling; the graphics device was lost or could not finish safely";
    if(has("Optical Flow")||has("optical flow")||has("NVOFA"))
        return "Check NVIDIA GPU/driver optical-flow support; update the driver, then restart LS";
    if(has("NGX")||has("NR feature")||has("runtime")||has("Model parameter")||has("NR float"))
        return "Check the selected NR model and NVIDIA driver; retry initialization or select a compatible model";
    if(has("D3D12 device")) return "Check D3D12 support on the selected GPU/driver, then restart LS";
    if(has("shared")||has("Shared")||has("fence"))
        return "GPU sharing/synchronization failed; restart LS and check the selected GPU/driver";
    if(has("settings")||has("Invalid")) return "Restore NR defaults and retry; report this error if it persists";
    return "Retry initialization; if it persists, restart LS and copy these diagnostics";
}
}
