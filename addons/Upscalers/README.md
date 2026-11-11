# Standalone upscalers

DLSS, FSR and XeSS share this source tree. No neural-rendering engine is built here. See [the upscaler guide](docs/upscalers.md).

NIS replacement requires manager API 1.3 or newer and the verified LS 3.2.2.0 SDR/HDR shaders. Shader identity is checked before reading or caching NIS viewports: unrelated LSFG passes can retain the same coefficient textures. Unknown shaders leave Lossless Scaling's work intact. The upscalers operate at the later NIS stage; DLSS Neural Rendering still processes captures before LSFG.

`nr_nistest` covers false NIS matches, HDR, viewport readback across intervening LSFG passes and preservation of an NR-transformed input. `nr_statustest` checks status publication without capture or Settings UI, failures and reporter shutdown/restart. Both run in the `scalers` suite of `tools/run_addon_tests.ps1`.
