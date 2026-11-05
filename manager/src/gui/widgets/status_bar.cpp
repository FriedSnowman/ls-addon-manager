#include "status_bar.h"
#include "imgui.h"
#include "eam/widgets.h"

namespace eam {
namespace widgets {

float StatusBarHeight() { return ImGui::GetFrameHeight() * 1.05f; }

void StatusBar(const std::string& left) {
    const float h = StatusBarHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(origin.x, origin.y), ImVec2(origin.x + w, origin.y), eam::ui::theme::U(eam::ui::theme::kBorder), 1.0f);

    // Status text uses the full bar; clip long live messages to the available width.
    const float ty = origin.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->PushClipRect(origin, ImVec2(origin.x + w, origin.y + h), true);
    dl->AddText(ImVec2(origin.x + 2.0f, ty), eam::ui::theme::U(eam::ui::theme::kMuted), left.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h));
}

} // namespace widgets
} // namespace eam
