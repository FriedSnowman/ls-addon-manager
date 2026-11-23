// SPDX-License-Identifier: GPL-3.0-only
// The scRGB wide-gamut restore, run through D3D11 WARP with validation: a pixel the composition left
// alone comes back exactly as captured, in-gamut pixels are never written, a changed pixel gets its
// clipped part back scaled by the change in light (within the guard), alpha survives, and the host's
// constant buffer binding is restored.
#include "gamut_restore.h"
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
constexpr unsigned W = 67, H = 5; // partial thread groups and edges
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
float Y(const Pixel &p) { return .2126f * p[0] + .7152f * p[1] + .0722f * p[2]; }
Pixel Kept(const Pixel &p) { return {std::max(p[0], 0.f), std::max(p[1], 0.f), std::max(p[2], 0.f), p[3]}; }
struct Test {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> original, target, readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> sentinel;
    ComPtr<ID3D11InfoQueue> messages;
    GamutRestore restore;
    Test() {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &context),
              "Create WARP gamut test");
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
    std::vector<Pixel> Run(const std::vector<Pixel> &captured, const std::vector<Pixel> &composed, float maxRatio) {
        context->UpdateSubresource(original.Get(), 0, nullptr, captured.data(), W * sizeof(Pixel), 0);
        context->UpdateSubresource(target.Get(), 0, nullptr, composed.data(), W * sizeof(Pixel), 0);
        auto *cb = sentinel.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        restore.Apply(context.Get(), srv.Get(), uav.Get(), W, H, maxRatio);
        ComPtr<ID3D11Buffer> bound;
        context->CSGetConstantBuffers(0, 1, &bound);
        Require(bound == sentinel, "Restore changed the host constant buffer binding");
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
} // namespace

int main() {
    try {
        Test test;
        // A mix of in-gamut colours, wide-gamut ones (one or two negative channels, as scRGB stores colours
        // outside BT.709), highlights well above SDR white, and near-black wide-gamut pixels.
        std::vector<Pixel> captured(W * H);
        for (unsigned i = 0; i < W * H; ++i) {
            const float t = float(i % 17) / 16.f, s = float(i % 5);
            switch (i % 4) {
            case 0: captured[i] = {.1f + t, .2f + .5f * t, .05f + s * .3f, 1}; break;              // in gamut
            case 1: captured[i] = {-.08f * (1 + s), .4f + 3 * t, .1f + t, .5f}; break;            // saturated green
            case 2: captured[i] = {2 + 6 * t, -.05f - .02f * s, -.01f * s, 1}; break;             // bright saturated red
            default: captured[i] = {-.001f * s, .002f * t, -.0005f, .25f}; break;                 // near-black, out of gamut
            }
        }
        std::vector<Pixel> composed(W * H);

        // 1. The composition changed nothing: every pixel comes back exactly as captured.
        for (unsigned i = 0; i < W * H; ++i)
            composed[i] = Kept(captured[i]);
        auto out = test.Run(captured, composed, 2);
        for (unsigned i = 0; i < W * H; ++i)
            Require(out[i] == captured[i], "An unchanged pixel did not come back exactly as captured");
        puts("PASS unchanged pixels restored bit-exactly, including negative channels and alpha");

        // 2. In-gamut captures are never written, whatever the composition did.
        for (unsigned i = 0; i < W * H; ++i)
            composed[i] = {Kept(captured[i])[0] * 1.7f + .01f, Kept(captured[i])[1] * .6f, 3.f, captured[i][3]};
        out = test.Run(captured, composed, 2);
        for (unsigned i = 0; i < W * H; ++i)
            if (i % 4 == 0)
                Require(out[i] == composed[i], "An in-gamut pixel was written");
        puts("PASS in-gamut pixels untouched");

        // 3. A uniformly brightened or darkened pixel gets its clipped part scaled the same way (inside the
        //    guard), so the whole captured colour is scaled; outside the guard the ratio is bounded.
        for (const float scale : {1.5f, .7f, 4.f, .1f}) {
            const float guard = 2;
            for (unsigned i = 0; i < W * H; ++i) {
                const auto k = Kept(captured[i]);
                composed[i] = {k[0] * scale, k[1] * scale, k[2] * scale, k[3]};
            }
            out = test.Run(captured, composed, guard);
            for (unsigned i = 0; i < W * H; ++i) {
                if (i % 4 == 0 || i % 4 == 3)
                    continue; // in gamut, or dominated by the near-black floor
                const auto k = Kept(captured[i]);
                const float floorY = 1.f / 1024;
                const float ratio = std::clamp((Y(composed[i]) + floorY) / (Y(k) + floorY), 1 / guard, guard);
                for (unsigned c = 0; c < 3; ++c) {
                    const float expected = composed[i][c] + std::min(captured[i][c], 0.f) * ratio;
                    Require(std::abs(out[i][c] - expected) <= 1e-5f * std::max(1.f, std::abs(expected)),
                            "Clipped part not scaled by the bounded change in light");
                }
                Require(out[i][3] == composed[i][3], "Restore changed alpha");
            }
        }
        puts("PASS clipped part follows the model's change in light within the luminance guard");

        // 4. Near-black wide-gamut pixels the model brightened stay finite and bounded.
        for (unsigned i = 0; i < W * H; ++i)
            composed[i] = {.5f, .5f, .5f, captured[i][3]};
        out = test.Run(captured, composed, 8);
        for (unsigned i = 0; i < W * H; ++i)
            for (unsigned c = 0; c < 3; ++c) {
                Require(std::isfinite(out[i][c]), "Nonfinite restore output");
                Require(out[i][c] >= .5f + std::min(captured[i][c], 0.f) * 8 - 1e-5f, "Restore exceeded the guard");
            }
        puts("PASS near-black and heavily changed pixels finite and bounded");
        puts("GAMUT RESTORE TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}
