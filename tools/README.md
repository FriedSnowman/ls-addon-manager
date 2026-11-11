# Tools

Scripts to build, test, look at and package LS Addon Manager. They find the repository from their own location, so run them from anywhere.
Where a script needs your Lossless Scaling folder it takes `-LsDir` (or reads the `LS_DIR` environment variable); the default is the usual Steam path.

**Rules the tools follow**

- UI previews render offscreen. GUI and real-capture tests can create their own temporary windows; they do not modify games.
- `deploy.ps1` never replaces files under a running game: it refuses while the game or Lossless Scaling runs, and backs up what it overwrites.
- NR hardware tests use the GPU and run serially. Run them outside an active gameplay session.

| Tool | What it does |
|---|---|
| `build_all.ps1` | Configures and builds manager, independent NR and preserved upscaler products, Release x64. `-Only host,neural,scalers`; add `-IncludeTools` for the UI preview and offline upscaler diagnostics. Optional `-Build` sets the manager's release number; otherwise it displays Development. Supports custom build directories and `-Jobs` (default 2). |
| `ci.ps1` | Fast installer-payload and NR-loader smoke tests, with a 30-second limit per test. No GPU, model, dependency downloads or GUI builds. `-Full` opts into the broader manager, installer, NR and sample suite. |
| `run_addon_tests.ps1` | Offline tests, no game needed, in suites (core, features, sample, gui, installer, setupexe, nr). By default it builds and runs only the suites that the files changed since the last commit can affect, and prints only failures and the time each suite took. `-Only core,gui` picks suites, `-All` runs every one (before a release), `-List` shows what starts each. |
| `run_addon_tests.ps1 -Only nr -ModelPath <file>` | Runs independent flow, pinned OptiScaler parity, model and pipeline tests. Without a model path, runs flow/composition only. The retired host matrix is removed. |
| `eam_captureprobe` (manager test target) | Runs the local original LS DLL against a disposable animated window. Can check DLSS, FSR or XeSS alongside NR, including completed work after either addon is disabled. `EAM_PROBE_SCALER_ONLY=1` tests the supplied upscaler without registering an NR callback and checks standalone dispatch diagnostics. Installed files and settings are not changed. |
| `nr_fgeval.exe` (built with `build_all.ps1 -IncludeTools`) | Frame generation scored offline on a recording (`.lsrec`, made with frame generation off): every other frame is rebuilt by AMD's FSR 3.1 (`gen=fsr`) or Intel's XeSS (`gen=xess`, needs `fetch_xess_sdk.ps1`) frame generation, fed this project's motion estimate, and compared with the real one, beside a repeat and a 50/50 blend; the worst frames are saved as pictures. `nr_fgeval <recording> <folder> [gen=] [first=] [count=] [worst=] [depthdir=] [motion=none]`. |
| `fetch_depth_model.ps1` | Fetches Depth Anything V2 Small (ONNX, Apache 2.0, a pinned revision and SHA-256) into `addons\Upscalers\external\depth`: depth from a single picture, for the frame generation experiments. |
| `depth_maps.py` | Depth maps for exported frames (`nr_lsrec export`), with that model on the GPU (DirectML), for `nr_fgeval`'s `depthdir=`. Needs its own Python once: `py -m venv tools\.venv`, then `tools\.venv\Scripts\pip install onnxruntime-directml numpy pillow`. |
| `ui_preview.ps1` | Renders every tab, the addon cards and the addon panels offscreen to PNG. Set `EAM_PREVIEW_CLEAN=1` for the tidy scene used in the README. |
| `make_readme_shots.py` | Crops those renders into `docs/images`. |
| `compare_ui_renders.py` | Compares two folders of offscreen renders picture by picture (used to prove a rewrite of window code changed nothing you can see). |
| `measure_original_share.py` | How much of `manager/src` and `manager/sdk` is still the code this project started from, against a checkout of the original (see NOTICE.md). |
| `package.ps1` | Builds all products, the installer EXE and its SHA-256 checksum. Always verifies installation and extraction against the embedded payload. Requires `-Build <number>`; no source edit or commit is needed. Never packages the user-provided NR model. |
| `package_neural_source.ps1` | Packages GPL corresponding NR source, shaders, SDK headers, exact ImGui dependency and licences. Included in the installer payload. `-NeuralBuildDir` selects the build whose ImGui dependency is packaged. |
| `fetch_xess_sdk.ps1` | Fetches Intel's XeSS SDK (release 3.0.2: headers, `libxess.dll`, and for `nr_fgeval` `libxess_fg.dll` and `libxell.dll`, licence) into `addons\Upscalers\external\xess`, checked by SHA-256 and Intel's signature. Without it the XeSS Upscaler is not built. |
| `deploy.ps1` | Copies a build into a Lossless Scaling folder, with backups. `-What host\|nr\|dlaa\|fsr\|xess\|all`. |
| `gpu_logger.ps1` | `nvidia-smi` once a second to a CSV; stops itself after 45 minutes. |
| `make_echo_icon.py` | Draws the Echo icon (`manager/manager-icon.ico` and `.png`), each size on its own. |
| `bmp2png.py` | BMP to PNG for a folder (needs Pillow). |

## Building an installer

With CMake and the Visual Studio 2022 C++ tools available, run:

```powershell
powershell -NoProfile -File tools/package.ps1 -Build 6
```

The outputs are `dist/LSAddonManager-build-6-x64.exe` and `dist/LSAddonManager-build-6-x64.exe.sha256`. This command rebuilds all products from the current working tree, including uncommitted changes, includes corresponding NR source, and tests installation and extraction using a disposable LS fixture. It never installs into the real Lossless Scaling folder. Documentation and licences are embedded in the EXE and available through `--extract <folder>`; there is no release ZIP.

Packaging requires `-Build` from 1 through 65535. The number is passed to CMake and the compiler, so the manager UI, DLL resources and installer agree without storing a release number in source. Direct CMake builds can use `-DEAM_BUILD_NUMBER=6`; configure with `-DEAM_BUILD_NUMBER=` for an unnumbered development build. `build_all.ps1` clears any cached release number when `-Build` is omitted.

Use the same script for alternate build directories. Paths are relative to the repository root, or may be absolute. For the NR 1.0.2 build developed in this checkout:

```powershell
./tools/package.ps1 -Build 6 -HostBuildDir build/manager-no-security -NeuralBuildDir build/nr-required -SetupBuildDir build/nr-required/installer -Jobs 1 -PackageSuffix nr-1.0.2
```

`-ScalerBuildDir` defaults to `addons/Upscalers/build`. `-Jobs 1` builds serially. `-PackageSuffix` keeps a local package separate from the normal release output without changing the release number. The selected directories are rebuilt, checked against their source directories, and used consistently for binaries and the NR source dependency; no separate staging script is needed.
