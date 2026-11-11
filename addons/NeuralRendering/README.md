# DLSS Neural Rendering

`DLSSNR02` is the new NVIDIA-only addon. It processes **original real captures before LSFG**:

`capture -> original-image NVIDIA Optical Flow -> NR (1–10 passes) -> OptiScaler composition -> optional Picture adjustments -> LSFG analysis/interpolation -> presentation`

It never enhances generated frames or reuses an earlier neural edit. AMD NR is absent. The independent DLSS, FSR and XeSS upscaler addons are preserved; they operate later, at the NIS stage.

## Setup and supported profile

Install the matching manager (API 1.4) and this addon. Enable it in the manager. Select **WGC** in Lossless Scaling, with **LSFG 3 or frame generation off**. With FG off, choose Scaling Off or NIS. Select your user-provided `nvngx_dlssnr.dll` in the addon panel; the default location is beside `LosslessScaling.exe`. The model is never bundled or downloaded.

Supported LS runtime: original DLL 3.2.2.0 SHA-256 `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`. The LS hash is checked. Model DLLs are not restricted to a hash or version: newer user-provided DLLs are accepted if they expose the required D3D12 NGX functions and initialize successfully. An NVIDIA GPU with compatible NVOFA and model capabilities is required. While NR is enabled, unknown LS builds and unsupported capture profiles pause capture with an explicit status. Disable NR to change an unsupported setup. Model initialization or processing errors pause the current capture until Retry initialization or an explicit disable; originals are never selected automatically. DXGI duplication is not supported.

SDR Rec.709 and linear scRGB are supported at the capture boundary. Direct HDR10/PQ capture is not enabled. scRGB's unit is 80 nits; reference white defaults to 203 nits. Real game depth, exposure, jitter, masks and engine motion vectors are unavailable. Constant depth 1 is a compatibility input, not estimated geometry. Synthetic pipeline tests passed with these DLLs:

| Model version | SHA-256 |
|---|---|
| 310.8.0.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |
| 310.9.16.0 (unsigned compatibility build) | `cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed` |

The 310.9.16.0 tests cover Default, Original and Fast model creation/evaluation/retirement, live model switching, SDR/scRGB output, native and half working resolutions, scene resets, overload and initialization recovery. D3D12 validation reported zero errors. Other DLLs and gameplay across hardware still need validation.

**Model** selects Default (the DLL chooses), Original (A), or Fast (B). Fast requests preset 2, the smaller network included in 310.9.16.0 and later; Original requests preset 1. This mapping is documented in the [runtime model research](https://github.com/mauri870/dlssnr-pytorch#the-lighter-network-3109160-and-later). An older DLL may ignore the Fast hint and keep using its original network. The existing Preset 3 option remains available for saved configurations.

## Settings mapping

Model and composition controls use the pinned OptiScaler implementation's values directly. There is no conversion of retired DLSS5NR01 settings; the new addon has a separate ID and schema.

**Restore defaults** restores the model and composition values below, keeping the selected model file, working resolution and flow settings. Existing saved adjustments are preserved until this button is used. Composition is under **Composition (advanced)**. scRGB reference white defaults to 203 nits.

| Setting | Value / meaning | Default |
|---|---|---:|
| Multipass | Off = one evaluation; 2–10 sequential model evaluations on the same real capture | Off |
| Model | `DLSSNR.Hint.Render.Preset`: Default 0, Original (A) 1, Fast (B) 2, legacy Preset 3 | Default |
| Style | `DLSSNR.Style`, 0–2; upstream names Default/Natural/Cinematic | 0 |
| Intensity | `DLSSNR.Intensity`, 0–2; **100% = 1**, 50% = 0.5 | 1 |
| Local structure | `DLSSNR.LocalStructureStrength`, 0–2 | 0.8 |
| Local tone | `DLSSNR.LocalToneStrength`, 0–2 | 0.3 |
| Automatic skin detection | `DLSSNR.UseAutoMask`; enables the Skin structure control below it | On |
| Skin structure | `DLSSNR.SkinStructureStrength`, 0–2; adjusts detected skin detail when automatic detection is on | 0.8 |
| Detail | `gTransferStrength`, 0–4; 1 applies the model's luminance edit, 0 removes it | 1 |
| Colour | `gColourStrength`, 0–4; 0 preserves the original palette, 1 uses model colour, >1 boosts chroma | 1 |
| Maximum luminance ratio | `gMaxRatio`; two-sided luminance bound, e.g. 2 means 0.5×–2× | 2 |
| HDR proxy | `gReversibleMode`: 0 knee, 1 Neutwo composed, 2 Neutwo replace, 3 hybrid composed, 4 hybrid replace | 0 |
| Reconstruction | `gTransfer`: 0 classic, 1 matched same-frame residual | 1 |
| scRGB reference white | `gWhitePoint = nits / 80` in the linear captured image | 203 nits |
| Apply model result | `gApplyModel`; off compares the original while processing stays active | On |
| Working resolution | Explicit per-axis percentage; width/height are floor(capture dimension × percent / 100), with no hidden cap | 100% |
| Flow grid | NVOFA physical grid spacing: 1, 2 or 4 input pixels; independent of NR working resolution | 4 |
| Flow quality | NVOFA slow/medium/fast | Medium |

Slider limits also apply to typed values. Skin structure keeps its value while automatic detection is off. Legacy negative skin settings are converted once to the saved Local structure value; Skin structure is then independent of Local structure.

Replace proxy modes decode the model image directly and bypass the normal composition controls. In composed modes, detail 0 and colour 0 together give the original; intensity 0 alone is a model control, not a general replacement for disabling the addon (especially at reduced resolution). The native SDR pipeline's intensity-zero identity is tested. Apply-off retains the exact original handoff (including negative scRGB components) while the backend continues; this comparison-only handoff avoids upstream's final nonnegative clamp. Model/working-size/flow/proxy changes rebuild after the control is released. Capture waits during rebuilding.

Flow compares **current original to previous accepted original**. NVOFA S10.5 vectors are divided by 32 and scaled separately by workingWidth/captureWidth and workingHeight/captureHeight. NR receives working-pixel displacement with MV scale 1. Flow cost is diagnostic and does not attenuate vectors. This differs from OptiScaler's usual game-vector input; no numerical mapping to an undocumented LSFG flow texture is assumed.

Shortfuse/RenoDX composition inspired upstream OptiScaler, but controls from other revisions are not automatically interchangeable. This build pins OptiScaler `973761621353b99bee3dc7d4bb27b117fef2644f`; the exact shader and licences ship with the corresponding source. The parity test compares against that revision, not an evolving upstream HEAD.

## Picture

**Picture > Enable picture adjustments** is off by default. It adjusts the finished DLSS5 image at capture resolution, before Lossless Scaling's scaling and frame generation. It is independent of the model and composition controls. Slider changes apply on the next capture without rebuilding the model or resetting its history.

| Control | Range | Neutral |
|---|---|---|
| Exposure | -4 to +4 stops; +1 doubles linear light | 0 |
| Contrast | -2 to +2; smooth luminance contrast around middle grey | 0 |
| Midtones | -2 to +2; darken/brighten middle tones | 0 |
| Shadows | -2 to +2; deepen/lift dark areas, keeping black | 0 |
| Highlights | -2 to +2; lower/raise bright areas | 0 |
| Saturation | 0 to 4; 0 is monochrome | 1 |
| Vibrance | -1 to +2; preferentially boosts muted colours; negative values mute them | 0 |
| Temperature | -2 to +2; cooler/warmer relative balance, not Kelvin | 0 |
| Tint | -2 to +2; greener/more magenta | 0 |
| Sharpening | 0 to 4; 0–1 suppresses halos, above 1 allows stronger edge overshoot | 0 |

Turning Picture off preserves its values. **Reset picture** returns only these ten sliders to neutral, keeping the switch as it was. The model's **Restore defaults** does not reset Picture. **Apply model result** off still shows the exact original, bypassing Picture too, while NR continues processing.

SDR is decoded from sRGB to linear light for exposure and colour adjustments and encoded again at output. Tone curves keep SDR black and white fixed; exposure at extremes can still clip SDR. Out-of-gamut SDR colour is reduced toward the same luminance. scRGB uses the existing reference-white value to locate tonal regions; it retains HDR values above 1 and signed colour channels, with only the FP16 storage bound at output. Saturation/vibrance operate around Rec.709 luminance; vibrance does not detect skin. Sharpening precedes grading and uses a five-sample cross. Up to 1 it stays inside the neighbourhood's luminance range; above 1 it progressively permits overshoot, reaching 75% of that range at 4, with a maximum 8x luminance gain. Strong settings intentionally allow halos and amplified noise. There is no temporal filtering, grain, HUD mask or image history in this stage. Highlights cannot recover detail already clipped in the input.

NR 1.2.2 expands these ranges without rescaling existing settings or changing defaults. Contrast and midtones beyond +/-1 extend their original curves with a second monotonic stage, avoiding inverted gradients. `neural_picturetest --capture-output <file>` and `--verify-output <file>` compare 100 SDR/HDR images at settings within the original ranges between builds.

The active shader **replaces the existing final D3D11 copy**: one dispatch, no extra image texture, copy or fence. Off, or enabled with all values neutral, uses the original copy shader without picture constant-buffer work. The picture shader and 48-byte constant buffer are created lazily on the first active frame and retained until the pipeline retires. A never-enabled pipeline allocates neither. Zero added GPU work while bypassed is checked separately from the active shader's cost. Existing NR GPU timing covers the D3D12 stages and excludes the D3D11 handoff (both its original copy and Picture variant).

`neural_picturetest` runs the actual shader through D3D11 WARP with validation: exact bypasses and lazy resources, one-dispatch handoff, linear exposure, signed/HDR range, colour adjustments, monotonic extreme tone curves, sharpening bounds, alpha/source preservation and constant-buffer restoration. It needs the Windows graphics debug layer. `neural_pipelinetest <model> --picture --debug` adds real NVIDIA pipeline coverage, including live controls without history resets, apply-off comparison and optional `--passes 2`.

## Multipass

**Multipass**, below **Automatic skin detection**, selects Off, 2, 3, …, 10. Off retains the original single-pass model settings, GPU operations and composition; it does not disable Neural Rendering. Existing configurations default to Off (stored as one pass). One pass allocates no extra model features or scratch images. Restore defaults also restores Off.

The chain follows [OptiScaler PreSR-Multipass's design](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/blob/main/OptiScaler/dlssnr/design/pre-sr-multipass.md): each pass owns a persistent model and independent temporal history. Later passes inherit the selected model controls with local tone set to zero. The input is encoded once, model outputs alternate between two working textures, and the final answer is composed once against the untouched original input. Flow is still calculated once from original captures; every pass receives the same current-frame guides and reset. The pinned composition shader is unchanged.

Extra passes add model time and model-history VRAM, plus one working-size RGBA16F scratch texture in each of the three frame slots. They can strengthen the effect, including unwanted artifacts. The model timing reports the entire chain. Changing the count rebuilds on control release; capture waits for all requested features to finish initialization. A failed extra pass pauses processing with its pass number, just like other NR failures; no partial result or fewer passes are silently substituted. Initialization and retirement are fenced, with all feature handles released before shared runtime shutdown.

On RTX 5080 with model 310.9.16.0, the one-pass build matched all 108 pre-change output images byte-for-byte across the six pipeline scenarios. Two-, three- and ten-pass debug runs passed SDR/scRGB, native/reduced working size, scene resets, overload, model switching and live 1→3→2→1 switching with zero D3D12 errors. A copy-model fixture separately checks the pass resource chain through ten passes, independent histories, shared guides/resets, and creation/evaluation failures in passes 2, 3 and 10. This is synthetic validation; representative gameplay quality and pacing remain to be assessed.

## Timing, resets and failures

### Diagnosing a black screen or missing enhancement

The manager footer uses one line during normal operation, including idle/disabled states and brief settings changes. Warnings and errors expand it to show the current reason and recovery action first, then capture/NR measurements and system information. Content scrolls above the footer; the outer window does not add a second scrollbar. **Details** expands the unabridged diagnostics in either layout; **Copy** copies them for a bug report. Long rows show an ellipsis. The NR status refreshes twice per second independently of capture callbacks and the Settings tab, including while the first capture is blocked. Status transitions are written to `logs/LSAddonManager.log`. A blocked first frame must not leave the old “Waiting for a real capture” message hiding its error.

The version message names **supported LS 3.2.2.0**, not an automatically discovered “latest” version. The existing original-DLL hash check remains authoritative; even a DLL with the same version resource can be an unverified build. Update the pinned hash and supported-version text together when validating a new LS release.

| Scenario / edge case | What the status means and what to do |
|---|---|
| DXGI or another capture API | Capture is paused. Disable NR, select **WGC**, then restart scaling. The profile is diagnosed as soon as LS applies settings, before a capture callback succeeds. |
| Unsupported LS DLL, changed build, stale original DLL, or unreadable hash | Version may differ; use supported **3.2.2.0** and repair the manager installation. Details show the detected original-DLL version and verified/unverified state. A matching version string alone does not pass the check. |
| LSFG other than 3 | Disable NR and select LSFG 3 or FG Off, then restart scaling. |
| FG Off with a scaler other than Off/NIS | Disable NR and select Scaling Off or NIS. Other scalers with FG on are not rejected by this rule. |
| Settings still being applied | Wait for LS; if it stays there, disable NR and restart scaling. A transition is not reported as a model failure. |
| Scaling not started; stopped, minimized, paused, or source not producing captures | Waiting/no new captures. After a quiet interval the status stops claiming current capture fps. These conditions cannot be distinguished reliably by the hook alone. |
| Compute dispatches arrive but no supported capture matches | The hook is active but has not recognized the real-frame stage. Check WGC/profile and restart scaling. This is different from receiving no dispatches at all. |
| Missing original LS DLL/device-creation entry point, import patch failure, MinHook/Dispatch hook failure, or shader-identification hook failure | Capture hook unavailable. The existing hook reports its failure; restart LS, repair the manager, and inspect Details/Logs. No extra hook probes are run. |
| LS D3D11 device creation fails | Reports the original HRESULT. A subsequently observed working device clears this transient device-creation error. |
| Shader has no recorded identity | Capture pauses with a shader-identification explanation. Restart LS and inspect hook installation; the addon does not guess an unrecognized shader's role. |
| HDR flag/format mismatch, direct PQ/HDR10, multisampled/array texture, invalid size/mips | Unsupported capture texture; Details retain dimensions, format, HDR flag, samples, array size and mip count. Disable NR and use SDR/scRGB. |
| Missing capture output/scaler input, non-mip-zero view, or unexpected dispatch shape | Reports the specific handoff/view mismatch. Restart scaling using the supported profile; report persistent failures with copied diagnostics. |
| Capture handoff allocation/view creation fails | Reports HRESULT and asks to reduce capture size/check VRAM. This is distinguished from an unsupported image format. |
| Addon not loaded, faulted, incompatible host API, or missing dependency | The manager shows the loader's error even when no addon reporter exists. Install matching manager/addon files or restart when the loader says a restart is required. |
| Another addon owns real-frame processing | Disable the conflicting addon and reload NR. A failed registration does not unregister another owner's callback. |
| No NVIDIA capture adapter / LS selected an integrated or other GPU | Select a compatible NVIDIA GPU for LS. The observed LS adapter is shown separately from the existing GPU-0 load sampler. |
| D3D12, shared-resource or shared-fence setup fails | Reports the failing operation and HRESULT; check the selected GPU/driver and restart LS. No speculative Windows settings changes are prescribed. |
| NVIDIA Optical Flow driver/API unavailable or incomplete; session/capability/format/resource setup fails | Reports the original optical-flow error/code; check GPU/driver support and update the driver if appropriate. |
| Flow grid unsupported | Select the default 4-pixel grid and retry; a driver rejecting that default still reports its error. |
| Capture outside optical-flow dimension limits | Reduce the game/capture resolution. NR Working resolution does **not** shrink optical flow's original capture input. |
| Missing user model | Select `nvngx_dlssnr.dll` with Browse. No model is downloaded automatically. |
| Missing addon forwarder | Repair/reinstall the matching addon package. |
| NGX core missing/unloadable, missing model exports, model init/create failure, unsupported parameter ABI, or parameter round-trip failure | Check the selected model and NVIDIA driver. Retry initialization or select a compatible model; keep the exact error/code in a report. |
| Invalid saved model/pipeline settings | Restore NR defaults and retry. Unknown internal errors retain their original text and suggest copying diagnostics rather than guessing a cause. |
| GPU/system allocation failure | Reduce capture/working resolution and disable Multipass. An allocation HRESULT does not prove VRAM exhaustion; both system and GPU allocations can fail. |
| Long initialization, pipeline retirement, free-slot wait, scene-summary wait, optical flow, model evaluation, composition, or LS handoff | Shows the observed operation and elapsed seconds. After three seconds it warns that capture is waiting. It does not declare a timeout/device crash or automatically bypass NR. |
| Model evaluation or an additional multipass initialization/evaluation fails | Pauses with the exact error (and pass number when supplied). Retry or disable; no partial/fewer-pass result is selected. |
| Device removed/hung/reset or failed safe retirement | Restart LS. Failed retirement stays fatal across toggling/retry because the old GPU objects cannot safely be reused. |
| Callback faults or returns false while still required | Manager reports the callback failure even though the callback cannot publish its own error. Disable NR and restart LS. |
| Resize, capture restart, model/working-size/flow change, sequence gap, or scene cut | Expected reinitialization/history resets, with fresh dimensions and per-epoch counters. Old initialization errors clear when a valid retry is requested. |
| NR disabled / Apply model result off / neutral picture controls | Disabling NR permits originals. Apply-off still runs NR and can still block; it is a comparison mode. Neutral Picture controls do not imply NR is disabled. |
| GPU sampler unavailable/stale, multiple GPUs, unknown CPU or driver | Show unavailable/unknown rather than inventing readings. GPU-0 utilization/VRAM is explicitly identified and may not belong to the LS adapter. Missing telemetry does not prevent NR. |
| Black pixels despite advancing captures and successful processing | The status cannot establish why the image is black: source content, protected capture, wrong target, another overlay/addon, display/HDR configuration, and driver behavior remain possibilities. Compare with NR disabled and collect diagnostics; no pixel scans, process inventory, overlay detection or invasive compatibility checks are added. |

Capture fps measures callback arrival, not game FPS or displayed/generated FPS. GPU timings are the last completed samples and are not a live measurement during a stall; zero initially means no completed sample. Counts are per pipeline epoch. CPU identity is cached from CPUID, the observed LS GPU/Windows driver comes from DXGI, and NVIDIA driver/load/VRAM uses the existing NVML sampler. Static metadata is not polled through WMI, external commands, registry scans, network requests, or new driver hooks.

`neural_statustest` loads the actual addon with a fake host and no GPU/model. It covers unsupported profiles, status refresh without Settings/capture completion, a blocked first-frame failure, old manager APIs, ownership conflicts, and reporter shutdown. Manager GUI tests cover reason ordering, capture observations and multiple-GPU attribution; offscreen previews cover DXGI, runtime mismatch and running states.

Three slots retain images, allocators, descriptors and fences until both APIs finish using them. Initialization and retirement run on a worker. Every capture waits for a free slot and the consuming LSFG work waits on the exact current enhanced image. GPU or VRAM contention reduces throughput and can stall the image; it never selects an original or an older neural edit. There is one processing policy, with no overload mode or automatic quality reduction. Capture also waits for initialization and reconfiguration.

Automatic cut detection reads an eight-byte GPU summary of 1,024 original-image samples. It uses histogram and colour-change thresholds and waits for the current GPU summary without a frame deadline. GPU failures produce an explicit paused error. Hard cuts, sequence gaps, resize/device changes, reconfiguration and long gaps reset temporal state. This conservative heuristic cannot match game-provided scene metadata and can miss similar-histogram or gradual cuts. No production CPU frame/vector copy occurs.

The panel reports effective dimensions, per-epoch processed counts, frame-slot and scene-summary CPU waits, and completed GPU timings. Flow timing includes scheduling/wait and conversion; summary wait is a separate CPU measurement and overlaps that interval, so do not add it again. GPU timings are sampled from retired slots. Capture intervals describe observations at the hook, not the game's internal render time.

Version 1.0.1 follows an elevated capture-device GPU priority with a supported D3D12 HIGH queue; normal capture priority remains normal. Unsupported or rejected HIGH creation falls back to normal. DXGI and D3D12 priority numbers use different scales. Every five seconds, the manager log records effective priorities, capture/callback CPU percentiles, last completed GPU stage times, processed/reset counts and slot/summary waits. These measurements do not establish displayed frame cadence: the LS FPS counter alone cannot prove smooth interpolation. For a choppiness comparison, leave NR enabled and disable only **Apply model result**; processing continues with the exact original image passed to LSFG.

Validation includes synthetic ground-truth flow, composition parity, model recreation, SDR/scRGB pipeline tests, overload/recovery, and provenance comparisons through the real LS capture loop. Gameplay image quality and performance across hardware and drivers remain unvalidated.

The NR model has an independent lifetime: it does not initialize or shut down the process-wide NGX core used by standalone DLSS upscalers. Live coexistence tests verify each addon continues processing after the other is disabled.

## Build and distribution

Visual Studio 2022 C++ tools, Windows SDK and CMake 3.20+: configure this directory, then build Release target `DLSSNR02`. Neither a proprietary NGX SDK nor the model is needed to compile. The pinned ImGui source is fetched unless supplied through `FETCHCONTENT_SOURCE_DIR_IMGUI`; source packages include it in `dependencies/imgui`.

`neural_forwardertest` checks runtime loading, model selection, independent feature handles and failure recovery with test DLLs, without a GPU or proprietary model. Hardware tests: `neural_flowtest`, `neural_compositiontest`, `neural_modeltest <model> [intensity=1] [preset=0]`, `neural_pipelinetest <model> [--debug] [--preset 0|1|2|3] [--passes 1..10]`, and `neural_multipasstest` (NVIDIA hardware/driver, no proprietary model required). For before/after pixel comparisons, run the old pipeline test with `--capture-output <file>` and the new one with `--verify-output <file>`, using the same model, preset and GPU. The manager's excluded `eam_captureprobe` target exercises the real LS DLL against its own source window; it never installs or modifies game/LS files.

GPL-3.0-only; see LICENSE and NOTICE.md. Binary packages include `NeuralRendering-source.zip` and attribution. The installer backs up only the retired plugin's owned executables/manifest, preserving user models, settings and unrelated files at their paths. The manager also refuses both retired folder IDs after manual overlay upgrades.
