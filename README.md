# LS Addon Manager

A local addon manager for Lossless Scaling. The manager includes ReShade input passthrough, windowed output, performance metrics, settings and a transactional installer.

The standalone DLSS, FSR and XeSS upscalers are in [addons/Upscalers](addons/Upscalers). Choose NIS in Lossless Scaling and enable one upscaler. Their vendor requirements and controls are documented in the [upscaler guide](addons/Upscalers/docs/upscalers.md).

[DLSS Neural Rendering](addons/NeuralRendering/README.md) is a fresh NVIDIA-only addon: original capture -> independent NVIDIA Optical Flow -> NR -> pinned OptiScaler composition -> LSFG. Enable WGC and use the validated LS/model builds described in its guide. The model is user-provided. Multipass and AMD NR are excluded; standalone upscalers remain supported.

The old DLSS5NR01 implementation is completely retired. Upgrade backs up its owned executables and preserves user models/settings. New settings live under DLSSNR02, with no reinterpretation of the old values. The [implementation log](docs/dlss-nr-implementation-log.md) records tests, compatibility limits and departures from the [approved plan](docs/dlss-nr-replacement-plan.md).

Build instructions: [LOCAL_BUILD.md](docs/LOCAL_BUILD.md). Addon SDK: [addon authors](docs/addon-authors.md). Third-party notices: [NOTICE.md](NOTICE.md). Historical NR documentation is labelled and retained under [docs/history/retired-nr](docs/history/retired-nr).
