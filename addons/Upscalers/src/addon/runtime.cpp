// Frame path for the standalone upscalers. No neural-rendering engine is linked here.
#include "addon/state.h"
#include "addon/log.h"
#include "addon/present_hook.h"
#include "addon/framegen11.h"
#include "addon/screenshot.h"
#include "addon/scaler11.h"
#include "addon/hdr.h"
#include "engine/sr_engine.h"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "version.lib")
#include <d3d11_1.h>
#include <dxgi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace nr {

namespace {

bool SameCard(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}

const char* FormatName(uint32_t f) {
    switch (f) {
    case 87: return "BGRA8"; case 91: return "BGRA8s"; case 28: return "RGBA8"; case 29: return "RGBA8s"; case 24: return "RGB10A2";
    case 10: return "RGBA16F"; case 34: return "RG16F"; case 16: return "RG32F"; case 49: return "RG8"; case 41: return "R32F";
    case 54: return "R16F"; case 61: return "R8"; case 26: return "R11G11B10F"; case 2: return "RGBA32F"; default: return "?";
    }
}

// The card a device is on, and whether it is NVIDIA's.
struct Card { LUID luid{}; std::string name = "?"; bool nvidia = false; bool drivesDisplay = false; };
Card CardOf(ID3D11Device* dev) {
    Card card;
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return card;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);
        card.luid = desc.AdapterLuid; card.name = Utf8(desc.Description); card.nvidia = desc.VendorId == 0x10DE;
        IDXGIOutput* output = nullptr;
        card.drivesDisplay = SUCCEEDED(adapter->EnumOutputs(0, &output));
        if (output) output->Release();
        adapter->Release();
    }
    dxgi->Release();
    return card;
}


thread_local bool t_ownWork = false;
std::atomic<uint32_t> g_marker{0};
std::atomic<uint64_t> g_markerUntil{0};
std::atomic<IDXGISwapChain*> g_fgChain{nullptr};
std::atomic<bool> g_pairRequested{false};
int g_pairStep = 0, g_pairWaited = 0;
std::wstring g_pairBase;
ID3D11Resource* g_pairOut = nullptr;
D3D11_BOX g_pairBox{};
DXGI_FORMAT g_pairFormat = DXGI_FORMAT_UNKNOWN;
uint32_t g_pairEncoding = 0;
float g_pairWhite = 200;
void ForgetPair();
void ScalerPresentGuarded(IDXGISwapChain*);
void ScalerMarkerGuarded(IDXGISwapChain*);
void FollowRuntimeChoice();
void ReleaseDecision(TapDecision& d) {
    if (d.frame) d.frame->Release();
    if (d.flow) d.flow->Release();
    d.frame = nullptr; d.flow = nullptr;
}

void ShowMarker(uint32_t colour) { g_marker = colour; g_markerUntil = GetTickCount64() + 1200; }

void Record(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content = 0, uint32_t tag = 0) {
    static const bool logSet = (g_recorder.SetLog([](const char* m) { Log("%s", m); }), true);
    (void)logSet;
    bool on; float seconds; int budget, saveAfter;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; seconds = g_config.recordSeconds; budget = g_config.recordBudgetMb; saveAfter = g_config.recordSaveAfter; }
    g_recorder.Configure(on, seconds, static_cast<uint32_t>(budget));
    g_recorder.Offer(ctx, frame, source, content, tag);
    static bool savedForTest = false;
    if (saveAfter > 0 && !savedForTest && g_recorder.GetStatus().frames >= static_cast<uint32_t>(saveAfter)) { savedForTest = true; SaveRecording(); }
}

nr::FrameEncoding FrameEncodingOf(DXGI_FORMAT format, IDXGISwapChain* chain, float* white, ID3D11Device* device = nullptr) {
    int setting;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); setting = g_config.frameEncoding; }
    const nr::DisplayHdr display = nr::QueryDisplayHdr(chain, device ? device : nullptr);
    const nr::FrameEncoding e = nr::EncodingOf(scaler::ViewFormat(format), setting, display.hdr);
    *white = display.whiteNits;
    static uint64_t said = ~0ull;
    const uint64_t key = (uint64_t)e << 48 | (uint64_t)format << 32 | (uint32_t)display.whiteNits | (display.hdr ? 1ull << 31 : 0);
    if (key != said) {
        said = key;
        Log("frames of format %d are %s (display in %s, SDR white %.0f nits%s)", (int)format, nr::EncodingName(e), display.hdr ? "HDR" : "SDR",
            display.whiteNits, setting ? ", set by hand" : "");
        char text[96];
        if (e == nr::FrameEncoding::Sdr) snprintf(text, sizeof text, "SDR");
        else snprintf(text, sizeof text, "%s, SDR white %.0f nits", nr::EncodingName(e), display.whiteNits);
        std::lock_guard<std::mutex> lock(g_textMutex); g_encodingText = text;
    }
    return e;
}

void LogPassTable() {
    static uint64_t loggedAt = 0; static size_t loggedRows = 0;
    const uint64_t now = GetTickCount64();
    if (now - loggedAt < 5000) return;
    loggedAt = now;
    std::vector<DispatchEntry> rows = g_tap.Snapshot();
    if (rows.size() == loggedRows) return;
    loggedRows = rows.size();
    std::sort(rows.begin(), rows.end(), [](const DispatchEntry& a, const DispatchEntry& b) { return a.count > b.count; });
    DispatchSig tick, tap; g_tap.GetRoles(tick, tap);
    Log("--- dispatch table: %zu shapes, %llu dispatches, ticks %llu, taps %llu, roles %s/%s ---", rows.size(), (unsigned long long)g_tap.Dispatches(),
        (unsigned long long)g_tap.Ticks(), (unsigned long long)g_tap.Taps(), tick.Empty() ? "no-tick" : "tick", tap.Empty() ? "no-tap" : "tap");
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        const DispatchEntry& e = rows[i];
        Log("  %6u x (%u,%u,%u) %s%s", e.count, e.sig.x, e.sig.y, e.sig.z, PassText(e.sig).c_str(), e.roleAuto == 1 ? " [auto TICK]" : e.roleAuto == 2 ? " [auto TAP]" : "");
    }
}

void SafeReleasePair() { if (g_pairOut) { g_pairOut->Release(); g_pairOut = nullptr; } }

D3D11_BOX PairBox(const NisPass& pass) {   // the part of NIS's output the picture is in (the whole of it, or its output viewport)
    D3D11_BOX b{}; b.left = pass.Partial() ? pass.outX : 0; b.top = pass.Partial() ? pass.outY : 0; b.right = b.left + pass.outW; b.bottom = b.top + pass.outH; b.back = 1;
    return b;
}

void ReadHotkeys() {
    bool on; int keys[7];
    { std::lock_guard<std::mutex> lock(g_settingsMutex);   // only what is needed: a copy of the whole Config would allocate at every present
      on = g_config.hotkeys; keys[0] = g_config.keyAB; keys[1] = g_config.keySplit; keys[2] = g_config.keySharpDn; keys[3] = g_config.keySharpUp; keys[4] = g_config.keyPreset;
      keys[5] = g_config.keyShot; keys[6] = g_config.keyRecord; }
    static bool wasDown[7] = {};
    const bool modifiers = on && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    for (int i = 0; i < 7; ++i) {
        const bool down = modifiers && keys[i] > 0 && (GetAsyncKeyState(keys[i]) & 0x8000);
        // the upscalers have no split view, looks or screenshots: only before / after and the sharpening keys act there (a look could
        // otherwise overwrite the upscaler's sharpening with one saved for Neural Rendering)
        const bool applies = i == 0 || i == 2 || i == 3 || i == 5 || i == 6;   // (5, the screenshot key: the before / after pair there)
        if (down && !wasDown[i] && applies) {
            if (i == 0) { g_compare = g_compare == 2 ? 0 : 2; ShowMarker(g_compare == 2 ? 2 : 1); Log("hotkey: %s", g_compare == 2 ? "original only" : "enhanced"); }
            else if (i == 5) { g_pairRequested = true; Log("hotkey: before / after pair"); }   // no corner square: it would be in the picture
            else if (i == 6) { Log("hotkey: save the recording"); SaveRecording(); }   // no corner square either: it would be recorded
            else {
                float sharpen;
                { std::lock_guard<std::mutex> lock(g_settingsMutex); sharpen = g_config.p.sharpen = std::clamp(g_config.p.sharpen + (i == 3 ? 0.05f : -0.05f), 0.0f, 1.0f);
                  if (kScalerAddon && g_config.scalerPerGame) KeepForGame(g_config, g_scalerGame); }
                Config c; std::vector<Look> looks;
                { std::lock_guard<std::mutex> lock(g_settingsMutex); c = g_config; looks = g_looks; }
                SaveSettings(g_host, kAddonId, c, looks);
                ShowMarker(4);
                Log("hotkey: sharpen %.2f", sharpen);
            }
        }
        wasDown[i] = down;
    }
}

std::string FocusExe(uint32_t* clientW = nullptr, uint32_t* clientH = nullptr) {
    const HWND window = GetForegroundWindow();
    if (!window) return {};
    if (clientW && clientH) { RECT r{}; GetClientRect(window, &r); *clientW = static_cast<uint32_t>(r.right - r.left); *clientH = static_cast<uint32_t>(r.bottom - r.top); }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]; DWORD size = MAX_PATH; std::string exe;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) { const wchar_t* name = wcsrchr(path, L'\\'); exe = Utf8(name ? name + 1 : path); }
    CloseHandle(process);
    for (char& c : exe) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return exe;
}

void ForgetPair() { SafeReleasePair(); g_pairStep = 0; }
void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_off || !sc) return;
    if (kScalerAddon) {
        ScalerPresentGuarded(sc);
        ScalerMarkerGuarded(sc);   // the corner square after a hotkey
        bool frameGen; { std::lock_guard<std::mutex> lock(g_settingsMutex); frameGen = g_config.frameGen; }
        if (kFrameGen && frameGen && sc == g_fgChain.load(std::memory_order_acquire)) {   // Lossless Scaling's output swap chain only (not the manager's window)
            static const bool runtimeSet = (nr::framegen::SetRuntime(g_addonDir + L"\\fsr\\amd_fidelityfx_dx12.dll"), true);   // the shipped FSR 3.1 (FSR 4's build has no frame generation)
            (void)runtimeSet;
            float white = 80.0f; uint32_t encoding = 0;
            { ID3D11Texture2D* back = nullptr;   // what its frames hold (scRGB or HDR10 on an HDR display), for measuring their motion in their SDR view
              if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) { D3D11_TEXTURE2D_DESC d; back->GetDesc(&d); back->Release(); encoding = static_cast<uint32_t>(FrameEncodingOf(d.Format, sc, &white)); } }
            bool recordShown, guard; { std::lock_guard<std::mutex> lock(g_settingsMutex); recordShown = g_config.recordShown; guard = g_config.frameGenGuard; }
            nr::framegen::SetGuard(guard);
            nr::framegen::ShownFn shown;   // "record what is shown": every frame presented, the frames between and the real ones, tagged
            if (recordShown) shown = [](ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, bool made) { Record(ctx, frame, lsrec::kPresented, 0, made ? lsrec::kMadeBetween : lsrec::kReal); };
            nr::framegen::BeforeRealPresent(sc, sync, flags, encoding, white, [](const char* m) { Log("%s", m); }, shown);
        }
        return;
    }
}

} // namespace
std::string PassText(const DispatchSig& sig) {
    std::string text;
    char view[64];
    for (int i = 0; i < 8; ++i) if (sig.srv[i].valid) { snprintf(view, sizeof view, "S%d:%ux%u %s ", i, sig.srv[i].w, sig.srv[i].h, FormatName(sig.srv[i].fmt)); text += view; }
    for (int i = 0; i < 4; ++i) if (sig.uav[i].valid) { snprintf(view, sizeof view, "U%d:%ux%u %s ", i, sig.uav[i].w, sig.uav[i].h, FormatName(sig.uav[i].fmt)); text += view; }
    return text;
}

void ForgetFramePath() { std::lock_guard<std::mutex> lock(g_frameMutex); g_tap.Reset(); ResetNisPass(); ResetStatusObservations(); ForgetPair(); screenshot::Forget(); }
void OnDeviceEvent(uint32_t id, const void*, uint32_t, void*) {
    { std::lock_guard<std::mutex> lock(g_frameMutex); g_tap.Reset(); ResetNisPass(); ResetStatusObservations(); ForgetPair(); screenshot::Forget(); }
    if (id == EAM_EVENT_D3D11_DEVICE_CHANGED) return;
    auto* const dev = static_cast<ID3D11Device*>(g_host->GetD3D11Device());
    { std::lock_guard<std::mutex> lock(g_textMutex); g_cardDrivesDisplay = false; g_cardName = "?"; }
    if (!dev) return;
    const Card card = CardOf(dev);
    { std::lock_guard<std::mutex> lock(g_textMutex); g_cardName = card.name; g_cardDrivesDisplay = card.drivesDisplay; }
    Log("device %p on '%s' LUID %08x:%08x display=%d -> %s", static_cast<void*>(dev), card.name.c_str(), card.luid.HighPart, card.luid.LowPart, card.drivesDisplay ? 1 : 0,
        card.nvidia ? "NVIDIA, ok" : kAnyCardScaler ? "not NVIDIA, ok for this upscaler" : "not NVIDIA, ignored");
    // (the upscalers judge the card on the NIS pass itself, which names the card really in use: Lossless Scaling makes devices on others too)
    // (nor does Neural Rendering: the card frame generation really runs on is named from its passes, in Tappable)
    if (kScalerAddon) SetStatus("waiting for Lossless Scaling's NIS pass (Scaling Type: NIS, the game in a window smaller than the screen)");   // once it runs, the upscaler's own line
    // The engine starts from the tap, on the card whose device actually runs LSFG (one card, a hybrid laptop, or either card of a two-card
    // machine), not from these events, which come for every device Lossless Scaling makes.
}
// ---- DLSS as Lossless Scaling's scaler (the DLSS Upscaler; see scaler11.h and engine/sr_engine.h)
//
// Every pass still goes through the frame tap, which follows frame generation's optical flow and tells real frames apart; the NIS pass is
// replaced when DLSS is ready, and runs as usual until then, when DLSS fails, or while the Before / after hotkey shows the original.
//
// NVIDIA's DLSS code runs only on the engine's own D3D12 device (started on a thread of its own: it touches no other device). Lossless
// Scaling's D3D11 device and context are used only here, on its render thread, and only for copies and one fence signal (by default nothing
// waits: the picture shown is the newest DLSS has finished, the frame before's).
// (DLSS run on Lossless Scaling's D3D11 device itself crashed it three times, 2026-09-24.)

namespace {
SrEngine g_sr;                                   // DLSS on our own device; ready once g_srStarting is false
ScalerLink g_link;                               // Lossless Scaling's side: under g_frameMutex, on the render thread only
std::atomic<bool> g_srStarting{ false };
ID3D11Device* g_linkDevice = nullptr;            // the device the link was made on (the link holds it)
uint32_t g_nisSinceTap = 0, g_nisPerFrame = 0;   // NIS passes between two real frames: the presents per real frame
uint64_t g_nisSeen = 0, g_scalerStatusAt = 0, g_upscaled = 0;
uint64_t g_linkTries = 0;   // passes handed to the upscaler since its link to Lossless Scaling's device was made
ScalerSecond g_scalerSecond;   // under g_textMutex
std::string g_scalerBlocked;   // under g_textMutex: why the upscaler is not replacing NIS although the NIS pass is seen (empty: nothing in the way)

void SetScalerBlocked(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    if (why != g_scalerBlocked && !why.empty()) Log("%s upscaler: %s", kUpscalerName, why.c_str());
    g_scalerBlocked = why;
    if (!why.empty()) SetStatus(why, 2);
}
// the game's frame time, from one real frame to the next (frame generation's capture pass), for the log and the Performance tab
int64_t g_lastTapQpc = 0;
std::vector<float> g_frameTimes;

void NoteRealFrame() {
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (g_lastTapQpc) {
        const float ms = static_cast<float>((now.QuadPart - g_lastTapQpc) * 1000.0 / f.QuadPart);
        if (ms < 500.0f) {   // longer is a pause (loading, alt-tab), not a frame
            g_frameTimes.push_back(ms);
            if (g_host && g_host->GetHostVersion() >= 0x010000) g_host->PublishMetric(kAddonId, "frame_ms", ms, "ms");
        }
    }
    g_lastTapQpc = now.QuadPart;
    if (g_frameTimes.size() >= 600) {   // every 600 real frames: the spread of the game's frame times, and what DLSS cost meanwhile
        std::vector<float> t = g_frameTimes; std::sort(t.begin(), t.end());
        auto at = [&](double q) { return t[std::min(t.size() - 1, static_cast<size_t>(q * t.size()))]; };
        double sum = 0; for (float v : t) sum += v;
        Log("game frame time over %zu real frames: average %.1f ms (%.0f fps), p50 %.1f, p95 %.1f, p99 %.1f, worst %.1f | DLSS %.2f ms a presented frame (motion %.2f), %u presented per real frame",
            t.size(), sum / t.size(), 1000.0 * t.size() / sum, at(0.5), at(0.95), at(0.99), t.back(), g_sr.GpuMs(), g_sr.MotionMs(), g_nisPerFrame);
        g_frameTimes.clear();
    }
}
std::atomic<uint32_t> g_scaleInW{ 0 }, g_scaleInH{ 0 }, g_scaleOutW{ 0 }, g_scaleOutH{ 0 };

// The runtime the upscaler runs on: the file chosen in the manager's Runtimes list (its + menu sets "fsrRuntime" / "dlssRuntime" to it), else
// the one shipped in the addon's fsr or dlss folder. The engine is given the file's folder (NGX looks for nvngx_dlss.dll there by name).
std::wstring g_srRuntimeDir;   // the folder the engine was started from (empty: not started)
std::wstring ChosenRuntimeDir() {
    std::string chosen = g_host ? g_host->GetConfig(kAddonId, kRuntimeKey, "") : "";
    if (chosen.empty()) return g_addonDir + L"\\" + kRuntimeFolderW;
    std::wstring w(chosen.size(), L'\0');
    w.resize(std::max(0, MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), (int)chosen.size(), w.data(), (int)w.size())));
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) {   // moved or deleted by hand: the shipped one rather than no upscaler at all
        static std::string said;
        if (said != chosen) { said = chosen; Log("%s upscaler: the chosen runtime %s is not there: the shipped one runs", kUpscalerName, chosen.c_str()); }
        return g_addonDir + L"\\" + kRuntimeFolderW;
    }
    const size_t slash = w.find_last_of(L"\\/");
    return slash == std::wstring::npos ? w : w.substr(0, slash);
}

// A new choice in the Runtimes list, followed while running (checked twice a second, under g_frameMutex on the render thread). The upscalers
// stop their engine, and the next pass starts it on the new file; Neural Rendering takes the model file its setting now names and starts again.
void FollowRuntimeChoice() {
    static ULONGLONG checkedAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - checkedAt < 500 || !g_host) return;
    checkedAt = now;
    if (g_srStarting || g_srRuntimeDir.empty()) return;   // not started: it starts on the chosen file anyway
    const std::wstring dir = ChosenRuntimeDir();
    if (_wcsicmp(dir.c_str(), g_srRuntimeDir.c_str()) == 0) return;
    Log("%s upscaler: the runtime in %ls is chosen: the engine starts again on it", kUpscalerName, dir.c_str());
    g_link.Shutdown(); g_linkDevice = nullptr;
    g_sr.Shutdown(); g_sr.ClearFailure();
    g_srRuntimeDir.clear();
}

// Whether Neural Rendering is on (switched on in the manager, and its own switch on): then its Picture controls act on the shown picture,
// after the upscaler, and the upscalers' brightness, contrast and gamma stand aside so the picture is not changed twice. Read every second.

void StartEngineFor(const LUID& card) {   // the engine's own device only: safe on a thread of its own
    // Once NVIDIA's or AMD's runtime has run in this process, this DLL stays loaded until Lossless Scaling closes: their code keeps state
    // that points into ours (NGX's library is linked in), and a teardown left for the exit must not find freed code. Switching the addon off
    // still stops everything; a newer build of it then takes effect after Lossless Scaling restarts, as with Neural Rendering's Present hook.
    static bool pinned = false;
    if (!pinned) {
        HMODULE self = nullptr;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&StartEngineFor), &self) != 0;
    }
    g_srStarting = true;
    SetStatus(std::string(kUpscalerName) + ": starting...");
    g_srRuntimeDir = ChosenRuntimeDir();
    std::thread([card, dir = g_srRuntimeDir] {
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        const SrEngine::Backend backend = kXessScaler ? SrEngine::Backend::Xess : kFsrScaler ? SrEngine::Backend::Fsr : SrEngine::Backend::Dlss;
        const bool ok = g_sr.Init(card, g_addonDir, dir, [](const char* m) { Log("%s", m); }, backend);
        QueryPerformanceCounter(&b);
        Log("%s upscaler: engine %s in %.0f ms, on a thread of its own", kUpscalerName, ok ? "started" : "failed", (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
        SetStatus(ok ? std::string(kUpscalerName) + " ready; waiting for the first completed frame" : g_sr.LastError(), ok ? 0 : 3);
        g_srStarting = false;
    }).detach();
}

// The upscalers' settings per game: when another game takes focus (checked about once a second), its own settings come back; one seen for
// the first time keeps the settings in use, and whatever is changed while it is the game in play is kept for it (Commit). Only the window
// Lossless Scaling is scaling counts: one whose inside is the frame's size (a chat program or a browser in front is not a game).
void FollowScalerGame(uint32_t frameW, uint32_t frameH) {
    uint32_t w = 0, h = 0;
    const std::string exe = FocusExe(&w, &h);   // empty for Lossless Scaling itself (its panel): the game before stays the one in play
    if (exe.empty()) return;
    auto alike = [](uint32_t a, uint32_t b) { return a + 8 >= b && b + 8 >= a; };
    if (!alike(w, frameW) || !alike(h, frameH)) return;
    { std::lock_guard<std::mutex> lock(g_textMutex); g_focusExe = exe; }
    Config c; std::vector<Look> looks; bool known = false;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (exe == g_scalerGame) return;
        g_scalerGame = exe;
        if (!g_config.scalerPerGame) return;
        for (const auto& [e, p] : g_config.scalerGames) if (e == exe) { ApplyProfile(g_config, p); known = true; }
        if (!known) KeepForGame(g_config, exe);
        c = g_config; looks = g_looks;
    }
    SaveSettings(g_host, kAddonId, c, looks);
    if (known) Log("game %s took focus: its own settings (sharpening %.2f, stability %.2f, edge smoothing %.2f)", exe.c_str(), c.p.sharpen, c.scalerStability, c.scalerEdges);
    else Log("game %s took focus: new to the upscaler; it keeps the settings in use, and any change made now is kept for it", exe.c_str());
}

// ---- the stall monitor: a thread of its own that says, in the log, where Lossless Scaling's render thread stopped when it stays inside the
// upscaler's pass, or no NIS pass comes for a while after they had been coming (Lossless Scaling's queue held up, for instance by a GPU
// wait). Diagnosis only: it changes nothing.
std::atomic<ULONGLONG> g_passEnteredAt{ 0 }, g_passLeftAt{ 0 };
std::atomic<const char*> g_passStep{ "" };

void StartStallMonitor() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            bool saidInside = false, saidQuiet = false;
            for (;;) {
                Sleep(1000);
                const ULONGLONG now = GetTickCount64(), in = g_passEnteredAt.load(), out = g_passLeftAt.load();
                const bool inside = in && in > out;
                if (inside && now - in > 3000) {
                    if (!saidInside) Log("stall monitor: Lossless Scaling's render thread has been inside the upscaler's pass for %llu ms, at: %s / %s; %s",
                                         (unsigned long long)(now - in), g_passStep.load(), g_link.Step(), g_link.Describe().c_str());
                    saidInside = true;
                } else saidInside = false;
                if (!inside && out && g_upscaled && now - out > 3000) {
                    if (!saidQuiet) Log("stall monitor: no pass from Lossless Scaling for %llu ms (normal when scaling stopped; a stall if the picture froze). The last one left at: %s; %s", (unsigned long long)(now - out),
                                        g_passStep.load(), g_link.Describe().c_str());
                    saidQuiet = true;
                } else if (!inside) saidQuiet = false;
            }
        }).detach();
    });
}

struct PassTimer {
    PassTimer() { g_passEnteredAt = GetTickCount64(); g_passStep = "entered"; }
    ~PassTimer() { g_passLeftAt = GetTickCount64(); }
};

bool ScalerPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    StartStallMonitor();
    const PassTimer passTimer;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    // Feature creation can fail asynchronously after Init reported ready. A
    // failed engine is no longer ready, so the ready-only branch cannot report it.
    if (!g_srStarting && g_sr.IsFailed()) SetStatus(g_sr.LastError(), 3);
    g_passStep = "in the pass";
    TapDecision d;
    g_tap.Observe(ctx, x, y, z, d);   // the flow and the real frames (nothing is handed to a model here)
    if (d.isTap) { if (g_nisSinceTap) g_nisPerFrame = g_nisSinceTap; g_nisSinceTap = 0; NoteRealFrame(); }
    ReleaseDecision(d);
    LogPassTable();

    NisPass pass;
    g_passStep = "looking for the NIS pass";
    if (!FindNisPass(ctx, x, y, z, pass, [](const char* m) { Log("%s", m); })) {
        if (NisLayoutRefused())   // NIS is chosen, but for a window it scales into part of the screen in a way that cannot be followed
            SetScalerBlocked("Lossless Scaling scales this window into part of the screen in a way the upscaler cannot follow yet, so NIS stays. "
                             "A window of the screen's shape (16:9 on a 16:9 screen) or full screen works. The Logs tab has the details: please report it.");
        return false;
    }
    ++g_nisSeen; ++g_nisSinceTap;
    ObserveScalerPass(true);
    screenshot::Tick(ctx);
    if (g_pairStep == 3) {   // NIS's half is taken right after its own dispatch: still waiting at the next NIS pass, it never came
        Log("before / after pair: NIS's picture was not taken (its pass did not run); only the upscaled one was saved");
        ForgetPair();
    }
    if (g_pairRequested.exchange(false) && !g_pairStep) {
        std::string game; { std::lock_guard<std::mutex> lock(g_settingsMutex); game = g_scalerGame; }
        g_pairBase = screenshot::PairBase(game); g_pairStep = 1; g_pairWaited = 0;
    }
    if (g_pairStep == 2) {   // NIS's half: this pass runs as NIS, and its result is taken right after its dispatch (OnPostPass)
        SafeReleasePair();
        g_pairOut = pass.out; g_pairOut->AddRef();
        g_pairBox = PairBox(pass); g_pairFormat = scaler::ViewFormat(pass.outFmt); g_pairEncoding = g_link.Encoding(); g_pairWhite = g_link.White();
        g_pairStep = 3;
        ReleaseNisPass(pass);
        return false;
    }
    FollowRuntimeChoice();
    if ((g_nisSeen & 63u) == 1) FollowScalerGame(pass.inW, pass.inH);
    g_scaleInW = pass.inW; g_scaleInH = pass.inH; g_scaleOutW = pass.outW; g_scaleOutH = pass.outH;
    ReadHotkeys();
    bool replaced = false;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!g_srStarting && dev) {
        if (!g_sr.IsReady() && !g_sr.IsFailed()) {
            const Card card = CardOf(dev);
            if (card.nvidia || kAnyCardScaler) { SetScalerBlocked(""); StartEngineFor(card.luid); }   // FSR and XeSS run on any card
            else {
                if (g_nisSeen == 1) SetStatus("waiting: Lossless Scaling's device is not an NVIDIA card");
                SetScalerBlocked("Lossless Scaling runs on " + (card.name.empty() ? std::string("a card") : card.name) +
                                 ", not an NVIDIA card, and DLSS needs an NVIDIA RTX card. Set Lossless Scaling's Preferred GPU to your NVIDIA card, or use the "
                                 "FSR Upscaler, which runs on any card.");
            }
        } else if (g_sr.IsReady()) {
            if (dev != g_linkDevice) {   // Lossless Scaling's (new) device: the link is made on it, here on its render thread
                g_link.ReportDeviceChange();
                g_link.Shutdown();
                g_linkDevice = g_link.Init(dev, ctx, &g_sr, [](const char* m) { Log("%s", m); }) ? dev : nullptr;
                g_linkTries = 0;
            }
            int handoffMode; { std::lock_guard<std::mutex> settings(g_settingsMutex); handoffMode = g_config.scalerHandoff; }
            bool frameGenOn; { std::lock_guard<std::mutex> settings(g_settingsMutex); frameGenOn = kFrameGen && g_config.frameGen; }
            // Present is hooked in every mode: the hotkeys' corner square is drawn there (and, at Present, the picture goes over NIS's)
            if (!PresentHook::Installed() && !PresentHook::Install(dev, OnPresent, [](const char* m) { Log("%s", m); }))
                Log("%s upscaler: could not hook Present; no corner square after a hotkey%s", kUpscalerName,
                    handoffMode == static_cast<int>(ScalerLink::Handoff::AtPresent) || frameGenOn ? ", and the picture cannot go over NIS's there" : "");
            if (g_linkDevice == dev && g_compare.load() != 2) {   // "original only" lets NIS run, for comparing
                PictureParams p; unsigned preset; int handoff, motion; bool gpuWait, steadyFast, shapes; float stability, edges, leanFrom;
                { std::lock_guard<std::mutex> settings(g_settingsMutex); p = g_config.p; preset = g_config.dlaaPreset; handoff = g_config.scalerHandoff; motion = g_config.motionSource;
                  gpuWait = g_config.scalerGpuWait; stability = g_config.scalerStability; edges = g_config.scalerEdges; steadyFast = g_config.scalerFastMotion; shapes = g_config.motionShapes; leanFrom = g_config.scalerLeanFrom; }
                g_sr.SetStability(stability); g_sr.SetEdgeSmoothing(edges);
                g_sr.SetFastMotion(steadyFast ? -1.0f : 0.0f); g_sr.SetFastMotionShare(leanFrom * 0.01f);   // in fast motion lean on the frame (from the chosen share, or its own), or never
                g_sr.SetMeanWeight(shapes ? 0.0f : 1.0f); g_sr.SetGradWeight(shapes ? 0.3f : 0.0f);   // the motion matched by shape and edges (a test), or by brightness
                ScalerLink::Picture picture;   // at the defaults while Neural Rendering is on (its own Picture controls act on the shown picture)
                {
                    picture.brightness = p.brightness; picture.contrast = p.contrast; picture.gamma = p.gamma; picture.shadows = p.shadows;
                    picture.highlights = p.highlights; picture.saturation = p.saturation; picture.vibrance = p.vibrance;
                }
                g_link.SetPicture(picture);
                g_passStep = "reading the display's HDR state";
                {   // the display asked is the one Lossless Scaling's window is on: NIS writes its swap chain's back buffer, which names its chain
                    // (asked each time and let go at once: a reference kept could stop Lossless Scaling from replacing its swap chain)
                    IDXGISwapChain* chain = nullptr;
                    IDXGISurface* surface = nullptr;
                    if (pass.out && SUCCEEDED(pass.out->QueryInterface(IID_PPV_ARGS(&surface)))) {
                        if (FAILED(surface->GetParent(IID_PPV_ARGS(&chain)))) chain = nullptr;
                        surface->Release();
                    }
                    float white; const nr::FrameEncoding e = FrameEncodingOf(pass.inFmt, chain, &white, dev);
                    g_fgChain.store(chain, std::memory_order_release);   // (only compared with, never used: frame generation's swap chain)
                    if (chain) chain->Release();
                    g_link.SetEncoding(static_cast<uint32_t>(e), white);
                }
                {   // Technical status's frame line (with the encoding FrameEncodingOf decided)
                    static uint64_t shownKey = 0;
                    const uint64_t key = (uint64_t)pass.inW << 40 | (uint64_t)pass.inH << 16 | (uint64_t)pass.inFmt;
                    if (key != shownKey) {
                        shownKey = key;
                        char text[96]; snprintf(text, sizeof text, "%ux%u %s", pass.inW, pass.inH, FormatName(pass.inFmt));
                        std::lock_guard<std::mutex> lock(g_textMutex); g_frameText = text;
                    }
                }
                uint32_t fw = 0, fh = 0;
                ID3D11Resource* flow = motion == 1 ? g_tap.NewestFlow(fw, fh) : nullptr;
                const float fraction = g_nisPerFrame > 1 ? 1.0f / g_nisPerFrame : 1.0f;
                t_ownWork = true;
                g_passStep = "Upscale";
                replaced = g_link.Upscale(pass, flow, fw, fh, p.flowUnit, fraction, motion == 0, preset, p.sharpen * kScalerSharpenScale, g_resetRequested.exchange(false),
                                          static_cast<ScalerLink::Handoff>(handoff), gpuWait);
                t_ownWork = false;
                g_passStep = "the recorder";
                if (ID3D11Texture2D* grabbed = g_link.TakeGrabbed()) {   // (HDR frames go to the upscaler as light: said so in the file)
                    bool shown; { std::lock_guard<std::mutex> settings(g_settingsMutex); shown = kFrameGen && g_config.frameGen && g_config.recordShown; }
                    if (!shown) Record(ctx, grabbed, lsrec::kNisInput, g_link.Encoding() ? lsrec::kLight : lsrec::kOwnEncoding);   // (else the presents are recorded)
                }
                g_passStep = "after Upscale";   // the frame as DLSS or FSR got it
                if (flow) flow->Release();
                if (replaced) ++g_upscaled;
                if (g_pairStep == 1) {   // the upscaled half: NIS's output as the upscaler just wrote it
                    if (replaced) {
                        const std::wstring path = g_pairBase + L"_" + std::wstring(kRuntimeListW) + L".png";
                        if (screenshot::Capture(ctx, pass.out, PairBox(pass), scaler::ViewFormat(pass.outFmt), path, g_link.Encoding(), g_link.White())) g_pairStep = 2;
                        else { g_pairStep = 0; Log("before / after pair: the upscaled picture could not be copied"); }
                    } else if (++g_pairWaited > 240) { g_pairStep = 0; Log("before / after pair not taken: the upscaler is not replacing NIS"); }
                }
                ++g_linkTries;
                if (!replaced && g_sr.IsFailed()) SetStatus(g_sr.LastError(), 3);
                else if (!replaced && g_link.LastRefusedFormat())
                    SetScalerBlocked("this game's frames are in a format the upscalers cannot take (the Logs tab names it), so NIS stays. Please report it.");
                else if (replaced || g_upscaled) SetScalerBlocked("");
                else if (g_linkTries > 240 && (handoff == static_cast<int>(ScalerLink::Handoff::Late) || handoff == static_cast<int>(ScalerLink::Handoff::Wait)))
                    SetScalerBlocked("it is ready but has not replaced a frame yet. The Logs tab says why.");   // (Observe and AtPresent never replace it)
            } else if (g_linkDevice != dev) {
                SetScalerBlocked("it is ready but could not be connected to Lossless Scaling's device. The Logs tab says why.");
            }
        }
    }
    if (dev) dev->Release();
    ReleaseNisPass(pass);
    const uint64_t now = GetTickCount64();
    if (g_host && now - g_scalerStatusAt >= 1000) {
        // the last second's numbers for the panel: pictures a second, and the shares shown twice, not handed over, and waited for
        const ScalerLink::Counters n = g_link.Count();
        static ScalerLink::Counters before; static uint64_t beforeAt = 0;
        if (n.passes < before.passes) before = ScalerLink::Counters();   // a new link starts counting again
        const double seconds = beforeAt ? (now - beforeAt) / 1000.0 : 0.0;
        const uint64_t passes = n.passes - before.passes;
        if (seconds > 0.0) {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_scalerSecond.fps = passes / seconds;
            g_scalerSecond.repeatPct = passes ? 100.0 * (n.repeats - before.repeats) / passes : 0.0;
            g_scalerSecond.skipPct = passes ? 100.0 * (n.skipped - before.skipped) / passes : 0.0;
            g_scalerSecond.waitPct = passes ? 100.0 * (n.waits - before.waits) / passes : 0.0;
            g_scalerSecond.closePct = passes ? 100.0 * (n.closePasses - before.closePasses) / passes : 0.0;
            g_scalerSecond.valid = true;
        }
        before = n; beforeAt = now;
        g_scalerStatusAt = now;
        char text[160];
        if (replaced) {
            snprintf(text, sizeof text, "%s %ux%u -> %ux%u, %.1f ms", kUpscalerName, g_scaleInW.load(), g_scaleInH.load(), g_scaleOutW.load(), g_scaleOutH.load(), g_sr.GpuMs());
            if (g_host->GetHostVersion() >= 0x010000) g_host->PublishMetric(kAddonId, kXessScaler ? "xess_ms" : kFsrScaler ? "fsr_ms" : "dlss_ms", g_sr.GpuMs(), "ms");
            SetStatus(text, 1);
        } else if (g_compare.load() == 2) SetStatus("Showing Lossless Scaling's NIS (Before / after)");
    }
    if (replaced && (g_upscaled == 1 || g_upscaled % 3000 == 0))
        Log("%s scaler: %llu frames upscaled, NIS passes seen %llu, %u per real frame, %s %.2f ms", kUpscalerName, (unsigned long long)g_upscaled,
            (unsigned long long)g_nisSeen, g_nisPerFrame, kUpscalerName, g_sr.GpuMs());
    return replaced;   // true: Lossless Scaling's NIS pass is skipped, DLSS's picture is in its output
}

bool ScalerFault(unsigned code) {
    char text[64]; snprintf(text, sizeof text, "exception 0x%08x in the DLSS scaler", code);
    SwitchOff(text);
    return true;
}
bool ScalerGuarded(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {   // no objects here: __try cannot unwind them
    __try { return ScalerPass(ctx, x, y, z); } __except (ScalerFault(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { t_ownWork = false; return false; }
}
void ScalerPresent(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_linkDevice) g_link.PresentCopy(sc);
}
void ScalerPresentGuarded(IDXGISwapChain* sc) {   // no objects here: __try cannot unwind them
    __try { ScalerPresent(sc); } __except (ScalerFault(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
}
// The corner square after a hotkey (the same as Neural Rendering's compose pass draws: green the upscaled picture, red the original, amber
// the split), drawn into Lossless Scaling's back buffer at Present, so the Before / after hotkey says which is which in every mode.
void ScalerMarker(IDXGISwapChain* sc) {
    const uint32_t marker = GetTickCount64() < g_markerUntil ? g_marker.load() : 0u;
    if (!marker || sc != g_fgChain.load(std::memory_order_acquire)) return;   // Lossless Scaling's output only (not the manager's window)
    static const float kColours[5][4] = { { 0.10f, 0.85f, 0.20f, 1.0f }, { 0.90f, 0.15f, 0.15f, 1.0f }, { 0.95f, 0.70f, 0.10f, 1.0f },
                                          { 0.15f, 0.45f, 0.95f, 1.0f }, { 0.65f, 0.30f, 0.90f, 1.0f } };
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; if (dev) dev->GetImmediateContext(&ctx);
    ID3D11DeviceContext1* ctx1 = nullptr; if (ctx) ctx->QueryInterface(IID_PPV_ARGS(&ctx1));
    ID3D11RenderTargetView* rtv = nullptr;
    if (ctx1 && SUCCEEDED(dev->CreateRenderTargetView(back, nullptr, &rtv))) {
        D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
        const LONG side = static_cast<LONG>(std::max(12u, d.Width / 150u));
        const D3D11_RECT r{ 0, 0, side, side };
        ctx1->ClearView(rtv, kColours[std::min(marker, 5u) - 1], &r, 1);
        rtv->Release();
    }
    if (ctx1) ctx1->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}
void ScalerMarkerGuarded(IDXGISwapChain* sc) {   // no objects here: __try cannot unwind them
    __try { ScalerMarker(sc); } __except (ScalerFault(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
}
} // namespace

ScalerView GetScalerView() {
    ScalerView v;
    v.starting = g_srStarting; v.ready = !v.starting && g_sr.IsReady(); v.failed = !v.starting && g_sr.IsFailed();
    if (v.failed) v.error = g_sr.LastError();
    v.inW = g_scaleInW; v.inH = g_scaleInH; v.outW = g_scaleOutW; v.outH = g_scaleOutH;
    v.gpuMs = v.ready ? g_sr.GpuMs() : 0; v.motionMs = v.ready ? g_sr.MotionMs() : 0; v.runs = g_upscaled; v.nisSeen = g_nisSeen; v.perFrame = g_nisPerFrame;
    { std::lock_guard<std::mutex> lock(g_textMutex); v.second = g_scalerSecond; if (!v.starting && !v.failed) v.blocked = g_scalerBlocked; }
    if (v.ready) v.provider = g_sr.Provider();
    v.preparing = v.ready && g_sr.BusyMs() > 1500;
    return v;
}

void StopScaler() {
    LARGE_INTEGER f, a, b, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 500 && g_srStarting; ++i) Sleep(10);
    std::lock_guard<std::mutex> lock(g_frameMutex);   // nothing else takes it now: the callbacks are gone (AddonShutdown)
    const ScalerLink::Counters n = g_link.Count();
    if (n.passes)
        Log("%s upscaler: this link: %llu passes (%.1f%% within %.0f ms of the one before), %llu pictures shown twice (%.1f%%, %llu of them close), %llu frames not "
            "handed over (%.1f%%); waited on the GPU for the next picture %llu times (%.1f%%, %llu close)", kUpscalerName, (unsigned long long)n.passes,
            100.0 * n.closePasses / n.passes, ScalerLink::Counters::kClosePassMs, (unsigned long long)n.repeats, 100.0 * n.repeats / n.passes,
            (unsigned long long)n.closeRepeats, (unsigned long long)n.skipped, 100.0 * n.skipped / n.passes, (unsigned long long)n.waits,
            100.0 * n.waits / n.passes, (unsigned long long)n.closeWaits);
    g_link.Shutdown();   // unblocks the engine's queue (see ScalerLink::Unblock), drains it, releases the shared textures and fences
    g_linkDevice = nullptr;
    QueryPerformanceCounter(&b);
    const bool clean = g_sr.Shutdown();   // our own device (and NVIDIA's or AMD's runtime); left for the process's exit if the GPU is stuck
    QueryPerformanceCounter(&c);
    Log("%s upscaler: %.2f ms a frame on the GPU at the end (motion %.2f ms of it)", kUpscalerName, g_sr.GpuMs(), g_sr.MotionMs());
    Log("%s upscaler stopped: the link in %.0f ms, the engine in %.0f ms%s", kUpscalerName, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart,
        (c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart, clean ? "" : " (the GPU had not finished: its teardown is left for Lossless Scaling's exit)");
}

// After each of Lossless Scaling's passes (the upscalers only): NIS's half of a before / after pair, taken right after NIS wrote it.
void OnPostPass(uint32_t, uint32_t, uint32_t, void*) {
    if (!kScalerAddon || g_pairStep != 3) return;
    auto* const ctx = static_cast<ID3D11DeviceContext*>(g_host ? g_host->GetDispatchingContext() : nullptr);
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_pairStep != 3 || !g_pairOut || !ctx) return;
    if (!screenshot::Capture(ctx, g_pairOut, g_pairBox, g_pairFormat, g_pairBase + L"_NIS.png", g_pairEncoding, g_pairWhite))
        Log("before / after pair: NIS's picture could not be copied");
    SafeReleasePair();
    g_pairStep = 0;
}

bool OnPass(uint32_t x, uint32_t y, uint32_t z, void*) {
    if (!t_ownWork) ObserveScalerPass(false);
    auto* ctx = static_cast<ID3D11DeviceContext*>(g_host ? g_host->GetDispatchingContext() : nullptr);
    return !t_ownWork && !g_off && ctx ? ScalerGuarded(ctx, x, y, z) : false;
}


std::wstring RecordFolder() {
    std::string chosen;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); chosen = g_config.recordFolder; }
    if (!chosen.empty()) {
        std::wstring w(chosen.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), static_cast<int>(chosen.size()), w.data(), static_cast<int>(w.size()));
        w.resize(n > 0 ? n : 0);
        for (wchar_t& ch : w) if (ch == L'/') ch = L'\\';
        return w;
    }
    PWSTR videos = nullptr; std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) folder = videos;
    CoTaskMemFree(videos);
    // Videos moved into OneDrive would upload every recording (gigabytes): the profile's own Videos folder instead
    if (folder.find(L"\\OneDrive") != std::wstring::npos) {
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) folder = std::wstring(profile) + L"\\Videos";
        CoTaskMemFree(profile);
    }
    return (folder.empty() ? g_lsDir : folder) + L"\\Lossless Scaling";
}

void SaveRecording() {
    std::string game;
    { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
    bool on; { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; }
    if (!on) {   // each addon has its own recorder: say which one is off, not "nothing recorded" (the owner had it on in another addon)
        const std::string name = std::string(kUpscalerName) + " Upscaler";
        Log("recorder: nothing saved: the recorder is off in %s (tick \"Keep the last few seconds ready to save\" in its panel; each addon has its own)", name.c_str());
        SetStatus("Nothing saved: the recorder is off in " + name + " (each addon has its own)", 2);
        return;
    }
    if (!g_recorder.Save(RecordFolder(), game)) Log("recorder: nothing saved (%s)", g_recorder.GetStatus().saving ? "a save is running" : "nothing recorded yet");
}



namespace {
std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
const wchar_t* RuntimeFileName() { return kRuntimeFileW; }
const char* RuntimeKey() { return kRuntimeKey; }
} // namespace

std::vector<RuntimeChoice> RuntimeChoices() {
    std::vector<RuntimeChoice> list;
    if (!kScalerAddon) return list;
    list.push_back({ L"", std::string(kUpscalerName) + " (" + kRuntimeVendor + ", shipped)" });
    WIN32_FIND_DATAW found{};
    const std::wstring folder = g_addonDir + L"\\runtimes\\" + kRuntimeListW;
    const HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return list;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        const std::wstring dir = folder + L"\\" + found.cFileName, file = dir + L"\\" + RuntimeFileName();
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string name;
        if (FILE* about = _wfopen((dir + L"\\ABOUT.txt").c_str(), L"rb")) {   // its own name: the first line
            char line[128] = {};
            if (fgets(line, sizeof line, about)) { name = line; while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back(); }
            fclose(about);
            if (name.size() >= 3 && (unsigned char)name[0] == 0xEF) name.erase(0, 3);
        }
        if (name.empty()) name = std::string(kUpscalerName) + " (" + Narrow(found.cFileName) + ")";
        list.push_back({ file, name });
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return list;
}

std::wstring ChosenRuntimeFile() {
    const std::string chosen = g_host ? g_host->GetConfig(kAddonId, RuntimeKey(), "") : "";
    std::wstring w(chosen.size(), L'\0');
    w.resize(std::max(0, MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), (int)chosen.size(), w.data(), (int)w.size())));
    for (wchar_t& ch : w) if (ch == L'/') ch = L'\\';
    return w;
}

void ChooseRuntimeFile(const std::wstring& path) {
    if (!g_host) return;
    g_host->SetConfig(kAddonId, RuntimeKey(), Narrow(path).c_str());
    g_host->SaveConfig();
    Log("%s upscaler: runtime chosen in the panel: %s", kUpscalerName, path.empty() ? "the shipped one" : Narrow(path).c_str());
}

void RequestPair() { g_pairRequested = true; }   // the panel's button (the pair is taken at the next passes)

std::string ScalerEngineText() {
    if (g_srStarting) return "starting";
    bool blocked; { std::lock_guard<std::mutex> lock(g_textMutex); blocked = !g_scalerBlocked.empty(); }
    if (g_sr.IsReady()) return blocked ? "ready, but not replacing NIS (the Upscaling section says why)" : !g_nisSeen ? "ready, waiting for the NIS pass" : "running";
    if (g_sr.IsFailed()) return "failed: " + g_sr.LastError();
    if (blocked) return "not started: something is in the way (the Upscaling section says what)";
    return g_nisSeen ? "not started (the NIS pass is seen)" : "not started: it starts when Lossless Scaling runs its NIS pass";
}

} // namespace nr
