# DLSS Neural Rendering

`DLSSNR02` is the new NVIDIA-only addon. It processes **original real captures before LSFG**:

`capture -> original-image NVIDIA Optical Flow -> one NR evaluation -> OptiScaler composition -> LSFG analysis/interpolation -> presentation`

It never enhances generated frames or reuses an earlier neural edit. AMD NR and multipass are absent. The independent DLSS, FSR and XeSS upscaler addons are preserved; they operate later, at the NIS stage.

## Setup and supported profile

Install the matching manager (API 1.3) and this addon. Enable it in the manager. Select **WGC** in Lossless Scaling, with **LSFG 3 or frame generation off**. With FG off, choose Scaling Off or NIS. Select your user-provided `nvngx_dlssnr.dll` in the addon panel; the default location is beside `LosslessScaling.exe`. The model is never bundled or downloaded.

Currently validated: NVIDIA RTX GPU, driver test driver; LS original DLL 3.2.2.0 SHA-256 `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`; model 310.8.0.0 SHA-256 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`. LS/model hashes are checked. Other NVIDIA cards must satisfy the queried NVOFA and model capabilities; they have not been hardware tested here. Unknown LS/model builds, unsupported capture profiles and initialization failures pass originals through with an explicit status. DXGI duplication was unavailable on the validation desktop, so it is not enabled speculatively.

SDR Rec.709 and linear scRGB are supported at the capture boundary. Direct HDR10/PQ capture is not enabled. scRGB's unit is 80 nits; reference white defaults to 203 nits. Real game depth, exposure, jitter, masks and engine motion vectors are unavailable. Constant depth 1 is a verified compatibility input for this exact model, not estimated geometry.

## Settings mapping

Model and composition controls use the pinned OptiScaler implementation's values directly. There is no conversion of retired DLSS5NR01 settings; the new addon has a separate ID and schema.

**Restore OptiScaler defaults** restores the model and composition values below, keeping the selected model file, working resolution and flow settings. Existing saved adjustments are preserved until this button is used. Composition is under **Composition (advanced)** and is normally best left at its defaults. scRGB reference white is the capture-specific 203-nit baseline, not an inferred game exposure.

| New setting | OptiScaler value / meaning | Default |
|---|---|---:|
| Preset | `DLSSNR.Hint.Render.Preset`, 0–3 | 0 |
| Style | `DLSSNR.Style`, 0–2; upstream names Default/Natural/Cinematic | 0 |
| Intensity | `DLSSNR.Intensity`, 0–2; **100% = 1**, 50% = 0.5 | 1 |
| Local structure | `DLSSNR.LocalStructureStrength`, 0–2 | 1 |
| Local tone | `DLSSNR.LocalToneStrength`, 0–2 | 1 |
| Skin structure | `DLSSNR.SkinStructureStrength`, -1–2; -1 follows local structure | -1 |
| Automatic mask | `DLSSNR.UseAutoMask`, boolean | On |
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

Replace proxy modes decode the model image directly and bypass the normal composition controls. In composed modes, detail 0 and colour 0 together give the original; intensity 0 alone is a model control, not a general replacement for disabling the addon (especially at reduced resolution). The native SDR pipeline's intensity-zero identity is tested. Apply-off retains the exact original handoff (including negative scRGB components) while the backend continues; this comparison-only handoff avoids upstream's final nonnegative clamp. Model/working-size/flow/proxy changes rebuild after the control is released. Original frames pass through during rebuilding.

Flow compares **current original to previous accepted original**. NVOFA S10.5 vectors are divided by 32 and scaled separately by workingWidth/captureWidth and workingHeight/captureHeight. NR receives working-pixel displacement with MV scale 1. Flow cost is diagnostic and does not attenuate vectors. This differs from OptiScaler's usual game-vector input; no numerical mapping to an undocumented LSFG flow texture is assumed.

Shortfuse/RenoDX composition inspired upstream OptiScaler, but controls from other revisions are not automatically interchangeable. This build pins OptiScaler `973761621353b99bee3dc7d4bb27b117fef2644f`; the exact shader and licences ship with the corresponding source. The parity test compares against that revision, not an evolving upstream HEAD.

## Timing, resets and failures

Three slots retain images, allocators, descriptors and fences until both APIs finish using them. Initialization and retirement run on a worker. The consuming LSFG work waits on the exact current image, adding real GPU cost; a stale result is never substituted. A full queue bypasses the current capture.

Automatic cut detection reads an eight-byte GPU summary of 1,024 original-image samples. It uses histogram and colour-change thresholds, with a 4 ms completion deadline. A timeout bypasses unchanged and resets history. Hard cuts, sequence gaps, resize/device changes, reconfiguration and long gaps reset temporal state. This conservative heuristic cannot match game-provided scene metadata and can miss similar-histogram or gradual cuts. No production CPU frame/vector copy occurs.

The panel reports effective dimensions, per-epoch accepted/bypassed counts and completed GPU timings. Flow timing includes scheduling/wait and conversion; summary wait is a separate CPU measurement and overlaps that interval, so do not add it again. GPU timings are sampled from retired slots. Capture intervals describe observations at the hook, not the game's internal render time.

Validation includes synthetic ground-truth flow, 160 byte-exact composition parity cases, model recreation, SDR/scRGB pipeline tests, overload/recovery, and 5,495 provenance comparisons through the real LS capture loop. D3D12 debug validation reports no errors. Gameplay image quality and performance across other cards/drivers remain unvalidated. See `docs/dlss-nr-implementation-log.md` in the source checkout.

The NR model has an independent lifetime: it does not initialize or shut down the process-wide NGX core used by standalone DLSS upscalers. Live coexistence tests verify each addon continues processing after the other is disabled.

## Build and distribution

Visual Studio 2022 C++ tools, Windows SDK and CMake 3.20+: configure this directory, then build Release target `DLSSNR02`. Neither a proprietary NGX SDK nor the model is needed to compile. The pinned ImGui source is fetched unless supplied through `FETCHCONTENT_SOURCE_DIR_IMGUI`; source packages include it in `dependencies/imgui`.

Hardware tests: `neural_flowtest`, `neural_compositiontest`, `neural_modeltest <model>`, `neural_pipelinetest <model> [--debug]`. The manager's excluded `eam_captureprobe` target exercises the real LS DLL against its own source window; it never installs or modifies game/LS files.

GPL-3.0-only; see LICENSE and NOTICE.md. Binary packages include `NeuralRendering-source.zip` and attribution. The installer backs up only the retired plugin's owned executables/manifest, preserving user models, settings and unrelated files at their paths. The manager also refuses both retired folder IDs after manual overlay upgrades.
