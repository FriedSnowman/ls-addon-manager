// SPDX-License-Identifier: GPL-3.0-only
#include "composition.h"
#include "gpu_test_utils.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d3dcompiler.h>
#include <filesystem>

using namespace neural;
namespace {
// Independent upstream invocation: explicit original shader constants and bindings,
// without using CompositionConstants or the production adapter's mapping code.
void Reference(Gpu &gpu, Recording &rec, Kernel &kernel, unsigned mode, unsigned w, unsigned h, Encoding encoding,
               const CompositionSettings &s, const std::array<Resource *, 5> &inputs,
               const std::array<Resource *, 2> &outputs) {
    std::array<uint32_t, 24> c{};
    auto f = [&](unsigned i, float v) { memcpy(&c[i], &v, 4); };
    c[0] = mode;
    f(1, encoding == Encoding::ScRgb ? s.whiteNits / 80.f : 1.f);
    c[2] = w;
    c[3] = h;
    f(4, s.detail);
    f(5, s.colour);
    f(7, s.maxRatio);
    c[8] = encoding == Encoding::Sdr709;
    f(9, 1);
    f(10, 1);
    c[11] = w;
    c[12] = h;
    f(14, .5f);
    f(15, 1);
    c[17] = s.reconstruction;
    f(18, 1);
    c[19] = s.reversible;
    c[20] = s.apply;
    f(22, 1);
    for (auto *r : inputs)
        if (r)
            r->Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (auto *r : outputs)
        if (r)
            r->Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    kernel.Dispatch(gpu.device.Get(), rec, c.data(), sizeof(c), inputs, outputs, (w + 7) / 8, (h + 7) / 8);
    for (auto *r : outputs)
        if (r)
            r->Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void Compare(Gpu &gpu, Recording &rec, Resource &actual, Resource &expected, const char *stage) {
    auto a = test::Read(gpu, rec, actual, 8), b = test::Read(gpu, rec, expected, 8);
    if (a != b)
        throw std::runtime_error(std::string("Upstream parity mismatch at ") + stage);
    const auto *half = reinterpret_cast<const uint16_t *>(a.data());
    for (size_t i = 0; i < a.size() / 2; ++i)
        if (!std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(half[i])))
            throw std::runtime_error(std::string("Non-finite output at ") + stage);
}
} // namespace
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto path = std::filesystem::path(NEURAL_SOURCE_DIR) / "third_party/optiscaler/dlssnr.hlsl";
        ComPtr<ID3DBlob> shader, error;
        const HRESULT hr =
            D3DCompileFromFile(path.c_str(), nullptr, nullptr, "CSMain", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &error);
        if (FAILED(hr))
            throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                           : "Cannot compile upstream reference");
        Gpu gpu;
        gpu.Initialize();
        Kernel reference;
        reference.Initialize(gpu.device.Get(), shader->GetBufferPointer(), shader->GetBufferSize());
        Composition adapter;
        adapter.Initialize(gpu.device.Get());
        Recording rec;
        rec.Initialize(gpu.device.Get());
        constexpr unsigned w = 63, h = 37;
        auto original = gpu.Texture(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT);
        unsigned cases = 0;
        for (Encoding encoding : {Encoding::Sdr709, Encoding::ScRgb})
            for (bool reduced : {false, true}) {
                const unsigned ww = reduced ? 31 : w, wh = reduced ? 19 : h;
                auto full = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT),
                     keep = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
                auto expectedFull = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT),
                     expectedKeep = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
                auto work = gpu.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT),
                     expectedWork = gpu.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
                auto model = gpu.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
                auto output = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT),
                     expected = gpu.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
                std::vector<float> input(w * h * 4);
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x) {
                        const size_t i = (y * w + x) * 4;
                        float scale = encoding == Encoding::ScRgb ? 16.f : 1.f;
                        input[i] = float(x) / w * scale;
                        input[i + 1] = float(y) / h * scale;
                        input[i + 2] = ((x + y) % 9) / 8.f * scale;
                        input[i + 3] = .75f;
                        if (x < 8) {
                            input[i] = input[i + 1] = input[i + 2] = x * 0.00001f;
                        }
                        if (y < 4) {
                            input[i] = scale;
                            input[i + 1] = input[i + 2] = 0;
                        }
                    }
                test::Upload(gpu, rec, original, input.data(), w * 16);
                for (unsigned reversible = 0; reversible < 5; ++reversible)
                    for (unsigned transfer = 0; transfer < 2; ++transfer)
                        for (unsigned control = 0; control < 4; ++control) {
                            CompositionSettings s;
                            s.reversible = reversible;
                            s.reconstruction = transfer;
                            if (control == 1) {
                                s.detail = 0;
                                s.colour = 0;
                            }
                            if (control == 2) {
                                s.detail = 1.5f;
                                s.colour = 2.f;
                            }
                            if (control == 3)
                                s.apply = false;
                            rec.Begin(gpu.fence.Get());
                            adapter.Encode(gpu, rec, original, full, keep, encoding, s);
                            Reference(gpu, rec, reference, 0, w, h, encoding, s, {&original},
                                      {&expectedFull, &expectedKeep});
                            adapter.Downsample(gpu, rec, full, work);
                            Reference(gpu, rec, reference, 2, ww, wh, encoding, s, {&expectedFull}, {&expectedWork});
                            test::Finish(gpu, rec);
                            Compare(gpu, rec, full, expectedFull, "encode");
                            Compare(gpu, rec, keep, expectedKeep, "original retention");
                            Compare(gpu, rec, work, expectedWork, "downsample");
                            auto bytes = test::Read(gpu, rec, work, 8);
                            auto *halves = reinterpret_cast<uint16_t *>(bytes.data());
                            for (unsigned y = 0; y < wh; ++y)
                                for (unsigned x = 0; x < ww; ++x)
                                    for (unsigned channel = 0; channel < 3; ++channel) {
                                        const unsigned i = (y * ww + x) * 4 + channel;
                                        const float v = DirectX::PackedVector::XMConvertHalfToFloat(halves[i]);
                                        const float edit =
                                            std::sin(float(x + y)) * 0.025f + (channel == 0 ? .03f : -.01f);
                                        halves[i] =
                                            DirectX::PackedVector::XMConvertFloatToHalf(std::clamp(v + edit, 0.f, 1.f));
                                    }
                            test::Upload(gpu, rec, model, bytes.data(), ww * 8);
                            rec.Begin(gpu.fence.Get());
                            adapter.Resolve(gpu, rec, work, model, keep, output, encoding, s);
                            Reference(gpu, rec, reference, 1, w, h, encoding, s, {&expectedWork, &model, &expectedKeep},
                                      {&expected});
                            test::Finish(gpu, rec);
                            Compare(gpu, rec, output, expected, "resolve");
                            if (!s.apply)
                                Compare(gpu, rec, output, keep, "apply-off identity");
                            ++cases;
                        }
                printf(
                    "PASS %s %s: all proxy/reconstruction/control sweeps bit-exact to separately compiled upstream\n",
                    encoding == Encoding::ScRgb ? "scRGB" : "SDR", reduced ? "reduced" : "native");
            }
        printf("COMPOSITION TEST PASSED: %u cases, zero byte differences\n", cases);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "COMPOSITION TEST ERROR: %s\n", error.what());
        return 1;
    }
}
