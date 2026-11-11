#include "addon/scaler11.h"
#include <eam/shader_identity.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

namespace nr {
namespace {
// Verified LS 3.2.2 SDR/HDR NIS bytecode identities (resources 245/246).
// The manager attaches this metadata
// when the shader is created; no shader bytes or capture processing live here.
constexpr eam::ShaderSha256 kNis = {
    0xfc,0x8f,0x03,0x00,0xfe,0xc6,0xf8,0x02,0x50,0xb8,0xec,0x60,0x52,0x65,0xba,0x91,
    0xc8,0x0f,0x1d,0x89,0x7f,0xe7,0x99,0x00,0x4f,0x2c,0xb9,0x5e,0x14,0xca,0x3d,0x02};
constexpr eam::ShaderSha256 kNisHdr = {
    0x70,0x6e,0x76,0xdc,0xb5,0xb7,0x25,0xb0,0xdc,0x3c,0x03,0x68,0xfa,0xc0,0x13,0x5e,
    0x2f,0xff,0xab,0x60,0x35,0x72,0x77,0xdd,0xfe,0x05,0xc3,0x7c,0x0a,0xde,0x65,0x7a};
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
bool Texture2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& desc) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&desc);
    return true;
}

// ---- NIS's viewports
//
// NVIDIA's NIS scales between two viewports, which its constant buffer (NISConfig, b0) holds, and dispatches one group per 32x24 pixels of
// the output viewport. For a window of the screen's shape both are the whole textures; for another shape Lossless Scaling scales the window
// into part of the screen. The constants are read once per pass shape without waiting (copied now, mapped on a later pass), and only a
// layout that agrees with everything else (the dispatch, the textures, NIS's own scale factor) is used.
struct NisConfigView {   // NISConfig (NVIDIA Image Scaling SDK 1.0), the part read here: 18 floats, then the viewports
    float f[18]; uint32_t inX, inY, inW, inH, outX, outY, outW, outH;
};
static_assert(sizeof(NisConfigView) == 104, "NISConfig's viewports start at byte 72");

struct ViewportReader {
    struct Key { uint32_t inW, inH, outW, outH, x, y; bool operator==(const Key& k) const { return !memcmp(this, &k, sizeof k); } };
    ID3D11Device* dev = nullptr;
    ID3D11Buffer* staging = nullptr;
    Key key{}; int state = 0;         // 0 nothing, 1 a copy in flight, 2 known good, 3 known unusable
    NisConfigView cfg{};
    uint32_t tries = 0;
    void Reset() { if (staging) staging->Release(); staging = nullptr; dev = nullptr; state = 0; tries = 0; }
};
ViewportReader g_viewports;

// True with the viewports filled in, once known; false meanwhile (NIS runs as usual) and for a layout that does not fit.
bool ResolveViewports(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& in, const D3D11_TEXTURE2D_DESC& o, uint32_t x, uint32_t y, NisPass& pass,
                      const std::function<void(const char*)>& log) {
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (dev) dev->Release();   // only compared
    const ViewportReader::Key key{ in.Width, in.Height, o.Width, o.Height, x, y };
    ViewportReader& r = g_viewports;
    if (r.dev != dev || !(r.key == key)) { r.Reset(); r.dev = dev; r.key = key; }
    auto say = [&](const char* fmt, auto... args) { if (log) { char text[400]; snprintf(text, sizeof text, fmt, args...); log(text); } };
    if (r.state == 0) {
        if (++r.tries > 3) { r.state = 3; return false; }
        ID3D11Buffer* cb = nullptr; ctx->CSGetConstantBuffers(0, 1, &cb);
        if (!cb) { say("NIS pass on part of its output (%ux%u -> %ux%u, %ux%u groups): it has no constants bound; NIS stays", in.Width, in.Height, o.Width, o.Height, x, y); r.state = 3; return false; }
        D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
        if (d.ByteWidth < sizeof(NisConfigView)) { say("NIS pass on part of its output: its constants are %u bytes, too few for NIS's; NIS stays", d.ByteWidth); cb->Release(); r.state = 3; return false; }
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!r.staging && FAILED(dev->CreateBuffer(&sd, nullptr, &r.staging))) { cb->Release(); r.state = 3; return false; }
        ctx->CopyResource(r.staging, cb);
        cb->Release();
        r.state = 1;
        return false;
    }
    if (r.state == 1) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return false;   // not back yet
        memcpy(&r.cfg, m.pData, sizeof r.cfg);
        ctx->Unmap(r.staging, 0);
        const NisConfigView& c = r.cfg;
        const float scaleX = c.outW ? static_cast<float>(c.inW) / c.outW : 0.0f, scaleY = c.outH ? static_cast<float>(c.inH) / c.outH : 0.0f;
        const bool fits = c.inW && c.inH && c.outW && c.outH && c.inX + c.inW <= in.Width && c.inY + c.inH <= in.Height &&
                          c.outX + c.outW <= o.Width && c.outY + c.outH <= o.Height && x == (c.outW + 31) / 32 && y == (c.outH + 23) / 24 &&
                          c.outW >= c.inW && c.outH >= c.inH &&
                          std::abs(c.f[12] - scaleX) < 0.02f * scaleX + 1e-4f && std::abs(c.f[13] - scaleY) < 0.02f * scaleY + 1e-4f;   // kScaleX, kScaleY
        say("NIS pass on part of its output: frame %ux%u, output %ux%u, %ux%u groups; its constants: input viewport %u,%u %ux%u, output viewport %u,%u %ux%u, "
            "scale %.4f x %.4f -> %s", in.Width, in.Height, o.Width, o.Height, x, y, c.inX, c.inY, c.inW, c.inH, c.outX, c.outY, c.outW, c.outH, c.f[12], c.f[13],
            fits ? "the upscaler takes that part" : "they do not fit together; NIS stays");
        r.state = fits ? 2 : 3;
        if (!fits) return false;
    }
    if (r.state != 2) return false;
    const NisConfigView& c = r.cfg;
    pass.inX = c.inX; pass.inY = c.inY; pass.inW = c.inW; pass.inH = c.inH;
    pass.outX = c.outX; pass.outY = c.outY; pass.outW = c.outW; pass.outH = c.outH;
    return true;
}

} // namespace

// ---- recognising the NIS pass

namespace { bool g_lastRefused = false; }   // the last FindNisPass was a NIS pass whose layout could not be followed (render thread only)

bool FindNisPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, NisPass& pass, const std::function<void(const char*)>& log) {
    pass = {};
    g_lastRefused = false;
    if (!ctx || z != 1) return false;
    // LS leaves NIS's coefficient textures bound during unrelated LSFG work.
    // Reject those passes before they can reset a pending viewport readback.
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    ctx->CSGetShader(&shader, nullptr, nullptr);
    eam::ShaderSha256 identity{};
    if (!eam::ReadShaderSha256(shader.Get(), identity) || (identity != kNis && identity != kNisHdr)) return false;
    ID3D11ShaderResourceView* srvs[3] = {}; ID3D11UnorderedAccessView* uav = nullptr;
    ctx->CSGetShaderResources(0, 3, srvs); ctx->CSGetUnorderedAccessViews(0, 1, &uav);
    ID3D11Resource* res[3] = {}; ID3D11Resource* out = nullptr;
    for (int i = 0; i < 3; ++i) if (srvs[i]) srvs[i]->GetResource(&res[i]);
    if (uav) uav->GetResource(&out);
    D3D11_TEXTURE2D_DESC in{}, c1{}, c2{}, o{};
    auto coefficients = [](const D3D11_TEXTURE2D_DESC& d) { return d.Width == 2 && d.Height == 64 && d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT; };
    const bool nis = Texture2D(res[0], in) && Texture2D(res[1], c1) && Texture2D(res[2], c2) && Texture2D(out, o) && coefficients(c1) && coefficients(c2);
    const bool whole = nis && o.Width >= in.Width && o.Height >= in.Height &&   // 1:1 too (DLSS then runs as DLAA)
                       x == (o.Width + 31) / 32 && y == (o.Height + 23) / 24;
    for (auto*& v : srvs) SafeRelease(v);
    SafeRelease(uav); SafeRelease(res[1]); SafeRelease(res[2]);
    // NIS's bindings with a dispatch over less than the output: a window of another shape, scaled into part of the screen
    const bool part = nis && !whole && x <= (o.Width + 31) / 32 && y <= (o.Height + 23) / 24 && ResolveViewports(ctx, in, o, x, y, pass, log);
    g_lastRefused = nis && !whole && !part && g_viewports.state == 3;   // this very pass (not a verdict kept from another window shape)
    if (!whole && !part) { SafeRelease(res[0]); SafeRelease(out); pass = {}; return false; }
    pass.in = res[0]; pass.out = out; pass.inFmt = in.Format; pass.outFmt = o.Format;
    if (whole) { pass.inW = in.Width; pass.inH = in.Height; pass.outW = o.Width; pass.outH = o.Height; }
    return true;
}

void ReleaseNisPass(NisPass& pass) { SafeRelease(pass.in); SafeRelease(pass.out); pass = {}; }

bool NisLayoutRefused() { return g_lastRefused; }

void ResetNisPass() { g_viewports.Reset(); g_lastRefused = false; }


} // namespace nr
