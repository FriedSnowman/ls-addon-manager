#include "update_notice.h"
#include "../gui_scale.h"
#include "../../config/config_manager.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <windows.h>
#include <shellapi.h>

namespace eam::widgets {
namespace {

float drawnBannerHeight = 0;

void SavePolicy(updates::Checker& checker, bool enabled) {
    auto& config = ConfigManager::Instance();
    if (config.RestartPending()) return;
    config.GlobalSet("updates", "check", enabled);
    config.Save();
    checker.SetPolicy(enabled ? updates::Policy::Enabled : updates::Policy::Disabled);
}

} // namespace

void InitializeUpdateCheck(updates::Checker& checker) {
    const auto saved = ConfigManager::Instance().GlobalGet("updates", "check");
    checker.SetPolicy(!saved.is_boolean() ? updates::Policy::Ask :
                      saved.get<bool>() ? updates::Policy::Enabled : updates::Policy::Disabled);
}

void RenderUpdateConsent(updates::Checker& checker) {
    if (checker.GetPolicy() != updates::Policy::Ask || ConfigManager::Instance().RestartPending()) return;
    constexpr const char* title = "Automatic update checks";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(420));
        ImGui::TextWrapped("Automatically check GitHub for updates?");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextWrapped("If not, the app is completely offline.");
        ImGui::TextWrapped("You can change this later in Settings.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ImGui::Button("Yes, enable")) { SavePolicy(checker, true); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("No, stay offline")) { SavePolicy(checker, false); ImGui::CloseCurrentPopup(); }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void RenderUpdateSettings(updates::Checker& checker) {
    eam::ui::SectionLabel("Updates");
    bool enabled = checker.GetPolicy() == updates::Policy::Enabled;
    ImGui::BeginDisabled(ConfigManager::Instance().RestartPending());
    if (ImGui::Checkbox("Automatically check GitHub for updates", &enabled)) SavePolicy(checker, enabled);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Checks at startup. Download opens the release page in your browser.");
    if (checker.Get().status == updates::Status::Checking) ImGui::TextDisabled("Checking for updates...");
    else if (checker.Get().status == updates::Status::Failed) ImGui::TextDisabled("Could not check for updates. Will try again next startup.");
}

float UpdateBannerHeight(const updates::Result& result) {
    return result.status == updates::Status::Available ? ImGui::GetFrameHeight() + S(12) : 0;
}

float DrawnUpdateBannerHeight() { return drawnBannerHeight; }

void RenderUpdateBanner(const updates::Result& result) {
    drawnBannerHeight = UpdateBannerHeight(result);
    if (!drawnBannerHeight) return;
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float inset = S(8);
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + drawnBannerHeight),
        eam::ui::theme::U(eam::ui::theme::kSelection), S(3));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + inset, origin.y + S(6)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(updates::BannerText(result).c_str());
    const float buttonWidth = ImGui::CalcTextSize("Download").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - buttonWidth - inset, origin.y + S(6)));
    if (ImGui::Button("Download")) ShellExecuteW(nullptr, L"open", updates::kReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, drawnBannerHeight));
}

} // namespace eam::widgets
