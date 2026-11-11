#include "main_frame.h"
#include "status_text.h"
#include "../tabs/tab_about.h"
#include "../tabs/tab_addons.h"
#include "../tabs/tab_features.h"
#include "../tabs/tab_logs.h"
#include "../tabs/tab_performance.h"
#include "../tabs/tab_settings.h"
#include "../widgets/status_bar.h"
#include "../widgets/header_bar.h"
#include "../widgets/update_notice.h"
#include "../../addon/addon_manager.h"
#include "../../host/metrics.h"
#include "eam/real_frame_status.h"
#include "imgui.h"

namespace eam {
namespace window {

namespace {

widgets::StatusBarInfo BuildStatusLine(AddonManager* manager) {
    int total = 0, on = 0;
    if (manager) for (const auto& addon : manager->GetAddons()) { ++total; if (addon.enabled) ++on; }
    widgets::StatusBarInfo info;
    const Metrics::Status live = Metrics::Instance().BestStatus();   // the most relevant live status of any addon
    std::string who = live.addon;
    if (manager) for (const auto& addon : manager->GetAddons()) if (addon.id == live.addon) who = addon.GetDisplayName();
    const auto split=live.text.find('\n');
    info.reason=live.text.empty()?"Ready; start scaling a game":who+": "+live.text.substr(0,split);
    info.level=live.level;
    if(split!=std::string::npos) {
        const auto extra=live.text.find('\n',split+1);
        info.stats=live.text.substr(split+1,extra==std::string::npos?extra:extra-split-1);
        if(extra!=std::string::npos) info.details=live.text.substr(extra+1)+"\n";
    }
    // Loader/API/dependency failures have no running addon to publish a heartbeat.
    if(manager&&info.level<3)for(const auto& addon:manager->GetAddons())if(addon.enabled&&(addon.faulted||!addon.IsLoaded())) {
        info.reason=addon.GetDisplayName()+": "+(addon.errorMessage.empty()?(addon.RequiresRestart()?"Restart Lossless Scaling to load this addon":"Addon is not loaded; check Logs and repair its installation"):addon.errorMessage);
        info.level=3;break;
    }
    static RealFrameHook::Diagnostics capture;
    static std::string hardware;
    static ULONGLONG sampled=0;
    const auto now=GetTickCount64();
    if(!sampled||now-sampled>=500) {
        sampled=now;capture=RealFrameHook::GetDiagnostics();
        hardware=MachineSummary(capture,SystemStats::Instance().Get(),GpuStats::Instance().Get());
    }
    if(live.addon=="DLSSNR02"&&live.text.rfind("Disabled",0)!=0&&info.level<2&&RealFrameHook::Support()==EamFrameSupport::Waiting&&capture.dispatches&&capture.captures==0&&now-capture.settingsAt>=3000) {
        info.reason="NR: "+CaptureObservation(capture,now)+"; restart scaling with WGC and a supported profile";info.level=2;
    }
    if(live.addon=="DLSSNR02"&&live.text.rfind("Disabled",0)!=0&&info.level<2&&RealFrameHook::Support()==EamFrameSupport::SettingsChanging&&capture.settingsAt&&now-capture.settingsAt>=3000)
        info.level=2;
    if(live.addon=="DLSSNR02"&&live.text.rfind("Disabled",0)!=0&&RealFrameHook::Support()==EamFrameSupport::UnsupportedTexture&&!capture.captureError.empty())
        info.reason="NR: "+capture.captureError+". Disable NR; use SDR/scRGB or restart scaling";
    info.details+=CaptureSummary(capture)+"\n"+CaptureObservation(capture,now)+"\n"+StatusCounts(total,on)+"\nGPU timings are the last completed samples; capture fps is not game/display fps. Zero dimensions/timings mean no completed measurement yet.";
    info.stats=(info.stats.empty()?"":info.stats+" | ")+CaptureSummary(capture);
    info.hardware=hardware;
    return info;
}

void RenderTabs(AddonManager* manager, bool& bringAddonsForward) {
    if (!ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) return;

    if (ImGui::BeginTabItem("Addons", nullptr, bringAddonsForward ? ImGuiTabItemFlags_SetSelected : 0)) {
        bringAddonsForward = false;
        ImGui::Dummy(ImVec2(0, 5));
        RenderTabAddons(manager);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Features")) { RenderTabFeatures(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Performance")) { RenderTabPerformance(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Settings")) { RenderTabSettings(manager); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Logs")) {
        ImGui::Dummy(ImVec2(0, 5));
        RenderTabLogs();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("About")) { RenderTabAbout(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

} // namespace

void RenderMainFrame(AddonManager* manager, bool& bringAddonsForward) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    // The footer owns its vertical padding down to the window edge. Preserve the
    // header's top inset without adding a second bottom inset below the footer.
    const auto padding=ImGui::GetStyle().WindowPadding;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(padding.x,0));
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::SetCursorPosY(padding.y);

    // the header, fixed; then everything but the status bar along the bottom, which scrolls when a tab is long
    widgets::HeaderBar();
    ImGui::Dummy(ImVec2(0, 4));
    const auto status=BuildStatusLine(manager);
    const auto update = updates::Instance().Get();
    const float updateHeight = widgets::UpdateBannerHeight(update);
    const float contentHeight=ImGui::GetContentRegionAvail().y-widgets::StatusBarHeight(status.level)-ImGui::GetStyle().ItemSpacing.y
        - (updateHeight > 0 ? updateHeight + ImGui::GetStyle().ItemSpacing.y : 0);
    ImGui::BeginChild("##content", ImVec2(0, contentHeight>1?contentHeight:1), false, ImGuiWindowFlags_NoBackground);
    RenderTabs(manager, bringAddonsForward);
    ImGui::EndChild();

    widgets::RenderUpdateBanner(update);
    widgets::StatusBar(status);
    widgets::RenderUpdateConsent(updates::Instance());
    ImGui::End();
}

} // namespace window
} // namespace eam
