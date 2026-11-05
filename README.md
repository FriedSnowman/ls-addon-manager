# LS Addon Manager

A local addon manager for Lossless Scaling. The manager includes ReShade input passthrough, windowed output, performance metrics, settings and a transactional installer.

The standalone DLSS, FSR and XeSS upscalers are in [addons/Upscalers](addons/Upscalers). Choose NIS in Lossless Scaling and enable one upscaler. Their vendor requirements and controls are documented in the [upscaler guide](addons/Upscalers/docs/upscalers.md).

The old DLSS5NR01 plugin has been retired. It is removed from this checkout and ignored by the manager on upgrade; user models and settings are preserved. The NVIDIA-only replacement is being implemented according to the [approved plan](docs/dlss-nr-replacement-plan.md). Do not use old NR binaries with this build.

Build instructions: [LOCAL_BUILD.md](docs/LOCAL_BUILD.md). Addon SDK: [addon authors](docs/addon-authors.md). Third-party notices: [NOTICE.md](NOTICE.md). Historical NR documentation is labelled and retained under [docs/history/retired-nr](docs/history/retired-nr).
