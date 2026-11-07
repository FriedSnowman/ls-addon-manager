#pragma once
#include <string>
#include "../../core/real_frame_hook.h"
#include "../../host/system_stats.h"
#include "../../host/gpu_stats.h"

namespace eam {
namespace window {

// The left side of the status bar: "<product>   |   N addon(s), M on".
std::string StatusCounts(int total, int on);

// Puts the most relevant live status before the less important addon counts.
std::string WithLiveStatus(std::string counts, const std::string& who, const std::string& text);
std::string CaptureSummary(const RealFrameHook::Diagnostics&);
std::string MachineSummary(const RealFrameHook::Diagnostics&, const SystemStats::Snapshot&, const GpuStats::Snapshot&);
std::string CaptureObservation(const RealFrameHook::Diagnostics&, uint64_t now);

} // namespace window
} // namespace eam
