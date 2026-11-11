// What the parts of the addon share: the frame path (runtime.cpp, on Lossless Scaling's render thread), the panel (panel.cpp, on the
// manager's window thread), the background jobs (tasks.cpp) and the exports (addon.cpp).
//
// Locks: g_settingsMutex guards g_config and g_looks. g_frameMutex guards the machinery (engine, bridge, tap, compose) and the device
// bookkeeping, and is held for the whole of each tapped pass and each present. g_textMutex guards the strings shown in the panel. The counters
// the panel only displays are written on the render thread and read without a lock (a torn read shows a wrong number for one frame).
#pragma once
#include <eam/addon_sdk.h>
#include "addon/texture_format.h"
#include "addon/frame_tap.h"
#include "addon/product.h"
#include "addon/recorder.h"
#include "addon/settings.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace nr {


extern std::atomic<bool> g_resetRequested;
extern IHost* g_host;
extern std::wstring g_lsDir, g_addonDir;

extern std::mutex g_settingsMutex;
extern Config g_config;
extern std::vector<Look> g_looks;

extern std::mutex g_frameMutex;
extern FrameTap g_tap;
extern Recorder& g_recorder;   // never destroyed: its threads must not be joined as the process ends (Shutdown does it at AddonShutdown)
std::wstring RecordFolder();   // Config::recordFolder, or Videos\Lossless Scaling
void SaveRecording();          // the panel's button and the hotkey

// Switched off: by hand, by a failure, or by the watchdog (which switches it back on after a pause, three times a session at most).
extern std::atomic<bool> g_off, g_offByWatchdog;

// The engine's card, and the card the frames come from (the engine follows it).
extern LUID g_frameCard; extern std::atomic<bool> g_frameCardKnown;

// Display only, never saved (Lossless Scaling always starts on the enhanced picture).
extern std::atomic<int> g_compare;          // 0 enhanced, 1 split, 2 original only
extern std::atomic<float> g_splitPos;
extern std::atomic<bool> g_showHud;         // outline the protected areas on screen

// For the panel.

extern std::mutex g_textMutex;
extern std::string g_encodingText;   // what the frames hold (hdr.h), for the panel
extern std::string g_offReason, g_frameText, g_cardName, g_tappedDeviceText, g_focusExe;
extern std::string g_scalerGame;   // the upscalers: the game whose settings are in use (under g_settingsMutex; empty until one had focus)
extern bool g_cardDrivesDisplay;
extern uint64_t g_runs, g_otherPasses, g_lsPresents, g_composed, g_lastDelta;
extern double g_lastModelMs, g_avgModelMs, g_lastRunMs, g_lastOffset;

void SetStatus(const std::string& text, int level = 0);
std::string Status();
void ObserveScalerPass(bool nis);
void ResetStatusObservations();
void SwitchOff(const std::string& why);   // stays off until switched on again (or the watchdog's pause is over)
void SwitchOn();                           // by hand, from the panel

// The settings, as the panel changed them: kept, saved, and whatever depends on them updated.
void Commit(const Config& config, bool tapRolesChanged, bool modelSizeChanged);
void ApplyTapRoles();

// runtime.cpp
void ForgetFramePath();
// DLSS as Lossless Scaling's scaler (the DLSS 4 addon): what the panel shows, and letting go at shutdown.
struct ScalerSecond { bool valid = false; double fps = 0, repeatPct = 0, skipPct = 0, waitPct = 0, closePct = 0; };   // the upscaler's last second
struct ScalerView { bool starting = false, ready = false, failed = false; std::string error; uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
                    double gpuMs = 0, motionMs = 0; uint64_t runs = 0, nisSeen = 0; uint32_t perFrame = 0; ScalerSecond second;
                    std::string provider;     // FSR: the upscaler the runtime chose ("3.1.4", "4.1.1b")
                    std::string blocked;
                    bool preparing = false; };   // the runtime has been on one frame for a while (a first frame of a new format: FSR 4 compiles its shaders then)   // why it does not replace NIS although the NIS pass is seen (empty: nothing in the way)
ScalerView GetScalerView();
std::string ScalerEngineText();   // the upscalers: their engine's state in a few words, for Technical status
void StopScaler();
// The upscalers' runtime files to choose from (the panel's version list): the shipped one first (path empty), then those in the addon's
// runtimes\<FSR|DLSS> folder, named by the ABOUT.txt beside them or their version. Chosen: the file in use (empty: the shipped one).
struct RuntimeChoice { std::wstring path; std::string name; };
std::vector<RuntimeChoice> RuntimeChoices();
std::wstring ChosenRuntimeFile();
void ChooseRuntimeFile(const std::wstring& path);   // sets the setting the Runtimes list sets; the engine follows it   // the upscalers: Neural Rendering is on too (its Picture controls are in charge of the tone then)   // the frame path's device state, as at a device change                      // on the card the frames come from
void OnDeviceEvent(uint32_t id, const void* data, uint32_t size, void* user);
bool OnPass(uint32_t x, uint32_t y, uint32_t z, void* user);   // the manager's pre-dispatch callback
void OnPostPass(uint32_t x, uint32_t y, uint32_t z, void* user);   // its post-dispatch callback (the upscalers: NIS's half of the before / after pair)
void RequestPair();   // the upscalers' before / after pair (the panel's button)
std::string PassText(const DispatchSig& sig);                    // the views of a pass, for the log and the panel
void ResetWatchdog();

// panel.cpp
void DrawPanel();

} // namespace nr
