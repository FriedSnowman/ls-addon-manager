# Tools

Scripts to build, test, look at and package LS Addon Manager. They find the repository from their own location, so run them from anywhere.
Where a script needs your Lossless Scaling folder it takes `-LsDir` (or reads the `LS_DIR` environment variable); the default is the usual Steam path.

**Rules the tools follow**

- UI previews render offscreen. GUI and real-capture tests can create their own temporary windows; they do not modify games.
- `deploy.ps1` never replaces files under a running game: it refuses while the game or Lossless Scaling runs, and backs up what it overwrites.
- NR hardware tests use the GPU and run serially. Run them outside an active gameplay session.

| Tool | What it does |
|---|---|
| `build_all.ps1` | Configures and builds manager, independent NR, preserved upscalers and UI preview, Release x64. `-Only host,neural,scalers`. |
| `run_addon_tests.ps1` | Offline tests, no game needed, in suites (core, features, sample, gui, installer, setupexe, nr). By default it builds and runs only the suites that the files changed since the last commit can affect, and prints only failures and the time each suite took. `-Only core,gui` picks suites, `-All` runs every one (before a release), `-List` shows what starts each. |
| `run_addon_tests.ps1 -Only nr -ModelPath <file>` | Runs independent flow, pinned OptiScaler parity, model and pipeline tests. Without a model path, runs flow/composition only. The retired host matrix is removed. |
| `nr_fgeval.exe` (built with the addons) | Frame generation scored offline on a recording (`.lsrec`, made with frame generation off): every other frame is rebuilt by AMD's FSR 3.1 (`gen=fsr`) or Intel's XeSS (`gen=xess`, needs `fetch_xess_sdk.ps1`) frame generation, fed this project's motion estimate, and compared with the real one, beside a repeat and a 50/50 blend; the worst frames are saved as pictures. `nr_fgeval <recording> <folder> [gen=] [first=] [count=] [worst=] [depthdir=] [motion=none]`. |
| `fetch_depth_model.ps1` | Fetches Depth Anything V2 Small (ONNX, Apache 2.0, a pinned revision and SHA-256) into `addons\Upscalers\external\depth`: depth from a single picture, for the frame generation experiments. |
| `depth_maps.py` | Depth maps for exported frames (`nr_lsrec export`), with that model on the GPU (DirectML), for `nr_fgeval`'s `depthdir=`. Needs its own Python once: `py -m venv tools\.venv`, then `tools\.venv\Scripts\pip install onnxruntime-directml numpy pillow`. |
| `ui_preview.ps1` | Renders every tab, the addon cards and the addon panels offscreen to PNG. Set `EAM_PREVIEW_CLEAN=1` for the tidy scene used in the README. |
| `make_readme_shots.py` | Crops those renders into `docs/images`. |
| `compare_ui_renders.py` | Compares two folders of offscreen renders picture by picture (used to prove a rewrite of window code changed nothing you can see). |
| `measure_original_share.py` | How much of `manager/src` and `manager/sdk` is still the code this project started from, against a checkout of the original (see NOTICE.md). |
| `package.ps1` | Builds all products and a verified installer zip in `dist\` (`-Version`). No stale build fallback. Never packages the user-provided NR model. |
| `package_neural_source.ps1` | Packages GPL corresponding NR source, shaders, SDK headers, exact ImGui dependency and licences. Included in the installer payload. |
| `fetch_xess_sdk.ps1` | Fetches Intel's XeSS SDK (release 3.0.2: headers, `libxess.dll`, and for `nr_fgeval` `libxess_fg.dll` and `libxell.dll`, licence) into `addons\Upscalers\external\xess`, checked by SHA-256 and Intel's signature. Without it the XeSS Upscaler is not built. |
| `deploy.ps1` | Copies a build into a Lossless Scaling folder, with backups. `-What host\|nr\|dlaa\|fsr\|xess\|all`. |
| `gpu_logger.ps1` | `nvidia-smi` once a second to a CSV; stops itself after 45 minutes. |
| `make_echo_icon.py` | Draws the Echo icon (`manager/manager-icon.ico` and `.png`), each size on its own. |
| `bmp2png.py` | BMP to PNG for a folder (needs Pillow). |
