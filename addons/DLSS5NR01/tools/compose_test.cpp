// Executes the real compositor on WARP. Synthetic deltas make misplaced enhancement visible without loading a model or a game.
#include "addon/compose11.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
static int failed = 0;
static void Check(const char* name, bool ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; }
static void Require(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }

struct Fixture {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> delta, flow, motion;
    ComPtr<ID3D11ShaderResourceView> deltaView, motionView;
    Compose11 compose;
    Fixture() {
        Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
        if (!compose.Init(dev.Get(), [](const char* s) { puts(s); })) throw std::runtime_error("shader compilation failed");
        std::vector<float> pixels(16 * 16 * 4);
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const int i = (y * 16 + x) * 4;
            pixels[i] = 0.02f + 0.15f * (x + 0.5f) / 16;
            pixels[i + 1] = 0.02f + 0.15f * (y + 0.5f) / 16;
            pixels[i + 2] = 0.01f;
        }
        delta = Texture(16, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, pixels.data(), 16 * 16);
        Require(dev->CreateShaderResourceView(delta.Get(), nullptr, &deltaView));
        // A spatially varying, consistent flow: y displacement = clamp(2*v - .25, 0, 1.5), in flow units.
        const uint16_t halves[] = { 0, 0x3800, 0x3c00, 0x3e00 };
        std::vector<uint16_t> flows(4 * 4 * 4);
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            flows[(y * 4 + x) * 4 + 1] = y ? halves[y] | 0x8000 : 0;
            flows[(y * 4 + x) * 4 + 3] = halves[y];
        }
        flow = Texture(4, 4, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, flows.data(), 4 * 8);
        std::vector<float> moves(16 * 16 * 2);
        for (int i = 0; i < 16 * 16; ++i) moves[i * 2 + 1] = 1.6f; // +0.1 in normalized game coordinates
        motion = Texture(16, 16, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, moves.data(), 16 * 8);
        Require(dev->CreateShaderResourceView(motion.Get(), nullptr, &motionView));
    }
    ~Fixture() { compose.Shutdown(); }
    ComPtr<ID3D11Texture2D> Texture(UINT w, UINT h, DXGI_FORMAT format, UINT bind, const void* data, UINT pitch) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format; d.SampleDesc.Count = 1; d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA initial{ data, pitch, 0 };
        ComPtr<ID3D11Texture2D> t;
        Require(dev->CreateTexture2D(&d, data ? &initial : nullptr, &t));
        return t;
    }
    std::vector<uint8_t> Read(ID3D11Texture2D* t, UINT bpp) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.BindFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        Require(dev->CreateTexture2D(&d, nullptr, &staging));
        ctx->CopyResource(staging.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        Require(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
        std::vector<uint8_t> bytes(size_t(d.Width) * d.Height * bpp);
        for (UINT y = 0; y < d.Height; ++y) memcpy(bytes.data() + size_t(y) * d.Width * bpp, (const uint8_t*)m.pData + size_t(y) * m.RowPitch, d.Width * bpp);
        ctx->Unmap(staging.Get(), 0);
        return bytes;
    }
};

static bool Inside(UINT x, UINT y, const Compose11::Rect& r) { return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h; }

static void SdrCase(Fixture& f, const char* name, UINT w, UINT h, Compose11::Rect rect, bool direct, int movement = 0, bool controls = false, UINT debug = 0) {
    const Compose11::Rect area = rect.w ? rect : Compose11::Rect{ 0, 0, w, h };
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = (size_t(y) * w + x) * 4;
        // Some border pixels are non-black, to catch clearing instead of preserving. Alpha must survive too.
        for (int c = 0; c < 3; ++c) pixels[i + c] = Inside(x, y, area) ? 64 : ((x + y) % 7 == 0 ? 13 + c : 0);
        pixels[i + 3] = 191;
    }
    auto target = f.Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, direct ? D3D11_BIND_UNORDERED_ACCESS : 0, pixels.data(), w * 4);
    Compose11::Args a;
    a.target = target.Get(); a.delta = f.deltaView.Get(); a.content = rect; a.debugView = debug; a.isGen = true;
    if (movement == 1 || movement == -1) { a.flow = f.flow.Get(); a.flowW = a.flowH = 4; a.offset = float(movement); }
    if (movement == 2) { a.motion = f.motionView.Get(); a.offset = 1; }
    if (controls) { a.sharpen = 1; a.brightness = .1f; a.grain = 1; a.hudShow = true; a.hudCount = 1;
        a.hud[0][2] = a.hud[0][3] = 1; a.marker = 2; a.compare = 1; }
    bool ok = f.compose.Run(f.ctx.Get(), a);
    const auto output = f.Read(target.Get(), 4);
    size_t bad = 0;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = (size_t(y) * w + x) * 4;
        if (!Inside(x, y, area)) { if (memcmp(output.data() + i, pixels.data() + i, 4)) ++bad; continue; }
        if (output[i + 3] != pixels[i + 3]) ++bad;
        if (controls || debug) continue;
        const float u = (x - area.x + .5f) / area.w;
        float v = (y - area.y + .5f) / area.h;
        if (movement == 1 || movement == -1) v -= movement * std::clamp(2 * v - .25f, 0.0f, 1.5f) / 8;
        if (movement == 2) v += .1f;
        const float expected[] = { 64 + 255 * (.02f + .15f * std::clamp(u, .5f / 16, 15.5f / 16)),
                                   64 + 255 * (.02f + .15f * std::clamp(v, .5f / 16, 15.5f / 16)), 64 + 255 * .01f };
        for (int c = 0; c < 3; ++c) if (std::abs(output[i + c] - expected[c]) > 1.1f) ++bad;
    }
    if (bad) printf("  %zu mismatched components/pixels\n", bad);
    Check(name, ok && !bad);
}

static void HdrCase(Fixture& f, bool direct) {
    const UINT w = 64, h = 48;
    const Compose11::Rect area{ 0, 8, 64, 32 };
    std::vector<uint16_t> pixels(w * h * 4);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = (y * w + x) * 4;
        for (int c = 0; c < 3; ++c) pixels[i + c] = Inside(x, y, area) ? 0x3400 : ((x + y) % 7 == 0 ? 0x4200 : 0);
        pixels[i + 3] = 0x3c00;
    }
    auto target = f.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, direct ? D3D11_BIND_UNORDERED_ACCESS : 0, pixels.data(), w * 8);
    Compose11::Args a; a.target = target.Get(); a.delta = f.deltaView.Get(); a.content = area; a.encoding = 1; a.debugView = 3; a.isGen = true;
    bool ok = f.compose.Run(f.ctx.Get(), a), changed = false;
    const auto output = f.Read(target.Get(), 8);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = (y * w + x) * 4;
        const bool same = memcmp(output.data() + i * 2, pixels.data() + i, 8) == 0;
        if (Inside(x, y, area)) changed |= !same; else ok &= same;
    }
    Check(direct ? "HDR borders preserved (direct UAV)" : "HDR borders preserved (copy back)", ok && changed);
}

int main() {
    try {
        const auto wide = Compose11::FitContent(3840, 1800, 3840, 2160);
        Check("logged 3840x1800 capture: 180-pixel borders", wide.x == 0 && wide.y == 180 && wide.w == 3840 && wide.h == 1800);
        const auto shortGame = Compose11::FitContent(3840, 1700, 3840, 2160);
        Check("3840x1700 capture: 230-pixel borders", shortGame.y == 230 && shortGame.h == 1700);
        const auto narrow = Compose11::FitContent(1600, 1200, 3840, 2160);
        Check("4:3 capture: pillarboxing", narrow.x == 480 && narrow.y == 0 && narrow.w == 2880 && narrow.h == 2160);
        const auto full = Compose11::FitContent(1920, 1080, 3840, 2160);
        Check("same-aspect upscaling: entire display", full.x == 0 && full.y == 0 && full.w == 3840 && full.h == 2160);
        Fixture f;
        SdrCase(f, "4K letterbox: borders and interior alignment", 3840, 2160, wide, true);
        const auto smallWide = Compose11::FitContent(3840, 1700, 384, 216);
        SdrCase(f, "letterbox copy-back path", 384, 216, smallWide, false);
        SdrCase(f, "pillarbox with spatial flow forward", 384, 216, Compose11::FitContent(1600, 1200, 384, 216), true, 1);
        SdrCase(f, "letterbox with spatial flow backward", 384, 216, smallWide, false, -1);
        SdrCase(f, "letterbox with model motion", 384, 216, smallWide, true, 2);
        for (UINT view = 1; view <= 5; ++view) SdrCase(f, "diagnostic view preserves borders", 384, 216, smallWide, false, 0, false, view);
        SdrCase(f, "picture controls and overlays preserve borders", 384, 216, smallWide, true, 0, true);
        SdrCase(f, "full-frame present fallback", 384, 216, {}, false);
        SdrCase(f, "resize and odd dimensions", 127, 95, Compose11::FitContent(200, 100, 127, 95), true);
        HdrCase(f, true); HdrCase(f, false);
    } catch (const std::exception& e) { printf("FAIL  %s\n", e.what()); ++failed; }
    printf("%d failure(s)\n", failed);
    return failed ? 1 : 0;
}
