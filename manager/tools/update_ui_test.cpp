// Drives the real consent and settings controls offscreen, with a fake network.
#include "tools/ui_shot.h"
#include "src/config/config_manager.h"
#include "src/gui/gui_style.h"
#include "src/gui/widgets/update_notice.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <thread>

using namespace eam;
static int failed = 0;
static void Check(const char* label, bool ok) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failed;
}

int main(int argc, char** argv) {
    const auto out = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "eam_update_ui";
    std::filesystem::create_directories(out);
    const auto path = (out / "config.json").wstring();
    auto& cfg = ConfigManager::Instance();
    cfg.Load(path);
    cfg.Replace({{"global", nlohmann::json::object()}});
    std::atomic<int> requests{0};
    updates::Checker checker([&](const std::atomic<bool>&) {
        ++requests;
        return R"({"name":"Build 99","tag_name":"build-99","draft":false,"prerelease":false})";
    });
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LoadUiFonts("", "");
    ApplyUiScale(1.0f);
    UiShot shot;
    if (!shot.Init(760, 480)) return 2;
    auto frame = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Update test", nullptr, ImGuiWindowFlags_NoDecoration);
        widgets::RenderUpdateSettings(checker);
        const auto result = checker.Get();
        if (result.status == updates::Status::Available)
            ImGui::SetCursorPosY(480 - widgets::UpdateBannerHeight(result) - ImGui::GetStyle().WindowPadding.y);
        widgets::RenderUpdateBanner(result);
        widgets::RenderUpdateConsent(checker);
        ImGui::End();
    };
    auto activate = [&](const char* window, const char* label) {
        auto* w = ImGui::FindWindowByName(window);
        if (!w) { Check("control window exists", false); return; }
        ImGui::ActivateItemByID(w->GetID(label));
        shot.Frame(frame);
    };
    auto wait = [&] {
        for (int i = 0; i < 3000 && checker.Get().status == updates::Status::Checking; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    widgets::InitializeUpdateCheck(checker);
    shot.Frame(frame);
    Check("first startup asks without networking", checker.GetPolicy() == updates::Policy::Ask && requests == 0 && ImGui::GetTopMostPopupModal());
    shot.Save((out / "consent.bmp").string().c_str());
    activate("Automatic update checks", "No, stay offline");
    Check("No saves false and stays offline", cfg.GlobalGet("updates", "check") == false && requests == 0);
    cfg.Load(path);
    widgets::InitializeUpdateCheck(checker);
    shot.Frame(frame);
    Check("declined policy survives startup without another prompt", checker.GetPolicy() == updates::Policy::Disabled && !ImGui::GetTopMostPopupModal());

    cfg.GlobalSet("updates", "check", "invalid");
    widgets::InitializeUpdateCheck(checker);
    shot.Frame(frame);
    Check("invalid policy asks without networking", checker.GetPolicy() == updates::Policy::Ask && requests == 0 && ImGui::GetTopMostPopupModal());
    activate("Automatic update checks", "Yes, enable");
    wait();
    Check("Yes saves true and checks immediately", cfg.GlobalGet("updates", "check") == true && requests == 1 && checker.Get().status == updates::Status::Available);
    shot.Frame(frame);
    shot.Save((out / "build-banner.bmp").string().c_str());
    checker.SetPolicy(updates::Policy::Ask); // emulate a fresh process
    cfg.Load(path);
    widgets::InitializeUpdateCheck(checker);
    wait();
    shot.Frame(frame);
    Check("opt-in survives startup and checks without prompting", checker.GetPolicy() == updates::Policy::Enabled && requests == 2 && !ImGui::GetTopMostPopupModal());
    activate("Update test", "Automatically check GitHub for updates");
    Check("Settings disables checks and removes the banner", checker.GetPolicy() == updates::Policy::Disabled && cfg.GlobalGet("updates", "check") == false && widgets::DrawnUpdateBannerHeight() == 0);
    activate("Update test", "Automatically check GitHub for updates");
    wait();
    Check("Settings enables checks immediately", checker.GetPolicy() == updates::Policy::Enabled && requests == 3);

    cfg.Replace(cfg.Snapshot(), true);
    activate("Update test", "Automatically check GitHub for updates");
    Check("pending settings import freezes the toggle", checker.GetPolicy() == updates::Policy::Enabled && requests == 3);
    shot.Frame([&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Generic banner", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::SetCursorPosY(480 - widgets::UpdateBannerHeight({updates::Status::Available}) - ImGui::GetStyle().WindowPadding.y);
        widgets::RenderUpdateBanner({updates::Status::Available});
        Check("generic release still has a banner", widgets::DrawnUpdateBannerHeight() > 0);
        ImGui::End();
    }, 3);
    shot.Save((out / "generic-banner.bmp").string().c_str());
    checker.SetPolicy(updates::Policy::Disabled);
    shot.Shutdown();
    ImGui::DestroyContext();
    printf("\nUPDATE UI TEST %s (%d failed)\n", failed ? "FAILED" : "PASSED", failed);
    return failed ? 1 : 0;
}
