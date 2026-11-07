// SPDX-License-Identifier: GPL-3.0-only
#include "picture.h"

#include "bridge.h" // FXC's generated header requires the Windows BYTE definition above.
#include <array>
#include <cstdio>
#include <cstring>
#include <d3d11sdklayers.h>
#include <limits>
#include <vector>
using namespace neural;
namespace {
using Pixel = std::array<float, 4>;
constexpr unsigned W = 257, H = 3; // Exercise partial thread groups and image edges.
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
bool Near(float a, float b, float tolerance = .0001f) {
    return std::abs(a - b) <= tolerance * std::max(1.f, std::abs(b));
}
float Y(const Pixel &p) { return .2126f * p[0] + .7152f * p[1] + .0722f * p[2]; }
struct Test {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> source, output, readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ComputeShader> copy;
    ComPtr<ID3D11Buffer> sentinel;
    ComPtr<ID3D11InfoQueue> messages;
    Picture picture;
    Test() {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &context),
              "Create WARP picture test");
        Check(device.As(&messages), "Get D3D11 validation messages");
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W;
        d.Height = H;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&d, nullptr, &source), "Create source");
        Check(device->CreateShaderResourceView(source.Get(), nullptr, &srv), "Create SRV");
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&d, nullptr, &output), "Create output");
        Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create UAV");
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&d, nullptr, &readback), "Create readback");
        Check(device->CreateComputeShader(g_bridge, sizeof(g_bridge), nullptr, &copy),
              "Create original copy");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb, nullptr, &sentinel), "Create host constants");
    }
    std::vector<Pixel> Read(ID3D11Texture2D *texture) {
        context->CopyResource(readback.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read picture");
        std::vector<Pixel> pixels(W * H);
        for (unsigned y = 0; y < H; ++y)
            memcpy(pixels.data() + y * W, static_cast<char *>(mapped.pData) + y * mapped.RowPitch,
                   W * sizeof(Pixel));
        context->Unmap(readback.Get(), 0);
        return pixels;
    }
    std::vector<Pixel> Run(const std::vector<Pixel> &input, const PictureSettings &s, Encoding encoding,
                           float whiteNits = 80, bool audit = false) {
        context->UpdateSubresource(source.Get(), 0, nullptr, input.data(), W * sizeof(Pixel), 0);
        auto *cb = sentinel.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        ComPtr<ID3D11Query> query;
        if (audit) {
            D3D11_QUERY_DESC d{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            Check(device->CreateQuery(&d, &query), "Create dispatch audit");
            context->Begin(query.Get());
        }
        if (!picture.Apply(context.Get(), srv.Get(), uav.Get(), W, H, encoding, whiteNits, s)) {
            context->CSSetShader(copy.Get(), nullptr, 0);
            auto *view = srv.Get();
            auto *target = uav.Get();
            context->CSSetShaderResources(0, 1, &view);
            context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            context->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            view = nullptr;
            target = nullptr;
            context->CSSetShaderResources(0, 1, &view);
            context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        }
        if (audit)
            context->End(query.Get());
        auto out = Read(output.Get());
        ComPtr<ID3D11Buffer> restored;
        context->CSGetConstantBuffers(0, 1, &restored);
        Require(restored == sentinel, "Picture changed the host constant buffer binding");
        Require(Read(source.Get()) == input, "Picture modified its source");
        for (size_t i = 0; i < out.size(); ++i) {
            for (float v : out[i])
                Require(std::isfinite(v), "Nonfinite picture output");
            Require(out[i][3] == input[i][3], "Picture changed alpha");
        }
        if (audit) {
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            HRESULT status;
            const auto until = GetTickCount64() + 5000;
            do {
                status = context->GetData(query.Get(), &stats, sizeof(stats), 0);
            } while (status == S_FALSE && GetTickCount64() < until);
            Require(status == S_OK && stats.CSInvocations == ((W + 7) / 8) * ((H + 7) / 8) * 64,
                    "Handoff must use exactly one dispatch, whether enabled or bypassed");
        }
        return out;
    }
    void Validate() {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            messages->GetMessage(i, message, &size);
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                puts(message->pDescription);
                throw std::runtime_error("D3D11 validation warning/error");
            }
        }
    }
};
std::vector<Pixel> Flat(Pixel pixel) { return std::vector<Pixel>(W * H, pixel); }
} // namespace
int main() {
    try {
        Test test;
        PictureSettings s;
        auto input = Flat({-.125f, 2.f, 8.f, .375f});
        s.exposure = 1;
        Require(!test.picture.Apply(nullptr, nullptr, nullptr, W, H, Encoding::ScRgb, 80, s),
                "Disabled path touched context");
        Require(test.Run(input, s, Encoding::ScRgb, 80, true) == input, "Disabled copy is not exact");
        s = {};
        s.enabled = true;
        Require(!test.picture.Apply(nullptr, nullptr, nullptr, W, H, Encoding::ScRgb, 80, s),
                "Neutral path touched context");
        Require(test.Run(input, s, Encoding::ScRgb, 80, true) == input && !test.picture.Allocated(),
                "Neutral copy changed pixels or allocated resources");
        for (const auto &c : PictureControls)
            s.*(c.value) = std::numeric_limits<float>::quiet_NaN();
        Require(!test.picture.Apply(nullptr, nullptr, nullptr, W, H, Encoding::ScRgb, 80, s),
                "Invalid settings did not become neutral");
        puts("PASS disabled/neutral: exact copy, no picture allocation or context access, one original "
             "dispatch");

        s = {};
        s.enabled = true;
        s.exposure = 1;
        auto out = test.Run(input, s, Encoding::ScRgb, 203, true);
        for (unsigned c = 0; c < 3; ++c)
            Require(Near(out[0][c], input[0][c] * 2),
                    "HDR exposure is not in stops or clipped signed/high values");
        s.exposure = 20;
        out = test.Run(input, s, Encoding::ScRgb);
        Require(Near(out[0][2], 32), "Exposure bounds were not enforced");
        s.exposure = 1;
        input = Flat({.25f, .25f, .25f, .375f});
        out = test.Run(input, s, Encoding::Sdr709);
        // Reference transfer functions, independent of the shader's tone/colour implementation.
        const float linear = std::pow((.25f + .055f) / 1.055f, 2.4f) * 2;
        const float encoded = 1.055f * std::pow(linear, 1 / 2.4f) - .055f;
        Require(Near(out[0][0], encoded), "SDR exposure operated on encoded values");
        puts("PASS linear-light SDR/scRGB exposure, signed gamut, HDR headroom and bounded settings");

        s = {};
        s.enabled = true;
        s.saturation = 0;
        input = Flat({.1f, .4f, .9f, .375f});
        out = test.Run(input, s, Encoding::ScRgb);
        for (unsigned c = 0; c < 3; ++c)
            Require(Near(out[0][c], Y(input[0])), "Monochrome does not preserve luminance");
        s = {};
        s.enabled = true;
        s.vibrance = 1;
        auto muted = Flat({.3f, .35f, .4f, .375f});
        auto vivid = Flat({0, 0, 1, .375f});
        out = test.Run(muted, s, Encoding::ScRgb);
        Require(out[0][2] - out[0][0] > .1f && Near(Y(out[0]), Y(muted[0])),
                "Vibrance did not lift muted colour at constant luminance");
        out = test.Run(vivid, s, Encoding::ScRgb);
        Require(Near(out[0][2], 1) && Near(out[0][0], 0), "Vibrance pushed a fully saturated colour");
        s = {};
        s.enabled = true;
        s.temperature = 1;
        input = Flat({.18f, .18f, .18f, .375f});
        out = test.Run(input, s, Encoding::ScRgb);
        Require(out[0][0] > out[0][2] && Near(Y(out[0]), .18f), "Warmth direction or luminance is wrong");
        s.temperature = 0;
        s.tint = 1;
        out = test.Run(input, s, Encoding::ScRgb);
        Require(out[0][0] > out[0][1] && out[0][2] > out[0][1] && Near(Y(out[0]), .18f),
                "Tint direction or luminance is wrong");
        puts("PASS saturation, vibrance and neutral-luminance white balance");

        for (bool hdr : {false, true}) {
            std::vector<Pixel> ramp(W * H);
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x) {
                    float v = float(x) / (W - 1) * (hdr ? 16.f : 1.f);
                    ramp[y * W + x] = {v, v, v, .375f};
                }
            for (unsigned mask = 0; mask < 16; ++mask) {
                s = {};
                s.enabled = true;
                s.contrast = (mask & 1) ? 1.f : -1.f;
                s.midtones = (mask & 2) ? 1.f : -1.f;
                s.shadows = (mask & 4) ? 1.f : -1.f;
                s.highlights = (mask & 8) ? 1.f : -1.f;
                out = test.Run(ramp, s, hdr ? Encoding::ScRgb : Encoding::Sdr709);
                Require(Near(out.front()[0], 0), "Tone lifted pure black");
                for (unsigned x = 1; x < W; ++x)
                    Require(out[x][0] >= out[x - 1][0] - 1e-6f, "Tone curve reversed a gradient");
                Require(hdr ? out[W - 1][0] > 1 : Near(out[W - 1][0], 1),
                        "Tone clipped HDR or moved SDR white");
            }
        }
        // Regional controls act most strongly in their named region (compare exposure ratios).
        auto dark = Flat({.01f, .01f, .01f, .375f}), mid = Flat({.18f, .18f, .18f, .375f}),
             bright = Flat({8, 8, 8, .375f});
        s = {};
        s.enabled = true;
        s.shadows = 1;
        Require(test.Run(dark, s, Encoding::ScRgb)[0][0] / .01f >
                    test.Run(bright, s, Encoding::ScRgb)[0][0] / 8,
                "Shadows did not target dark regions");
        s.shadows = 0;
        s.highlights = 1;
        Require(test.Run(bright, s, Encoding::ScRgb)[0][0] / 8 >
                    test.Run(dark, s, Encoding::ScRgb)[0][0] / .01f,
                "Highlights did not target bright regions");
        s.highlights = 0;
        s.midtones = 1;
        Require(test.Run(mid, s, Encoding::ScRgb)[0][0] / .18f >
                    test.Run(bright, s, Encoding::ScRgb)[0][0] / 8,
                "Midtones did not target middle grey");
        puts("PASS 32 extreme tone curves: monotonic gradients, black/white endpoints, HDR headroom and "
             "regional response");

        input.resize(W * H);
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                const float v = x < 126    ? .1f
                                : x == 126 ? .15f
                                : x == 127 ? .3f
                                : x == 128 ? .7f
                                : x == 129 ? .85f
                                           : .9f;
                input[y * W + x] = {v, v, v, .375f};
            }
        s = {};
        s.enabled = true;
        s.sharpen = 1;
        out = test.Run(input, s, Encoding::ScRgb);
        Require(!Near(out[127][0], input[127][0]), "Sharpening did not affect a blurred edge");
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                const float lo = input[y * W + (x ? x - 1 : x)][0],
                            hi = input[y * W + std::min(x + 1, W - 1)][0];
                Require(out[y * W + x][0] >= lo - 1e-6f && out[y * W + x][0] <= hi + 1e-6f,
                        "Sharpening introduced a halo");
            }
        Require(Near(out.front()[0], .1f) && Near(out.back()[0], .9f),
                "Sharpening changed a flat area or image border");
        puts("PASS sharpening changes blurred detail without overshoot, flat-field shifts or border seams");

        for (unsigned combination = 0; combination < 32; ++combination) {
            s = {};
            s.enabled = true;
            unsigned i = 0;
            for (const auto &c : PictureControls)
                s.*(c.value) = (combination & (1u << (i++ % 5))) ? c.high : c.low;
            for (unsigned p = 0; p < input.size(); ++p)
                input[p] = {float(int(p % 29) - 4) * 2, float(p % 13) * 3, float(int(p % 7) - 2) * 4, .375f};
            input[0] = {0, 0, 0, .375f};
            input[1] = {65504, 65504, 65504, .375f};
            test.Run(input, s, Encoding::ScRgb);
        }
        s = {};
        s.enabled = true;
        Require(test.Run(input, s, Encoding::ScRgb) == input, "Neutral after active is not exact");
        s.enabled = false;
        s.exposure = 2;
        Require(test.Run(input, s, Encoding::ScRgb) == input, "Disabled after active is not exact");
        test.Validate();
        puts("PICTURE TEST PASSED; finite extreme combinations, unchanged alpha/source, restored constants, "
             "zero D3D11 warnings/errors");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "PICTURE TEST ERROR: %s\n", e.what());
        return 1;
    }
}
