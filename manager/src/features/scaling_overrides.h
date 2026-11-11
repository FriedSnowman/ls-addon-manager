#pragma once
#include <string>
#include <windows.h>

namespace eam::features::scaling {

inline constexpr const char* kId = "LosslessScalingOverrides";
enum class FpsCorner { TopLeft, TopRight, BottomLeft, BottomRight };

void Start();
void Stop();
FpsCorner GetFpsCorner();
void SetFpsCorner(FpsCorner corner);
std::string Status();
void RenderOptions();

// Called on the GUI thread, outside DllMain, before scaling starts. Only text drawn
// by this module is eligible; other Direct2D users in the process are untouched.
bool InstallHooks(HMODULE lossless);

} // namespace eam::features::scaling
