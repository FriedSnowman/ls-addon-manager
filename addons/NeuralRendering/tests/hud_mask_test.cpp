// SPDX-License-Identifier: GPL-3.0-only
// HUD protection through D3D11 WARP with validation: a still block over a changing scene is learned
// and comes back as captured, the scene is left to the model, a still scene teaches nothing, a HUD
// element that starts changing is released, strength 0 writes nothing, and the host's constant
// buffer binding is restored. Run with SDR (8-bit) and scRGB (FP16) captures.
#include "hud_mask.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11sdklayers.h>
#include <random>
#include <stdexcept>
#include <vector>
using namespace neural;
namespace {
using Pixel = std::array<float, 4>;
constexpr unsigned W = 133, H = 96; // partial thread groups
constexpr unsigned HudX0 = 50, HudX1 = 80, HudY0 = 30, HudY1 = 50;
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
bool InHud(unsigned x, unsigned y) { return x >= HudX0 && x < HudX1 && y >= HudY0 && y < HudY1; }
bool Interior(unsigned x, unsigned y) { return x >= HudX0 + 2 && x < HudX1 - 2 && y >= HudY0 + 2 && y < HudY1 - 2; }
bool Far(unsigned x, unsigned y) { return x + 3 < HudX0 || x >= HudX1 + 3 || y + 3 < HudY0 || y >= HudY1 + 3; }
struct Test {
    DXGI_FORMAT format;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> original, target, readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> sentinel;
    ComPtr<ID3D11InfoQueue> messages;
    HudMask hud;
    explicit Test(DXGI_FORMAT f) : format(f) {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &context),
              "Create WARP HUD test");
        Check(device.As(&messages), "Get D3D11 validation messages");
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W;
        d.Height = H;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = format;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&d, nullptr, &original), "Create original");
        Check(device->CreateShaderResourceView(original.Get(), nullptr, &srv), "Create SRV");
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // the pipeline's composed image
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
    void Upload(ID3D11Texture2D *texture, DXGI_FORMAT f, const std::vector<Pixel> &pixels) {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM) {
            std::vector<uint32_t> packed(W * H);
            for (size_t i = 0; i < packed.size(); ++i)
                for (unsigned c = 0; c < 4; ++c)
                    packed[i] |= uint32_t(std::lround(std::clamp(pixels[i][c], 0.f, 1.f) * 255)) << (8 * c);
            context->UpdateSubresource(texture, 0, nullptr, packed.data(), W * 4, 0);
        } else {
            std::vector<uint16_t> half(W * H * 4);
            for (size_t i = 0; i < W * H; ++i)
                for (unsigned c = 0; c < 4; ++c)
                    half[i * 4 + c] = DirectX::PackedVector::XMConvertFloatToHalf(pixels[i][c]);
            context->UpdateSubresource(texture, 0, nullptr, half.data(), W * 8, 0);
        }
    }
    // One capture: the original, the model's image (a flat grey), then HUD protection.
    std::vector<Pixel> Frame(const std::vector<Pixel> &captured, float strength = 1, bool show = false) {
        Upload(original.Get(), format, captured);
        Upload(target.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, std::vector<Pixel>(W * H, Pixel{.25f, .25f, .25f, 1}));
        auto *cb = sentinel.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        hud.Apply(context.Get(), original.Get(), srv.Get(), uav.Get(), W, H, strength, show);
        ComPtr<ID3D11Buffer> bound;
        context->CSGetConstantBuffers(0, 1, &bound);
        Require(bound == sentinel, "HUD protection changed the host constant buffer binding");
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read target");
        std::vector<Pixel> out(W * H);
        for (unsigned y = 0; y < H; ++y) {
            const auto *row = reinterpret_cast<const uint16_t *>(static_cast<char *>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < W; ++x)
                for (unsigned c = 0; c < 4; ++c)
                    out[y * W + x][c] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
        }
        context->Unmap(readback.Get(), 0);
        Require(messages->GetNumStoredMessages() == 0, "D3D11 validation reported a problem");
        return out;
    }
};
bool Near(float a, float b) { return std::abs(a - b) <= 1.f / 255 + 1e-3f * std::abs(b); }
// Scene changes every frame; the HUD block is a fixed pattern (seeded per pixel, not per frame).
std::vector<Pixel> Capture(std::mt19937 &rng, bool sceneMoves, unsigned frame, float hudShift = 0) {
    std::uniform_real_distribution<float> scene(.2f, 1.f);
    std::vector<Pixel> p(W * H);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            if (InHud(x, y)) {
                const float v = ((x * 7 + y * 13) % 5) / 5.f * .8f + .1f + hudShift;
                p[y * W + x] = {v, 1 - v, .5f, 1};
            } else if (sceneMoves) {
                p[y * W + x] = {scene(rng), scene(rng), scene(rng), 1};
            } else {
                const float v = ((x * 3 + y * 5 + frame * 0) % 11) / 11.f * .8f + .2f;
                p[y * W + x] = {v, v * .5f, 1 - v, 1};
            }
        }
    return p;
}
const Pixel &Grey() {
    static const Pixel g{.25f, .25f, .25f, 1};
    return g;
}
void Run(DXGI_FORMAT format, const char *name) {
    Test t(format);
    std::mt19937 rng(7);

    // A still scene teaches nothing, however long it lasts.
    for (unsigned f = 0; f < 80; ++f) {
        const auto out = t.Frame(Capture(rng, false, f));
        for (const auto &px : out)
            Require(px == Grey(), "A still scene was taken for HUD");
    }
    printf("PASS %s: still scene leaves the model image alone\n", name);

    // Camera moving behind a still HUD: after about a second it is restored, the scene never is.
    std::vector<Pixel> captured, out;
    for (unsigned f = 0; f < 60; ++f) {
        captured = Capture(rng, true, f);
        out = t.Frame(captured);
    }
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const auto &o = out[y * W + x], &c = captured[y * W + x];
            if (Interior(x, y))
                for (unsigned ch = 0; ch < 3; ++ch)
                    Require(Near(o[ch], c[ch]), "A learned HUD pixel was not restored to the capture");
            if (Far(x, y))
                Require(o == Grey(), "A moving scene pixel was replaced by the capture");
            Require(o[3] == 1, "HUD protection changed alpha");
        }
    printf("PASS %s: still HUD over a moving scene restored, scene left to the model\n", name);

    // The debug view tints the detected area instead.
    out = t.Frame(captured = Capture(rng, true, 60), 1, true);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x)
            if (Interior(x, y))
                Require(out[y * W + x][0] > .6f && out[y * W + x][1] < .2f, "Mask view did not tint the HUD");
    // Strength 0 writes nothing.
    out = t.Frame(Capture(rng, true, 61), 0);
    for (const auto &px : out)
        Require(px == Grey(), "Strength 0 wrote a pixel");
    printf("PASS %s: mask view and zero strength\n", name);

    // A HUD element that starts changing (a counter ticking) is released within a few frames.
    for (unsigned f = 0; f < 6; ++f)
        out = t.Frame(Capture(rng, true, 62 + f, (f % 2) * .3f + .05f));
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x)
            if (Interior(x, y))
                Require(out[y * W + x] == Grey(), "A changing HUD pixel was still restored");
    printf("PASS %s: changing HUD released\n", name);
}
} // namespace

int main() {
    try {
        Run(DXGI_FORMAT_R8G8B8A8_UNORM, "SDR");
        Run(DXGI_FORMAT_R16G16B16A16_FLOAT, "scRGB");
        puts("HUD PROTECTION TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}
