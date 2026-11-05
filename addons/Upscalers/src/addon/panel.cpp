// The addon's settings panel, drawn by the manager in its window (the window's thread). It works on a copy of the settings and hands the copy
// back (Commit) when something changed. The sections start closed; the person opens what they need.
#include "addon/framegen11.h"
#include "engine/sr_engine.h"
#include "addon/state.h"
#include "addon/present_hook.h"
#include "addon/screenshot.h"
#include "imgui.h"
#include <eam/widgets.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nr {

// Wrapped tooltip for the widget just submitted, after a short hover delay.
static void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    const char* hint = eam::ui::SliderHint();   // sliders add "default, double-click resets, Ctrl+scroll fine-tunes"
    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f); ImGui::TextUnformatted(text);
    if (hint) { ImGui::Dummy(ImVec2(0, 2)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); ImGui::TextUnformatted(hint); ImGui::PopStyleColor(); }
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
}
// Wrapped help text in the disabled colour (ImGui::TextDisabled does not wrap, so long lines ran off the panel).
static void Note(const char* fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", b);
    ImGui::PopStyleColor();
}
// Every collapsible section of the panel starts closed (eam::ui::SectionHeader with no second argument); the person opens what they need.
// A titled block of the panel: some room, a thin line, the title in the small capitals of the other apps, and a little room under it.
static void Block(const char* title, bool first = false) {
    const float u = ImGui::GetFontSize();
    if (!first) { ImGui::Dummy(ImVec2(0, u * 0.7f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, u * 0.5f)); }
    eam::ui::SectionLabel(title);
    ImGui::Dummy(ImVec2(0, u * 0.35f));
}

void DrawPanel() {
    Config c; { std::lock_guard<std::mutex> lk(g_settingsMutex); c = g_config; }
    bool changed = false, createChanged = false, tapChanged = false;
    // Every slider bound to a field of the params gets that field's default (a fresh PictureParams): a tick on the groove, a ring on the
    // knob while it differs, double-click to reset. Sliders bound to anything else simply have no default.
    static const PictureParams kDefaults;
    auto SL = [&](const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
        const char* base = (const char*)&c.p; const char* pv = (const char*)v; const float* def = nullptr;
        if (pv >= base && pv < base + sizeof(PictureParams)) def = (const float*)((const char*)&kDefaults + (pv - base));
        return eam::ui::SliderFloat(label, v, lo, hi, fmt, 0, def);
    };

    ImGui::TextWrapped("%s", Status().c_str());
    if (g_off && ImGui::Button("Retry")) SwitchOn();
    auto choices = RuntimeChoices();
    const auto chosen = ChosenRuntimeFile(); int current = 0;
    for (size_t i=0;i<choices.size();++i) if (choices[i].path==chosen) current=int(i);
    if (!choices.empty() && ImGui::BeginCombo("Runtime", choices[current].name.c_str())) {
        for (size_t i=0;i<choices.size();++i) if (ImGui::Selectable(choices[i].name.c_str(),current==int(i))) ChooseRuntimeFile(choices[i].path);
        ImGui::EndCombo();
    }
    if (!kAnyCardScaler) {
        const char* names[]={"Auto", "Default", "E", "J", "L", "M"}; const unsigned values[]={16,0,5,10,12,13}; int p=0;
        for(int i=0;i<6;++i) if(c.dlaaPreset==values[i]) p=i;
        if(ImGui::Combo("DLSS preset",&p,names,6)) { c.dlaaPreset=values[p]; changed=true; }
    }
    const char* motions[]={"Measured from frames", "LSFG flow", "None"};
    changed |= ImGui::Combo("Motion",&c.motionSource,motions,3);
    if (kScalerAddon && eam::ui::SectionHeader("Upscaling")) {
        const ScalerView v = GetScalerView();
        const char* const U = kUpscalerName;
        if (v.starting) ImGui::TextDisabled(kXessScaler ? "Loading Intel's XeSS runtime..." : kFsrScaler ? "Loading AMD's FSR runtime..." : "Loading NVIDIA's DLSS runtime...");
        else if (v.failed) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "%s could not run: %s. Lossless Scaling's NIS runs as usual.", U, v.error.c_str());
        else if (v.preparing) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "%s is getting ready for this frame format (the first frame can take a while); NIS runs meanwhile.", U);
        else if (!v.blocked.empty()) {   // the NIS pass is there but the upscaler cannot take it: say why, rather than look like it runs
            ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
            if (v.nisSeen) ImGui::TextWrapped("NIS pass found (%ux%u -> %ux%u), but %s is not replacing it: %s", v.inW, v.inH, v.outW, v.outH, U, v.blocked.c_str());
            else ImGui::TextWrapped("%s is not replacing NIS: %s", U, v.blocked.c_str());
            ImGui::PopStyleColor();
        }
        else if (!v.nisSeen) ImGui::TextWrapped("Waiting for Lossless Scaling's NIS pass. Choose NIS as the Scaling Type and scale a game that runs in a window smaller than the screen.");
        else if (!v.ready) ImGui::TextDisabled("NIS pass found (%ux%u -> %ux%u); %s is not running yet.", v.inW, v.inH, v.outW, v.outH, U);
        else {
            const std::string running = v.provider.empty() ? std::string(U) : std::string(U) + " " + v.provider;   // "FSR 4.1.1b"
            ImGui::TextWrapped("%s upscales %ux%u -> %ux%u (x%.2f) in place of NIS: %.2f ms a frame on the GPU (motion %.2f ms of it), %llu frames so far%s.", running.c_str(), v.inW, v.inH,
                               v.outW, v.outH, v.inW ? (float)v.outW / v.inW : 0.0f, v.gpuMs, v.motionMs, (unsigned long long)v.runs,
                               v.perFrame > 1 ? " (real and generated frames alike)" : "");
            if (v.second.valid) {
                ImGui::TextDisabled("Last second: %.0f pictures a second (%.0f%% in close pairs); %.1f%% shown twice, %.1f%% of frames not handed over, %.1f%% waited for on the GPU.",
                                    v.second.fps, v.second.closePct, v.second.repeatPct, v.second.skipPct, v.second.waitPct);
                Tip("Close pairs: two frames within 6 ms of each other, as adaptive frame generation makes them. Shown twice: the upscaler had not finished a newer picture "
                    "when Lossless Scaling needed one, so the one before was shown again (a small judder). Waited for on the GPU: rather than that, Lossless Scaling's "
                    "frame waited on the GPU for the next picture (the setting below). Not handed over: the upscaler still had two frames to do, so this one was "
                    "left out. A GPU running at its limit makes these rise: a frame cap with some headroom helps most.");
            }
            if (g_compare.load() == 2) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "Showing Lossless Scaling's NIS for comparison (Before / after hotkey).");
        }
        changed |= SL("Sharpening", &c.p.sharpen, 0.0f, 1.0f, c.p.sharpen <= 0.001f ? "off" : "%.2f");
        if (kFsrScaler) Tip("FSR's own sharpening (AMD's RCAS), part of its upscaling pass; above about 0.6 an extra pass adds more than RCAS can. Lossless Scaling's "
                            "NIS sharpens too (its Sharpness setting), so without it FSR can look softer next to NIS. 0.5 is a good start (Ctrl+Shift+F2 / F3 in the game).");
        else Tip("Contrast-adaptive sharpening of DLSS's picture (the FidelityFX CAS formula), which costs a fraction of a millisecond; above about 0.6 its effect is amplified "
                 "past CAS's own maximum. DLSS has no sharpening of its own, while Lossless Scaling's NIS does (its Sharpness setting), so without it DLSS can look softer "
                 "next to NIS. 0.5 is a good start (Ctrl+Shift+F2 / F3 in the game).");
        // FSR 4 keeps its history its own way and takes none of FSR 3.1's tuning (nor the mask the slider widens): the slider would do nothing
        const bool fsr4 = kFsrScaler && GetScalerView().provider.rfind("4", 0) == 0;
        if (fsr4) ImGui::BeginDisabled();
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("Stability", &c.scalerStability, 0.0f, 1.0f, c.scalerStability <= 0.001f ? "off" : "%.2f", 0, &off); }
        if (fsr4) { ImGui::EndDisabled(); Note("FSR 4 handles stability by itself: this slider is for FSR 3.1."); }
        { char tip[768];
          snprintf(tip, sizeof tip, "Less shimmer on thin lines, wires and leaves, at the cost of more trailing behind what moves. The game gives the upscaler no "
                   "sub-pixel camera shifts, so a line thinner than a pixel flickers from frame to frame; with this up, the motion measurement stops reporting that "
                   "flicker as a place not to trust the history, so %s averages it out over the frames before%s. Off: as before. Try 0.5, then move it while "
                   "you look at a fence or a power line. The game's own anti-aliasing (MSAA) is still what draws thin lines in the first place.",
                   U, kFsrScaler ? ", and FSR keeps more of its history and reacts less to small changes of shading" : "");
          Tip(tip); }
        {
            if (ImGui::Checkbox("Steady in fast motion", &c.scalerFastMotion)) changed = true;
            Tip("On (the default): where the picture moves fast (a quick camera turn) or the motion cannot be followed, the upscaler's picture leans on "
                "the new frame instead of the frames before. The game gives the upscaler no sub-pixel camera shifts, so its memory of earlier frames adds "
                "little detail there, and in a fast turn it trailed: leaves and edges with soft doubled outlines. Off: as before. Flip it while you turn "
                "the camera to compare.");
            if (c.scalerFastMotion) {
                if (ImGui::SliderFloat("Lean from", &c.scalerLeanFrom, 0.0f, 0.5f, c.scalerLeanFrom <= 0.0f ? "automatic" : "%.2f %% of the width a frame")) changed = true;
                Tip("How much motion the lean starts from (fully at twice it). Automatic (0): 0.1 % of the frame's width a frame at 1:1 (about 4 px at 4K), "
                    "0.05 % when upscaling. Lower: less trailing and smearing while you move, but at 1:1 the game's own jagged edges show more in motion. "
                    "Higher: more of the upscaler's smoothing in motion, and more trailing. Move it while you walk and turn.");
            }
        }
        {
            if (ImGui::Checkbox("Motion by shape (test)", &c.motionShapes)) changed = true;
            Tip("A test, off by default. The motion the upscaler is given is found by matching small patches of the frame before to this one. Off: by "
                "their brightness, as before. On: by their shape and edges, with the brightness itself counting little, so a flash, a spell effect, a "
                "fade or a shadow passing over should mislead it less. Flip it where the light changes while you move, and tell us which looks steadier.");
        }
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("Edge smoothing", &c.scalerEdges, 0.0f, 1.0f, c.scalerEdges <= 0.001f ? "off" : "%.2f", 0, &off); }
        Tip("Anti-aliasing along the edges of the upscaled picture: for games without anti-aliasing of their own (stair steps on roofs, fences and "
            "wires). It finds where the brightness steps, which way the edge runs and how far, and blends across it by the part of a pixel the true "
            "edge would cover. Costs a fraction of a millisecond. It also softens text a little. If the game has anti-aliasing (MSAA), that is "
            "better: use this where it has none.");
        {   // the picture controls: on the frame before it is upscaled; Neural Rendering's own take over while it is on
            const bool nrOn = false;

            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Vibrance", &c.p.vibrance, 0.0f, 1.0f, c.p.vibrance <= 0.001f ? "off" : "%.2f", 0, &d); }
            Tip("Lifts muted colours much more than vivid ones, so skin and already-strong colours are not pushed further: a natural colour boost.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Saturation", &c.p.saturation, 0.0f, 2.0f, std::abs(c.p.saturation - 1.0f) < 0.005f ? "unchanged" : "%.2f", 0, &d); }
            Tip("More colour everywhere (above 1) or less (below 1, 0 is black and white).");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Shadows", &c.p.shadows, -1.0f, 1.0f, std::abs(c.p.shadows) < 0.005f ? "unchanged" : "%+.2f", 0, &d); }
            Tip("Lifts (right) or deepens (left) the dark parts only: see into a dark interior or a night without greying the rest.");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Highlights", &c.p.highlights, -1.0f, 1.0f, std::abs(c.p.highlights) < 0.005f ? "unchanged" : "%+.2f", 0, &d); }
            Tip("Tames (left) or brightens (right) the bright parts only: a blown-out sky or snow gets detail back.");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Brightness", &c.p.brightness, -0.3f, 0.3f, "%+.2f", 0, &d); }
            Tip("Lifts or lowers the whole picture, as a monitor's brightness does. For a game that is too dark (or too bright) to see into.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Contrast", &c.p.contrast, 0.5f, 1.5f, "%.2f", 0, &d); }
            Tip("Spreads the tones apart (above 1) or draws them together (below 1), around mid-grey.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Gamma", &c.p.gamma, 0.5f, 2.0f, "%.2f", 0, &d); }
            Tip("Above 1 brightens the mid-tones and leaves black and white where they are: dark corners get visible without washing out the "
                "sky. Below 1 darkens them.");
        }
        {
            std::string game; { std::lock_guard<std::mutex> lk(g_settingsMutex); game = g_scalerGame; }
            if (ImGui::Checkbox("Keep these settings per game", &c.scalerPerGame)) changed = true;
            Tip("On (the default): sharpening, stability, edge smoothing, the colour and tone controls, the DLSS model and the motion are kept for each game, and come back when "
                "that game takes focus. A change made here counts for the game played last.");
            if (c.scalerPerGame) {
                ImGui::SameLine();
                ImGui::TextDisabled(game.empty() ? "(no game seen yet)" : "for %s", game.c_str());
                if (!c.scalerGames.empty()) {
                    int forget = -1;
                    if (ImGui::TreeNode("Games with settings of their own")) {
                        for (size_t i = 0; i < c.scalerGames.size(); ++i) {
                            const auto& [exe, p] = c.scalerGames[i];
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s: sharpening %.2f, stability %.2f, edges %.2f", exe.c_str(), p.sharpen, p.stability, p.edges);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Forget")) forget = static_cast<int>(i);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (forget >= 0) { c.scalerGames.erase(c.scalerGames.begin() + forget); changed = true; }
                }
            }
        }
        if (kFrameGen) {   // a prototype: only in a build made with it (product.h)
            if (ImGui::Checkbox("Frame generation of our own (prototype)", &c.frameGen)) changed = true;
            Tip("Doubles the frame rate with a frame made between each two real ones, shown in Lossless Scaling's own window. Turn Lossless Scaling's frame "
                "generation OFF first (both at once would double twice). A prototype: the frame between is made by AMD's FSR 3.1 frame generation with the "
                "motion measured from the frames (when it cannot, the two real frames mixed half and half). Real frames are shown half a frame later than without it.");
            if (c.frameGen) {
                if (ImGui::Checkbox("  Keep the character clean in turns", &c.frameGenGuard)) changed = true;
                Tip("On (the default): where the two real frames agree and hardly move (a character the camera follows, a HUD) but the frame made "
                    "between shows something else, the real frames' own picture goes there. In a quick camera turn, FSR pastes the scene sweeping past "
                    "over the character (leaves on the hair); this takes most of it back. Off: FSR's frame as it made it.");
                const nr::framegen::Stats st = nr::framegen::GetStats();
                ImGui::TextDisabled("  %llu real, %llu made between (%llu by %s, %.1f ms each); real frames every %.1f ms", (unsigned long long)st.real,
                                    (unsigned long long)st.generated, (unsigned long long)st.byFsr, st.engine.empty() ? "FSR" : st.engine.c_str(), st.fsrMs, st.realIntervalMs);
            }
        }
        if (ImGui::Checkbox("Wait on the GPU rather than repeat a picture", &c.scalerGpuWait)) changed = true;
        Tip("When the upscaler has not finished a new picture by the time Lossless Scaling needs one (two frames close together, as adaptive frame generation makes "
            "them), the picture before would be shown again: a small judder. On (the default), Lossless Scaling's frame waits on the GPU for the new picture instead, "
            "as a game with DLSS built in waits for DLSS; the CPU never waits. Off: the picture before is shown again. Compare the line above with it on and off.");
        Note("Compare with the Before / after hotkey (Compare and hotkeys): it switches between %s and Lossless Scaling's own NIS while you play.", U);
        {   // HDR games: the frames are upscaled as light in the upscaler's HDR mode and put back in their own encoding (scaler11.h, SetEncoding)
            static const char* const encodings[] = { "Automatic", "SDR", "HDR" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Frame encoding", &c.frameEncoding, encodings, 3)) changed = true;
            Tip("For HDR games. Automatic (the default) works it out: 8-bit frames are SDR, 16-bit float ones HDR (scRGB), and 10-bit ones HDR (HDR10) "
                "when Windows runs the display in HDR. Set SDR or HDR only if the picture comes out washed out, too dark or too bright. "
                "Technical status shows what was decided.");
        }
    }
    if (kScalerAddon && eam::ui::SectionHeader("Compare and hotkeys")) {   // the upscalers: the upscaled picture or NIS's, and the sharpening
        int cm = g_compare.load() == 2 ? 1 : 0; const char* cms[] = { "Upscaled", "Lossless Scaling's NIS (before)" };
        if (ImGui::Combo("Compare view", &cm, cms, 2)) g_compare = cm == 1 ? 2 : 0;
        Tip("Upscaled = normal. Lossless Scaling's NIS = as if the addon were off, to compare (the upscaler stays loaded, so switching back is instant). Not saved.");
        changed |= ImGui::Checkbox("Hotkeys: Ctrl+Shift + key (work while the game has focus and the upscaler runs)", &c.hotkeys);
        Tip("Switch between the upscaler and NIS, and change the sharpening, from inside the game. The Ctrl+Shift pair keeps them away from the game's own key bindings.");
        auto fkey = [&](const char* label, int* vk) {
            int idx = *vk - VK_F1; if (idx < 0 || idx > 11) idx = 6;
            const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
            if (ImGui::Combo(label, &idx, names, 12)) { *vk = VK_F1 + idx; changed = true; }
        };
        fkey("Before / after", &c.keyAB); fkey("Sharpen -", &c.keySharpDn); fkey("Sharpen +", &c.keySharpUp); fkey("Save the recording", &c.keyRecord);
        fkey("Before / after pictures", &c.keyShot);
        auto fname = [](int vk) { static char b[4][8]; static int n = 0; char* o = b[n++ & 3]; snprintf(o, 8, "F%d", vk - VK_F1 + 1); return (const char*)o; };
        Note("Now: Ctrl+Shift+%s before/after  |  %s / %s sharpen - / +  |  %s save the recording  |  %s before/after pictures  (%s)", fname(c.keyAB),
             fname(c.keySharpDn), fname(c.keySharpUp), fname(c.keyRecord), fname(c.keyShot), c.hotkeys ? "hotkeys on" : "hotkeys OFF: tick the box above");
        if (ImGui::SmallButton("Save before / after pictures")) RequestPair();
        Tip("Two PNGs of the same moment: the upscaled picture and Lossless Scaling's NIS (one frame of NIS shows on screen while it is taken). "
            "Windows' own screenshots show the game's window, not Lossless Scaling's picture. HDR games are saved in their SDR view.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open the pictures folder")) ShellExecuteW(nullptr, L"open", screenshot::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        { bool ok; const std::string msg = screenshot::LastResult(ok);
          if (!msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopStyleColor(); } }
    }
    if (eam::ui::SectionHeader("Recording (for bug reports and tests)")) {
        Note(kScalerAddon ? "Keeps the last few seconds of the frames the upscaler receives (Lossless Scaling's frame as it goes to the upscaler, with the colour and tone settings), losslessly, ready to "
                            "save as a .lsrec file. Send one with a bug report and the problem can be replayed and fixed on another computer."
                          : "Keeps the last few seconds of the frames Neural Rendering receives (the game's frames before the model or anything else changes "
                            "them), losslessly, ready to save as a .lsrec file. Send one with a bug report and the problem can be replayed and fixed on "
                            "another computer.");
        if (ImGui::Checkbox("Keep the last few seconds ready to save", &c.recordOn)) {
            changed = true;
            g_recorder.Configure(c.recordOn, c.recordSeconds, static_cast<uint32_t>(c.recordBudgetMb));   // off: its memory goes now, not at the next frame
        }
        Tip("Off (the default) costs nothing. On: every frame is copied off the graphics card and compressed on a few background threads, which "
            "takes some processor time and the memory below. Nothing waits for it: a frame that comes while all the copies are busy is left out.");
        if (!c.recordOn) ImGui::BeginDisabled();
        if (kFrameGen) {
            if (ImGui::Checkbox("Record what is shown (with frame generation of our own)", &c.recordShown)) changed = true;
            Tip("With frame generation of our own on: the frames as they go to the screen, the frames made between and the real ones (after the upscaler, "
                "at the screen's size), each marked which it is, instead of the frame going to the upscaler. For showing what frame generation and the "
                "upscaler do in motion. The frames are big (4K), so fewer seconds fit: raise the memory, or keep 2 to 3 seconds.");
        }
        { const float d = 5.0f; changed |= eam::ui::SliderFloat("Seconds kept", &c.recordSeconds, 1.0f, 30.0f, "%.0f s", 0, &d); }
        Tip("How far back a saved recording goes. More seconds take more memory.");
        { float mb = static_cast<float>(c.recordBudgetMb); const float d = 3072.0f;
          if (eam::ui::SliderFloat("Memory for it", &mb, 512.0f, 16384.0f, "%.0f MB", 0, &d)) { c.recordBudgetMb = static_cast<int>(mb / 256.0f + 0.5f) * 256; changed = true; } }
        Tip("The most memory the kept frames may take; past it the oldest go first, so fewer seconds are kept. A 1080p frame takes about 3 to 5 MB "
            "compressed, so 5 seconds at 120 frames a second is about 2 to 3 GB.");
        const Recorder::Status rs = g_recorder.GetStatus();
        if (rs.on && rs.frames) ImGui::TextDisabled("Holding %.1f s: %u frames of %ux%u, %.0f MB%s", rs.seconds, rs.frames, rs.w, rs.h, rs.bytes / 1048576.0,
                                                   rs.missed ? "" : ", none left out");
        else if (rs.on) ImGui::TextDisabled("Waiting for frames (the game must be running and scaled).");
        if (rs.on && rs.missed) ImGui::TextDisabled("%llu frames left out so far (the copies were all busy).", (unsigned long long)rs.missed);
        const bool saving = rs.saving;
        if (saving) ImGui::BeginDisabled();
        if (eam::ui::Button(saving ? "Saving..." : "Save the last seconds", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) SaveRecording();
        if (saving) ImGui::EndDisabled();
        Tip("In the game, Ctrl+Shift + the Save the recording key (see Compare and hotkeys) does the same. The file is named after the game and the time.");
        if (!c.recordOn) ImGui::EndDisabled();
        const std::wstring folder = RecordFolder();
        const int size = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string folderText(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, folderText.data(), size, nullptr, nullptr);
        ImGui::TextWrapped("Folder: %s", folderText.c_str());
        if (ImGui::SmallButton("Open folder##rec")) { CreateDirectoryW(folder.c_str(), nullptr); ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        bool ok; const std::string result = g_recorder.LastResult(ok);
        if (!result.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", result.c_str()); ImGui::PopStyleColor(); }
    }
    if (changed) Commit(c, false, createChanged);
}
} // namespace nr
