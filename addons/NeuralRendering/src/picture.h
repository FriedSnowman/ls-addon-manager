// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "composition.h"
#include <algorithm>
#include <cmath>
#include <d3d11.h>

namespace neural {
struct PictureSettings {
    bool enabled = false;
    float exposure = 0, contrast = 0, midtones = 0, shadows = 0, highlights = 0;
    float saturation = 1, vibrance = 0, temperature = 0, tint = 0, sharpen = 0;
    PictureSettings Bounded() const;
    bool Active() const;
};
struct PictureControl {
    const char *key, *label;
    float PictureSettings::*value;
    float low, high, neutral;
    const char *format, *help;
};
inline constexpr PictureControl PictureControls[] = {
    {"pictureExposure", "Exposure", &PictureSettings::exposure, -4, 4, 0, "%+.2f stops",
     "Overall light level. +1 stop doubles the light; -1 halves it. Strong adjustments can clip SDR whites."},
    {"pictureContrast", "Contrast", &PictureSettings::contrast, -2, 2, 0, "%+.2f",
     "Separates light and dark around middle grey. Negative values soften contrast."},
    {"pictureMidtones", "Midtones", &PictureSettings::midtones, -2, 2, 0, "%+.2f",
     "Brightens or darkens the middle tones, with less effect on deep shadows and bright highlights."},
    {"pictureShadows", "Shadows", &PictureSettings::shadows, -2, 2, 0, "%+.2f",
     "Lifts or deepens dark areas. Pure black stays black."},
    {"pictureHighlights", "Highlights", &PictureSettings::highlights, -2, 2, 0, "%+.2f",
     "Raises or lowers bright areas. Cannot recover detail already clipped in the source."},
    {"pictureSaturation", "Saturation", &PictureSettings::saturation, 0, 4, 1, "%.2f",
     "Overall colour strength. 1 is unchanged; 0 is monochrome."},
    {"pictureVibrance", "Vibrance", &PictureSettings::vibrance, -1, 2, 0, "%+.2f",
     "Boosts muted colours more than already vivid colours. Negative values mute them. This does not detect skin."},
    {"pictureTemperature", "Temperature", &PictureSettings::temperature, -2, 2, 0, "%+.2f",
     "Relative colour balance: negative is cooler, positive is warmer."},
    {"pictureTint", "Tint", &PictureSettings::tint, -2, 2, 0, "%+.2f",
     "Relative colour balance: negative is greener, positive is more magenta."},
    {"pictureSharpen", "Sharpening", &PictureSettings::sharpen, 0, 4, 0, "%.2f",
     "Sharpens at capture resolution. 0 to 1 suppresses halos; above 1 allows increasingly strong edge overshoot. "
     "High values can amplify noise and halos. Later scaling also affects sharpness."}};
inline PictureSettings PictureSettings::Bounded() const {
    auto result = *this;
    for (const auto &c : PictureControls) {
        auto &v = result.*(c.value);
        v = std::isfinite(v) ? std::clamp(v, c.low, c.high) : c.neutral;
    }
    return result;
}
inline bool PictureSettings::Active() const {
    if (!enabled)
        return false;
    for (const auto &c : PictureControls)
        if (this->*(c.value) != c.neutral)
            return true;
    return false;
}

// An alternative to the final D3D11 copy, not an additional image pass.
// No resources are created and no context calls are made for disabled/neutral settings.
class Picture {
  public:
    bool Apply(ID3D11DeviceContext *, ID3D11ShaderResourceView *, ID3D11UnorderedAccessView *, unsigned width,
               unsigned height, Encoding, float whiteNits, const PictureSettings &);
    bool Allocated() const { return shader_ != nullptr; }

  private:
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};
} // namespace neural
