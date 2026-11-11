#include "scaling_overrides.h"
#include "../config/config_manager.h"
#include "../core/hook_util.h"
#include "../log/logger.h"
#include "imgui.h"
#include <MinHook.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <intrin.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>

#pragma comment(lib, "d2d1.lib")

namespace eam::features::scaling {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<bool> g_active{ false }, g_ready{ false };
std::atomic<int> g_corner{ 0 };
uintptr_t g_moduleBegin = 0, g_moduleEnd = 0;
const char* const kCorners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
using DrawTextFn = void(STDMETHODCALLTYPE*)(ID2D1RenderTarget*, const WCHAR*, UINT32,
    IDWriteTextFormat*, const D2D1_RECT_F*, ID2D1Brush*, D2D1_DRAW_TEXT_OPTIONS, DWRITE_MEASURING_MODE);
DrawTextFn g_drawText = nullptr;

// LS draws either the output FPS or "capture / output". Do not move unrelated text
// if another LS version starts drawing additional overlays with Direct2D.
bool IsCounter(const WCHAR* text, UINT32 length) {
    if (!text || !length || length > 32) return false;
    UINT32 i = 0;
    auto digits = [&] { const UINT32 start = i; while (i < length && text[i] >= L'0' && text[i] <= L'9') ++i; return i > start; };
    if (!digits()) return false;
    if (i == length) return true;
    if (i + 3 >= length || text[i] != L' ' || text[i + 1] != L'/' || text[i + 2] != L' ') return false;
    i += 3;
    return digits() && i == length;
}

void STDMETHODCALLTYPE DrawText(ID2D1RenderTarget* target, const WCHAR* text, UINT32 length,
    IDWriteTextFormat* format, const D2D1_RECT_F* rect, ID2D1Brush* brush,
    D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!g_active.load(std::memory_order_relaxed) || caller < g_moduleBegin || caller >= g_moduleEnd ||
        !format || !rect || !IsCounter(text, length)) {
        g_drawText(target, text, length, format, rect, brush, options, measuring);
        return;
    }

    const int corner = g_corner.load(std::memory_order_relaxed);
    const D2D1_SIZE_F size = target->GetSize(); // DIPs: also works when output DPI or size changes
    const float insetX = (std::min)(8.0f, size.width * 0.5f);
    const float insetY = (std::min)(8.0f, size.height * 0.5f);
    const D2D1_RECT_F bounds{ insetX, insetY, size.width - insetX, size.height - insetY };
    const auto alignment = format->GetTextAlignment();
    const auto paragraph = format->GetParagraphAlignment();
    const auto wrapping = format->GetWordWrapping();
    D2D1_MATRIX_3X2_F transform;
    target->GetTransform(&transform);
    // The counter belongs to the output's corner, including on rotated displays.
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    format->SetTextAlignment((corner & 1) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment((corner & 2) ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_drawText(target, text, length, format, &bounds, brush, options, measuring);
    format->SetWordWrapping(wrapping);
    format->SetParagraphAlignment(paragraph);
    format->SetTextAlignment(alignment);
    target->SetTransform(transform);
}
} // namespace

bool InstallHooks(HMODULE lossless) {
    if (g_ready) return true;
    if (!lossless) { LOG_WARN("ScalingOverrides", "Lossless Scaling module is unavailable"); return false; }
    // Match LS's DXGI-surface render target. WARP needs no visible window or hardware
    // context, and exposes the same Direct2D DrawText implementation as LS's target.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RenderTarget> target;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)) ||
        FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) || FAILED(texture.As(&surface)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &properties, &target))) {
        LOG_WARN("ScalingOverrides", "Could not initialize the FPS counter position override");
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(lossless);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(lossless) + dos->e_lfanew);
    g_moduleBegin = reinterpret_cast<uintptr_t>(lossless);
    g_moduleEnd = g_moduleBegin + nt->OptionalHeader.SizeOfImage;
    void* const draw = (*reinterpret_cast<void***>(target.Get()))[27]; // ID2D1RenderTarget::DrawText
    if (!hooks::Begin()) { LOG_WARN("ScalingOverrides", "MinHook could not initialize"); return false; }
    if (MH_CreateHook(draw, reinterpret_cast<void*>(&DrawText), reinterpret_cast<void**>(&g_drawText)) != MH_OK) {
        hooks::End();
        LOG_WARN("ScalingOverrides", "Could not create the FPS counter drawing hook");
        return false;
    }
    if (MH_EnableHook(draw) != MH_OK) {
        MH_RemoveHook(draw); hooks::End();
        LOG_WARN("ScalingOverrides", "Could not enable the FPS counter drawing hook");
        return false;
    }
    // Like the compute hooks, this trampoline lives for the process lifetime.
    // Stop only disables the override; an in-flight draw can still call its original.
    g_ready = true;
    LOG_INFO("ScalingOverrides", "FPS counter position override ready");
    return true;
}

void Start() {
    const std::string saved = ConfigManager::Instance().Get(kId, "fps_counter_corner", "0");
    g_corner = saved.size() == 1 && saved[0] >= '0' && saved[0] <= '3' ? saved[0] - '0' : 0;
    g_active = true;
}
void Stop() { g_active = false; }
FpsCorner GetFpsCorner() { return static_cast<FpsCorner>(g_corner.load()); }
void SetFpsCorner(FpsCorner corner) {
    const int value = static_cast<int>(corner);
    g_corner = value >= 0 && value < 4 ? value : 0;
    ConfigManager::Instance().Set(kId, "fps_counter_corner", std::to_string(g_corner.load()));
    ConfigManager::Instance().Save();
}
std::string Status() {
    return g_ready ? std::string("FPS counter: ") + kCorners[g_corner.load()] :
        "FPS counter override unavailable";
}
void RenderOptions() {
    int corner = g_corner.load();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::Combo("FPS counter location", &corner, kCorners, 4)) SetFpsCorner(static_cast<FpsCorner>(corner));
    ImGui::TextWrapped("Enable Draw FPS in Lossless Scaling to show its counter. Position changes apply immediately.");
}

} // namespace eam::features::scaling
