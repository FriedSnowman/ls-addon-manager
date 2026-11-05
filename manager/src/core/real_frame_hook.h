#pragma once
#include "eam/real_frame.h"
#include <d3d11.h>
namespace eam { class HostImpl; }
namespace RealFrameHook {
void VerifyRuntime(HMODULE);
void SetSettings(int frameGeneration, int capture, int scaler, bool hdr);
void SettingsChanging();
void Before(eam::HostImpl*, ID3D11DeviceContext*, UINT, UINT, UINT);
void After(eam::HostImpl*, ID3D11DeviceContext*, UINT, UINT, UINT, bool executed);
EamFrameSupport Support();
void Shutdown();
}
