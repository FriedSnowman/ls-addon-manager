// Offscreen integration test: the real Direct2D DrawText hook, DXGI render target,
// saved settings and pixel readback. No LS installation, game or visible window.
#include "src/config/config_manager.h"
#include "src/features/features.h"
#include "src/features/scaling_overrides.h"
#include "src/log/logger.h"
#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;
using namespace eam;
namespace scaling = eam::features::scaling;
namespace fs = std::filesystem;
int failures = 0;
void Check(const char* name, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }

struct Canvas {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> format;
    unsigned width, height;

    bool Init(unsigned w, unsigned h, float dpi, bool hardware) {
        width = w; height = h;
        if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        ComPtr<IDXGISurface> surface;
        ComPtr<ID2D1Factory> factory;
        ComPtr<IDWriteFactory> write;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(texture.As(&surface)) || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &target)) ||
            FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.GetAddressOf()))) ||
            FAILED(write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-US", &format))) return false;
        return true;
    }
    std::vector<unsigned> Draw(const wchar_t* text) {
        target->BeginDraw();
        target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        const auto rect = D2D1::RectF(8, 8, 300, 80);
        target->DrawText(text, static_cast<UINT32>(wcslen(text)), format.Get(), rect, brush.Get());
        Check("Direct2D draw completes", SUCCEEDED(target->EndDraw()));
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return {};
        std::vector<unsigned> pixels(size_t(width) * height);
        for (unsigned y = 0; y < height; ++y)
            memcpy(pixels.data() + size_t(y) * width, static_cast<const char*>(map.pData) + size_t(y) * map.RowPitch, width * 4);
        context->Unmap(staging.Get(), 0);
        return pixels;
    }
    void CheckCorner(const std::vector<unsigned>& pixels, int corner) {
        unsigned lit = 0, misplaced = 0;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            if (pixels.empty() || (pixels[size_t(y) * width + x] & 0xffffff) == 0) continue;
            ++lit;
            if ((x >= width / 2) != bool(corner & 1) || (y >= height / 2) != bool(corner & 2)) ++misplaced;
        }
        Check("counter pixels appear only in the selected corner", lit > 0 && misplaced == 0);
    }
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool foreign = argc > 1 && std::string(argv[1]) == "foreign";
    const bool hardware = argc > 1 && std::string(argv[1]) == "hardware";
    const fs::path dir = fs::temp_directory_path() / ("eam_fps_counter_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    const auto path = (dir / "config.json").wstring();
    ConfigManager& cfg = ConfigManager::Instance();
    cfg.Load(path);
    cfg.SetAddonEnabled(scaling::kId, false);
    Canvas canvas;
    if (!canvas.Init(640, 360, 96, hardware)) { puts("FAIL creating Direct2D test canvas"); return 1; }
    const auto original = canvas.Draw(L"60 / 120");
    const auto otherText = canvas.Draw(L"Status 60");
    Check("hook installs", scaling::InstallHooks(foreign ? GetModuleHandleW(L"d2d1.dll") : GetModuleHandleW(nullptr)));
    Check("disabled override preserves the original pixels", canvas.Draw(L"60 / 120") == original);
    features::SetOn(2, true);
    Check("enabling takes effect without a restart", !features::NeedsRestart(2));
    if (foreign) {
        scaling::SetFpsCorner(scaling::FpsCorner::BottomRight);
        Check("text from another module is untouched", canvas.Draw(L"60 / 120") == original);
    } else {
        unsigned expectedInk = 0;
        for (int corner = 0; corner < 4; ++corner) {
            scaling::SetFpsCorner(static_cast<scaling::FpsCorner>(corner));
            auto pixels = canvas.Draw(L"60 / 120");
            canvas.CheckCorner(pixels, corner);
            unsigned ink = 0;
            for (unsigned p : pixels) if (p & 0xffffff) ++ink;
            if (!corner) expectedInk = ink;
            Check("moving preserves all counter glyphs", ink == expectedInk);
            canvas.CheckCorner(canvas.Draw(L"144"), corner);
            cfg.Load(path);
            scaling::Stop(); scaling::Start();
            Check("corner survives saving and reloading", static_cast<int>(scaling::GetFpsCorner()) == corner);
        }
        Check("non-counter text is untouched", canvas.Draw(L"Status 60") == otherText);
        const auto transform = D2D1::Matrix3x2F::Rotation(90);
        canvas.target->SetTransform(transform);
        canvas.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        canvas.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        canvas.format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        canvas.CheckCorner(canvas.Draw(L"60 / 120"), 3);
        D2D1_MATRIX_3X2_F restored{};
        canvas.target->GetTransform(&restored);
        Check("render state and text formatting are restored", memcmp(&restored, &transform, sizeof restored) == 0 &&
            canvas.format->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_CENTER &&
            canvas.format->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER &&
            canvas.format->GetWordWrapping() == DWRITE_WORD_WRAPPING_WRAP);
        canvas.target->SetTransform(D2D1::Matrix3x2F::Identity());
        canvas.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        canvas.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Canvas resized;
        Check("new output at 150 percent DPI initializes", resized.Init(960, 540, 144, hardware));
        if (resized.target) for (int corner = 0; corner < 4; ++corner) {
            scaling::SetFpsCorner(static_cast<scaling::FpsCorner>(corner));
            resized.CheckCorner(resized.Draw(L"60 / 120"), corner);
        }
        for (const char* bad : { "-1", "4", "garbage", "2junk", "99999999999999999999" }) {
            cfg.Set(scaling::kId, "fps_counter_corner", bad);
            scaling::Start();
            Check("invalid saved corner falls back to top left", scaling::GetFpsCorner() == scaling::FpsCorner::TopLeft);
        }
    }
    features::SetOn(2, false);
    Check("switching off restores original placement and pixels", canvas.Draw(L"60 / 120") == original);
    Check("disabled feature clears its status", features::Status(2).empty());
    if (failures) for (const auto& entry : Logger::Instance().GetEntries(LogLevel::Warn))
        printf("%s: %s\n", entry.source.c_str(), entry.message.c_str());
    std::error_code ec;
    fs::remove_all(dir, ec);
    printf("\nFPS COUNTER TEST %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
