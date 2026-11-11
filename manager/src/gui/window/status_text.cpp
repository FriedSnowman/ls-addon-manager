#include "status_text.h"
#include "../../../sdk/include/eam/version.h"
#include <cstdio>

namespace eam {
namespace window {

std::string StatusCounts(int total, int on) {
    return std::string(EAM_PRODUCT_SHORT "   |   " EAM_VERSION_STRING "   |   ") + std::to_string(total) + (total == 1 ? " addon, " : " addons, ") +
           std::to_string(on) + " on";
}

std::string WithLiveStatus(std::string counts, const std::string& who, const std::string& text) {
    if (!text.empty()) counts = who + ": " + text + "   |   " + counts;
    return counts;
}
std::string CaptureSummary(const RealFrameHook::Diagnostics& d) {
    std::string text=EAM_VERSION_STRING " | LS " + d.runtimeVersion;
    text+=d.runtimeChecked?(d.runtimeVerified?" (verified)":" (unverified)"):" (not checked)";
    if(!d.configured)return text+" | capture settings not yet applied";
    const auto capture=d.capture==1?"WGC":d.capture==0?"DXGI":"other";
    const auto fg=d.fg==0?"Off":d.fg==1?"LSFG 3":"unsupported";
    const auto scaler=d.scaler==0?"Off":d.scaler==3?"NIS":"other";
    char profile[160];snprintf(profile,sizeof profile," | %s (%d) | FG %s (%d) | Scaling %s (%d) | %s",capture,d.capture,fg,d.fg,scaler,d.scaler,d.hdr?"HDR":"SDR");
    return text+profile;
}
std::string MachineSummary(const RealFrameHook::Diagnostics& d,const SystemStats::Snapshot& s,const GpuStats::Snapshot& g) {
    std::string text="LS GPU: "+(d.adapter.empty()?std::string("not observed yet"):d.adapter);
    if(!d.driver.empty())text+=" | adapter driver "+d.driver;
    if(!g.driver.empty())text+=" | NVIDIA driver "+g.driver;
    text+=" | CPU: "+(s.cpuName.empty()?std::string("unknown"):s.cpuName);
    char reading[192];
    if(s.ok) { snprintf(reading,sizeof reading," | CPU %.0f%% | RAM %.1f/%.1f GB",s.cpuPercent,s.ramUsedMB/1024.0,s.ramTotalMB/1024.0);text+=reading; }
    if(g.ok&&g.ageSeconds<4) {
        snprintf(reading,sizeof reading," | GPU 0 load %u%% | VRAM %.1f/%.1f GB | %u C",g.utilGpu,g.vramUsedMB/1024.0,g.vramTotalMB/1024.0,g.tempC);text+=reading;
        text+=" | sampled GPU 0: "+g.name;
        if(g.deviceCount>1)text+=" (multiple GPUs; load is not necessarily the LS GPU)";
    } else text+=" | GPU load unavailable"+(g.why.empty()?std::string():" ("+g.why+")");
    return text;
}
std::string CaptureObservation(const RealFrameHook::Diagnostics& d,uint64_t now) {
    if(!d.hookError.empty())return d.hookError;
    if(!d.captureError.empty())return d.captureError;
    if(!d.configured)return "No applied capture profile observed; start scaling a game";
    if(!d.dispatches)return "No LS compute dispatch observed; scaling may be idle or capture has not started";
    if(!d.captures)return std::to_string(d.dispatches)+" LS compute dispatches observed | no neural capture callbacks recorded";
    char text[200];snprintf(text,sizeof text,"%llu LS dispatches, %llu capture callbacks | last capture %.1f s ago | texture %ux%u, DXGI format %u",
        d.dispatches,d.captures,d.lastCapture&&now>=d.lastCapture?double(now-d.lastCapture)/1000:0,d.width,d.height,d.format);
    return text;
}

} // namespace window
} // namespace eam
