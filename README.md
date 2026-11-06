# LS Addon Manager

A local addon manager for Lossless Scaling. The manager includes ReShade input passthrough, windowed output, performance metrics, settings and a transactional installer.

The standalone DLSS, FSR and XeSS upscalers are in [addons/Upscalers](addons/Upscalers). Choose NIS in Lossless Scaling and enable one upscaler. Their vendor requirements and controls are documented in the [upscaler guide](addons/Upscalers/docs/upscalers.md).

[DLSS Neural Rendering](addons/NeuralRendering/README.md) is a fresh NVIDIA-only addon: original capture -> independent NVIDIA Optical Flow -> NR -> pinned OptiScaler composition -> LSFG. Enable WGC and use the validated LS/model builds described in its guide. The model is user-provided. Multipass and AMD NR are excluded; standalone upscalers remain supported.

The old DLSS5NR01 implementation is retired. Upgrade backs up its owned executables and preserves user models/settings. New settings live under DLSSNR02.

Build with Visual Studio 2022 C++ Build Tools, the Windows SDK and CMake 3.20 or newer:

```powershell
powershell -NoProfile -File tools/ci.ps1
```

This builds the manager, installer, neural rendering addon and sample addon, and runs the tests that need no GPU or game. Standalone upscalers also need the vendor SDKs described in [addons/Upscalers](addons/Upscalers).

Addon SDK: [addon authors](docs/addon-authors.md). Build and packaging tools: [tools](tools/README.md). Third-party notices: [NOTICE.md](NOTICE.md).
