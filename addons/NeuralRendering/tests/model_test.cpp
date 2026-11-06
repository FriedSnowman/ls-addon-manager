// SPDX-License-Identifier: GPL-3.0-only
#include "exception_diagnostics.h"
#include "gpu_test_utils.h"
#include "initialize_guides.h"
#include "model.h"
#include <DirectXPackedVector.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace neural;
int wmain(int argc, wchar_t **argv) {
    InstallExceptionDiagnostics();
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        puts("usage: neural_modeltest <user nvngx_dlssnr.dll> [intensity=1] [preset=0]");
        return 2;
    }
    try {
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto folder = std::filesystem::path(exe).parent_path();
        Gpu gpu;
        gpu.Initialize();
        printf("GPU %ls\n", gpu.adapterName.c_str());
        Recording rec;
        rec.Initialize(gpu.device.Get());
        constexpr unsigned w = 640, h = 360;
        auto colour = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT),
             output = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto depth = gpu.Texture(w, h, DXGI_FORMAT_R32_FLOAT), motion = gpu.Texture(w, h, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<uint16_t> pixels(w * h * 4);
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                size_t i = (y * w + x) * 4;
                const float v = .2f + .3f * std::sin(x * .035f) * std::cos(y * .041f);
                pixels[i] = DirectX::PackedVector::XMConvertFloatToHalf(std::clamp(v, .01f, 1.f));
                pixels[i + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.15f + .7f * y / h);
                pixels[i + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .8f * x / w);
                pixels[i + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
            }
        test::Upload(gpu, rec, colour, pixels.data(), w * 8);
        Kernel initialize;
        initialize.Initialize(gpu.device.Get(), g_initialize_guides, sizeof(g_initialize_guides));
        rec.Begin(gpu.fence.Get());
        depth.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        motion.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const unsigned c[] = {w, h, 0, 0};
        initialize.Dispatch(gpu.device.Get(), rec, c, sizeof(c), {}, {&depth, &motion}, (w + 7) / 8, (h + 7) / 8);
        depth.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        motion.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        test::Finish(gpu, rec);
        Model model;
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            ModelSettings settings;
            if (argc > 2)
                settings.intensity = float(_wtof(argv[2]));
            if (argc > 3)
                settings.preset = unsigned(_wtoi(argv[3]));
            if (cycle == 1)
                settings.intensity = 0;
            printf("Lifecycle cycle %u intensity %.2f preset %u\n", cycle, settings.intensity, settings.preset);
            rec.Begin(gpu.fence.Get());
            puts("Creating model feature");
            model.Initialize(gpu, rec, (folder / L"NeuralRendering_nvngx.dll").wstring(),
                             std::filesystem::absolute(argv[1]).wstring(), (folder / L"model-test-cache").wstring(), w,
                             h, settings);
            test::Finish(gpu, rec);
            printf("Feature created, float slot %d\n", model.FloatSlot());
            for (unsigned i = 0; i < 3; ++i) {
                rec.Begin(gpu.fence.Get());
                auto start = std::chrono::steady_clock::now();
                model.Evaluate(rec, colour, depth, motion, output, i == 0);
                test::Finish(gpu, rec);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const auto bytes = test::Read(gpu, rec, output, 8);
                const auto *result = reinterpret_cast<const uint16_t *>(bytes.data());
                double difference = 0, maximum = 0;
                for (size_t p = 0; p < size_t(w) * h; ++p)
                    for (unsigned ch = 0; ch < 3; ++ch) {
                        const size_t j = p * 4 + ch;
                        const float value = DirectX::PackedVector::XMConvertHalfToFloat(result[j]);
                        if (!std::isfinite(value))
                            throw std::runtime_error("Model returned non-finite colour");
                        difference += std::abs(value - DirectX::PackedVector::XMConvertHalfToFloat(pixels[j]));
                        maximum = std::max(maximum, double(value));
                    }
                printf("PASS eval%u reset=%u %.3fms mean edit=%.7f max=%.4f\n", i, i == 0, ms, difference / (w * h * 3),
                       maximum);
                if (maximum <= 0)
                    throw std::runtime_error("Model output is empty");
            }
            if (!model.Shutdown())
                throw std::runtime_error("Model retirement failed");
        }
        puts("MODEL TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "MODEL TEST ERROR: %s\n", e.what());
        return 1;
    }
}
