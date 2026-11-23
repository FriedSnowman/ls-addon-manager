// SPDX-License-Identifier: GPL-3.0-only
// Dark area protection, run through D3D11 WARP with validation: dark captures come back as captured at
// full strength and partly at half, bright ones are never written, the fade sits between half the
// threshold and the threshold, scRGB is judged relative to the reference white, strength 0 dispatches
// nothing, alpha survives, and the host's constant buffer binding is restored.
#include "dark_keep.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11sdklayers.h>
#include <vector>
using namespace neural;
namespace {
using Pixel = std::array<float, 4>;
constexpr unsigned W = 67, H = 9; // partial thread groups and edges
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
float SrgbToLinear(float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
struct Test {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> original, target, readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> sentinel;
    ComPtr<ID3D11InfoQueue> messages;
    DarkKeep keep;
    Test() {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &context),
              "Create WARP dark protection test");
        Check(device.As(&messages), "Get D3D11 validation messages");
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W;
        d.Height = H;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&d, nullptr, &original), "Create original");
        Check(device->CreateShaderResourceView(original.Get(), nullptr, &srv), "Create SRV");
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&d, nullptr, &target), "Create target");
        Check(device->CreateUnorderedAccessView(target.Get(), nullptr, &uav), "Create UAV");
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&d, nullptr, &readback), "Create readback");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb, nullptr, &sentinel), "Create host constants");
    }
    std::vector<Pixel> Run(const std::vector<Pixel> &captured, const std::vector<Pixel> &composed, float strength,
                           float threshold, bool scRgb = false, float white = 203) {
        context->UpdateSubresource(original.Get(), 0, nullptr, captured.data(), W * sizeof(Pixel), 0);
        context->UpdateSubresource(target.Get(), 0, nullptr, composed.data(), W * sizeof(Pixel), 0);
        auto *cb = sentinel.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        keep.Apply(context.Get(), srv.Get(), uav.Get(), W, H, strength, threshold, scRgb, white);
        ComPtr<ID3D11Buffer> bound;
        context->CSGetConstantBuffers(0, 1, &bound);
        Require(bound == sentinel, "Dark protection changed the host constant buffer binding");
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read target");
        std::vector<Pixel> out(W * H);
        for (unsigned y = 0; y < H; ++y)
            memcpy(out.data() + y * W, static_cast<char *>(mapped.pData) + y * mapped.RowPitch, W * sizeof(Pixel));
        context->Unmap(readback.Get(), 0);
        Require(messages->GetNumStoredMessages() == 0, "D3D11 validation reported a problem");
        return out;
    }
};
std::vector<Pixel> Flat(float grey, float alpha = 1) { return std::vector<Pixel>(W * H, Pixel{grey, grey, grey, alpha}); }
bool Near(float a, float b) { return std::abs(a - b) <= 1e-5f * std::max(1.f, std::abs(b)); }
} // namespace

int main() {
    try {
        Test test;
        const auto composed = Flat(.5f, .75f); // what the model made: always mid grey, alpha .75

        // 1. A dark capture (well under half the threshold) comes back exactly at full strength, alpha kept.
        auto out = test.Run(Flat(.04f), composed, 1, .2f);
        for (const auto &p : out)
            Require(p[0] == .04f && p[1] == .04f && p[2] == .04f && p[3] == .75f, "Dark pixel not restored exactly");
        // ... and half-way at half strength.
        out = test.Run(Flat(.04f), composed, .5f, .2f);
        for (const auto &p : out)
            Require(Near(p[0], .27f) && p[3] == .75f, "Half strength is not half-way");
        puts("PASS dark capture restored (exactly at 100%, half-way at 50%), alpha kept");

        // 2. A capture at or above the threshold is never written.
        for (const float grey : {.2f, .35f, .9f}) {
            out = test.Run(Flat(grey), composed, 1, .2f);
            for (const auto &p : out)
                Require(p == Pixel{.5f, .5f, .5f, .75f}, "A capture above the threshold was written");
        }
        puts("PASS captures at or above the threshold untouched");

        // 3. In the fade (between half the threshold and the threshold) it is partial and falls with brightness.
        float last = 2;
        for (const float grey : {.11f, .13f, .15f, .17f, .19f}) {
            out = test.Run(Flat(grey), composed, 1, .2f);
            const float m = (out[0][0] - .5f) / (grey - .5f); // how much of the capture came back
            Require(m > 0 && m < 1 && m < last, "The fade is not partial and falling");
            last = m;
        }
        puts("PASS fade between half the threshold and the threshold");

        // 4. Strength 0 writes nothing, whatever the capture.
        out = test.Run(Flat(0), composed, 0, .2f);
        for (const auto &p : out)
            Require(p == Pixel{.5f, .5f, .5f, .75f}, "Strength 0 wrote");
        puts("PASS strength 0 untouched");

        // 5. scRGB is judged relative to the reference white: the same displayed brightness as an SDR .04 / .3
        //    is restored / untouched whatever the white, and a negative (wide-gamut) channel survives.
        for (const float white : {80.f, 203.f, 400.f}) {
            const float scale = white / 80.f;
            auto dark = Flat(SrgbToLinear(.04f) * scale);
            for (auto &p : dark)
                p[0] = -.001f;
            out = test.Run(dark, composed, 1, .2f, true, white);
            for (unsigned i = 0; i < W * H; ++i)
                Require(out[i][0] == -.001f && out[i][1] == dark[i][1], "Dark scRGB pixel not restored exactly");
            out = test.Run(Flat(SrgbToLinear(.3f) * scale), composed, 1, .2f, true, white);
            for (const auto &p : out)
                Require(p == Pixel{.5f, .5f, .5f, .75f}, "Bright scRGB pixel written");
        }
        puts("PASS scRGB judged relative to the reference white; wide-gamut channels kept");

        // 6. A lone bright speck in the dark is still protected (3x3 average), so it does not punch a hole.
        auto speck = Flat(.02f);
        speck[2 * W + 30] = {1, 1, 1, 1};
        out = test.Run(speck, composed, 1, .2f);
        Require(out[2 * W + 30][0] > .5f, "A lone bright speck in the dark lost its protection");
        puts("PASS lone bright speck stays protected");

        puts("DARK PROTECTION TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "DARK PROTECTION TEST FAILED: %s\n", e.what());
        return 1;
    }
}
