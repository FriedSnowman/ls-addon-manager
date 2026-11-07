#pragma once
#include "eam/real_frame.h"
#include <d3d11.h>
#include <string>
namespace eam { class HostImpl; }
namespace RealFrameHook {
void VerifyRuntime(HMODULE);
void SetSettings(int frameGeneration, int capture, int scaler, bool hdr);
void SettingsChanging();
void Before(eam::HostImpl*, ID3D11DeviceContext*, UINT, UINT, UINT);
void After(eam::HostImpl*, ID3D11DeviceContext*, UINT, UINT, UINT, bool executed);
EamFrameSupport Support();
// Observations from existing hooks only: no capture probes or additional hooks.
struct Diagnostics {
    std::string runtimeVersion = "unknown", adapter, driver, hookError, captureError;
    bool runtimeChecked = false, runtimeVerified = false, configured = false, hdr = false;
    int fg = 0, capture = 0, scaler = 0;
    uint64_t dispatches = 0, captures = 0, lastDispatch = 0, lastCapture = 0, settingsAt = 0;
    unsigned width = 0, height = 0, format = 0;
};
Diagnostics GetDiagnostics();
void HookFailed(const char* reason);
void ObserveDevice(ID3D11Device*);
void CallbackFailed();
void Shutdown();
}
