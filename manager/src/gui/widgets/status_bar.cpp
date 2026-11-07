#include "status_bar.h"
#include "imgui.h"
#include "eam/widgets.h"

namespace eam {
namespace widgets {

namespace { float drawnHeight = 0; }
float StatusBarHeight(int level) {
    const float content=ImGui::GetTextLineHeight()+ImGui::GetTextLineHeightWithSpacing()*(level>=2?3:0);
    return content+ImGui::GetStyle().FramePadding.y*2;
}
float DrawnStatusBarHeight() { return drawnHeight > 0 ? drawnHeight : StatusBarHeight(); }

void StatusBar(const std::string& left) {
    StatusBar(StatusBarInfo{left});
}

void StatusBar(const StatusBarInfo& info) {
    const bool expanded = info.level >= 2;
    const float h = StatusBarHeight(info.level);
    drawnHeight = h;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(origin.x, origin.y), ImVec2(origin.x + w, origin.y), eam::ui::theme::U(eam::ui::theme::kBorder), 1.0f);

    using namespace eam::ui::theme;
    const float line=ImGui::GetTextLineHeightWithSpacing();
    const float contentHeight=ImGui::GetTextLineHeight()+line*(expanded?3:0);
    const float textY=origin.y+(h-contentHeight)*0.5f;
    const float buttons=ImGui::CalcTextSize("Details   Copy").x+ImGui::GetStyle().FramePadding.x*4+12;
    const float room=(w-buttons)>20?w-buttons:20;
    const std::string full=info.reason+"\n"+info.stats+"\n"+info.hardware+(info.details.empty()?"":"\n"+info.details);
    const float reasonHeight = line * (expanded ? 2 : 1);
    const ImVec4 reasonClip(origin.x,textY,origin.x+room,textY+reasonHeight);
    std::string reason=info.reason;
    // Normal statuses stay on one line even if another addon publishes newlines.
    if(!expanded)for(char& c:reason)if(c=='\n'||c=='\r')c=' ';
    auto popCharacter=[](std::string& text) {
        size_t cut=text.size()-1;
        while(cut&&(static_cast<unsigned char>(text[cut])&0xc0)==0x80)--cut;
        text.resize(cut);
    };
    auto fitsReason=[&](const std::string& text) {
        return expanded?ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),FLT_MAX,room-4,text.c_str()).y<=reasonHeight
                       :ImGui::CalcTextSize(text.c_str()).x<=room-4;
    };
    if(!fitsReason(reason)) {
        while(!reason.empty()&&!fitsReason(reason+"..."))popCharacter(reason);
        reason+="...";
    }
    dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(origin.x+2,textY),
        U(info.level>=3?kDanger:info.level==2?kWarn:info.level==1?kAccent:kText),reason.c_str(),nullptr,expanded?room-4:0,&reasonClip);
    auto row=[&](const std::string& text,float y) {
        // Explicit ellipsis, with the unabridged text available via Details/Copy.
        std::string shown=text;
        if(ImGui::CalcTextSize(shown.c_str()).x>w-4) {
            while(!shown.empty()&&ImGui::CalcTextSize((shown+"...").c_str()).x>w-4) {
                popCharacter(shown);
            }
            shown+="...";
        }
        dl->AddText(ImVec2(origin.x+2,y),U(kMuted),shown.c_str());
    };
    if(expanded) {
        row(info.stats,textY+line*2);
        row(info.hardware,textY+line*3);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x+w-buttons,textY));
    if(ImGui::SmallButton("Details"))ImGui::OpenPopup("Status details");
    ImGui::SameLine();
    if(ImGui::SmallButton("Copy"))ImGui::SetClipboardText(full.c_str());
    if(ImGui::BeginPopup("Status details")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+ImGui::GetFontSize()*52);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        if(ImGui::Button("Copy diagnostics"))ImGui::SetClipboardText(full.c_str());
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(w, h));
    if(ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*52);
        ImGui::TextUnformatted(full.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();
    }
}

} // namespace widgets
} // namespace eam
