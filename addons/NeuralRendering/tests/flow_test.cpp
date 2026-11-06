// SPDX-License-Identifier: GPL-3.0-only
#include "gpu_test_utils.h"
#include "optical_flow.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace neural;
namespace {
std::vector<uint8_t> Pattern(uint32_t w, uint32_t h, int dx, int dy, uint32_t seed) {
    std::vector<uint8_t> result(size_t(w) * h * 4);
    auto hash = [](uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352d;
        x ^= x >> 15;
        x *= 0x846ca68b;
        x ^= x >> 16;
        return x;
    };
    auto noise = [&](int x, int y, int c) {
        return float(hash(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(c) * 83492791u ^ seed) & 255);
    };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float px = (int(x) - dx) / 5.f, py = (int(y) - dy) / 5.f;
            int ix = int(floor(px)), iy = int(floor(py));
            const float fx = px - ix, fy = py - iy;
            for (int c = 0; c < 3; ++c) {
                float a = noise(ix, iy, c) * (1 - fx) + noise(ix + 1, iy, c) * fx,
                      b = noise(ix, iy + 1, c) * (1 - fx) + noise(ix + 1, iy + 1, c) * fx;
                result[(size_t(y) * w + x) * 4 + c] = uint8_t(std::clamp(a * (1 - fy) + b * fy, 0.f, 255.f));
            }
            result[(size_t(y) * w + x) * 4 + 3] = 255;
        }
    return result;
}
float Median(std::vector<float> v) {
    auto i = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), i, v.end());
    return *i;
}
float P95(std::vector<float> v) {
    auto i = v.begin() + v.size() * 95 / 100;
    std::nth_element(v.begin(), i, v.end());
    return *i;
}
bool Run(Gpu &gpu, uint32_t w, uint32_t h, uint32_t tw, uint32_t th, uint32_t grid) {
    printf("Initialize flow %ux%u grid %u\n", w, h, grid);
    OpticalFlow flow;
    flow.Initialize(gpu, w, h, grid);
    Recording prepare, finish, transfer;
    prepare.Initialize(gpu.device.Get());
    finish.Initialize(gpu.device.Get());
    transfer.Initialize(gpu.device.Get());
    auto original = gpu.Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    auto motion = gpu.Texture(tw, th, DXGI_FORMAT_R16G16_FLOAT);
    struct Step {
        int x, y;
        uint32_t seed;
        bool reset;
    };
    const Step steps[] = {{0, 0, 7, true},  {7, -4, 7, false}, {12, -1, 7, false}, {40, -9, 7, false},
                          {0, 0, 13, true}, {6, 3, 13, false}, {6, 3, 13, false}};
    bool passed = true;
    for (uint32_t f = 0; f < std::size(steps); ++f) {
        const auto s = steps[f];
        const auto pixels = Pattern(w, h, s.x, s.y, s.seed);
        test::Upload(gpu, transfer, original, pixels.data(), w * 4);
        const auto start = std::chrono::steady_clock::now();
        prepare.Begin(gpu.fence.Get());
        flow.Prepare(gpu, prepare, original, f % 3);
        const auto ready = gpu.Submit(prepare);
        flow.Execute(gpu, f % 3, (f + 2) % 3, s.reset, ready);
        finish.Begin(gpu.fence.Get());
        flow.Convert(gpu, finish, motion, s.reset);
        test::Finish(gpu, finish);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto bytes = test::Read(gpu, transfer, motion, 4);
        std::vector<float> xs, ys, errors;
        const float ex = s.reset ? 0.f : -(s.x - steps[f - 1].x) * float(tw) / w,
                    ey = s.reset ? 0.f : -(s.y - steps[f - 1].y) * float(th) / h;
        const auto *half = reinterpret_cast<const uint16_t *>(bytes.data());
        for (uint32_t y = th / 6; y < th * 5 / 6; ++y)
            for (uint32_t x = tw / 6; x < tw * 5 / 6; ++x) {
                const size_t i = (size_t(y) * tw + x) * 2;
                const float vx = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
                const float vy = DirectX::PackedVector::XMConvertHalfToFloat(half[i + 1]);
                xs.push_back(vx);
                ys.push_back(vy);
                errors.push_back(std::hypot(vx - ex, vy - ey));
            }
        const float mx = Median(xs), my = Median(ys), err = Median(errors), p95 = P95(errors);
        // Ground-truth displacement, not implementation-mirroring expected bytes.
        const bool ok = std::isfinite(err) && std::isfinite(p95) && err < (s.reset ? 0.0001f : 0.75f) &&
                        p95 < (s.reset ? 0.0001f : 1.5f);
        passed &= ok;
        printf("%s %ux%u grid%u -> %ux%u step%u%s: expected %.3f %.3f median %.3f %.3f EPE %.4f p95 %.4f %.3fms\n",
               ok ? "PASS" : "FAIL", w, h, grid, tw, th, f, s.reset ? " reset" : "", ex, ey, mx, my, err, p95, ms);
        if (f == 1) {
            const unsigned bpp = flow.Cost().format == DXGI_FORMAT_R32_UINT ? 4 : 1;
            const auto cost = test::Read(gpu, transfer, flow.Cost(), bpp);
            uint64_t sum = 0;
            for (size_t i = 0; i < cost.size() / bpp; ++i) {
                uint32_t value = 0;
                memcpy(&value, cost.data() + i * bpp, bpp);
                sum += value;
            }
            printf("Confidence cost mean %.3f (diagnostic only; no vector attenuation)\n",
                   double(sum) / (cost.size() / bpp));
        }
    }
    if (!gpu.Wait(gpu.LastSubmitted()) || !flow.Shutdown())
        throw std::runtime_error("Flow retirement failed");
    return passed;
}
} // namespace
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
        HMODULE module = nullptr;
        wchar_t path[32768]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(e->ExceptionRecord->ExceptionAddress), &module);
        GetModuleFileNameW(module, path, 32768);
        fprintf(stderr, "Exception %08lx at %ls + %llx\n", e->ExceptionRecord->ExceptionCode, path,
                (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)module));
        void *frames[32]{};
        const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &module);
            GetModuleFileNameW(module, path, 32768);
            fprintf(stderr, "  %ls + %llx\n", path, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)module));
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    try {
        Gpu gpu;
        gpu.Initialize();
        printf("GPU: %ls\n", gpu.adapterName.c_str());
        bool ok = true;
        ok = Run(gpu, 640, 360, 640, 360, 4) && ok;
        ok = Run(gpu, 640, 360, 640, 360, 2) && ok;
        ok = Run(gpu, 640, 360, 640, 360, 1) && ok;
        ok = Run(gpu, 641, 359, 321, 179, 2) && ok;
        ok = Run(gpu, 1024, 576, 768, 256, 1) && ok;
        ok = Run(gpu, 1920, 1080, 1280, 720, 4) && ok;
        bool rejected = false;
        try {
            OpticalFlow invalid;
            invalid.Initialize(gpu, 640, 360, 3);
        } catch (const std::exception &) {
            rejected = true;
        }
        puts(rejected ? "PASS unsupported grid rejected" : "FAIL unsupported grid accepted");
        ok &= rejected;
        puts(ok ? "FLOW TEST PASSED" : "FLOW TEST FAILED");
        return ok ? 0 : 1;
    } catch (const std::exception &error) {
        fprintf(stderr, "FLOW TEST ERROR: %s\n", error.what());
        return 1;
    }
}
