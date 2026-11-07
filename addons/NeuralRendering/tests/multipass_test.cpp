// SPDX-License-Identifier: GPL-3.0-only
#include "exception_diagnostics.h"
#include "model_runtime_fixture.h"
#include "pipeline.h"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace neural;
namespace {
void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template <class Fn> Fn Export(HMODULE module, const char *name) {
    auto fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
    Require(fn != nullptr, name);
    return fn;
}
}
int wmain() {
    InstallExceptionDiagnostics();
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::unique_ptr<Pipeline> pipeline;
    try {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "Get debug layer");
        debug->EnableDebugLayer();
        Gpu audit;
        audit.Initialize();
        ComPtr<ID3D12InfoQueue> messages;
        Check(audit.device.As(&messages), "Get GPU validation messages");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                               &device, nullptr, &context), "Create capture device");
        constexpr unsigned w = 320, h = 240;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> original, output, readback;
        std::vector<unsigned> pixels(w * h, 0xff604020);
        D3D11_SUBRESOURCE_DATA data{pixels.data(), w * 4, 0};
        Check(device->CreateTexture2D(&desc, &data, &original), "Create original");
        Check(device->CreateTexture2D(&desc, &data, &output), "Create output");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&desc, nullptr, &readback), "Create readback");
        auto unchanged = [&] {
            context->CopyResource(readback.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read output");
            bool same = true;
            for (unsigned y = 0; y < h; ++y)
                same &= memcmp(static_cast<char *>(mapped.pData) + y * mapped.RowPitch,
                               pixels.data() + y * w, w * 4) == 0;
            context->Unmap(readback.Get(), 0);
            Require(same, "Identity chain or failed pass changed the handoff");
        };
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        const auto folder = std::filesystem::path(exe).parent_path();
        PipelineSettings settings;
        settings.modelPath = (folder / L"neural_runtime_fixture.dll").wstring();
        settings.forwarderPath = (folder / L"NeuralRendering_nvngx.dll").wstring();
        settings.cachePath = (folder / L"multipass-test-cache").wstring();
        // Hold the fixture for inspection after the pipeline releases its module reference.
        auto fixture = LoadLibraryW(settings.modelPath.c_str());
        Require(fixture != nullptr, "Load fixture");
        auto configure = Export<int (*)(unsigned, unsigned)>(fixture, "neural_fixture_configure");
        auto live = Export<unsigned (*)()>(fixture, "neural_fixture_live");
        auto shutdowns = Export<unsigned (*)()>(fixture, "neural_fixture_shutdowns");
        auto evaluates = Export<unsigned (*)()>(fixture, "neural_fixture_evaluates");
        auto trace = Export<const FixtureEvaluation *(*)()>(fixture, "neural_fixture_trace");
        EamRealFrame frame;
        frame.context = context.Get(); frame.original = original.Get(); frame.output = output.Get();
        frame.width = w; frame.height = h; frame.epoch = 1;
        auto retire = [&] {
            Require(pipeline->Retire(), "Pipeline retirement failed");
            pipeline.reset();
            Require(live() == 0 && shutdowns() == 1, "Shared runtime was not retired exactly once");
        };
        for (unsigned count : {1u, 2u, 3u, PipelineSettings::MaxPasses, 1u}) {
            Require(configure(0, 0) == 1, "Fixture remained live");
            settings.passes = count;
            pipeline = std::make_unique<Pipeline>();
            pipeline->Initialize(device.Get(), w, h, Encoding::Sdr709, settings);
            Require(live() == count, "Wrong feature count");
            for (unsigned f = 0; f < 3; ++f) {
                if (f == 2) pipeline->Invalidate();
                frame.sequence = f + 1;
                pipeline->Process(frame, settings.composition);
                unchanged();
                Require(evaluates() == (f + 1) * count, "Each feature must see exactly one evaluation per frame");
                const auto *calls = trace() + f * count;
                for (unsigned pass = 0; pass < count; ++pass) {
                    Require(calls[pass].feature == trace()[pass].feature, "Pass history changed between frames");
                    Require(calls[pass].reset == (f != 1), "Reset was not propagated to every history");
                    Require(calls[pass].localTone == (pass ? 0 : settings.model.localTone), "Wrong pass tone profile");
                    Require(calls[pass].depth == calls[0].depth && calls[pass].motion == calls[0].motion,
                            "Passes did not use the same capture guides");
                    Require(calls[pass].output != calls[0].colour, "Chain overwrote its original composition anchor");
                    if (pass) {
                        Require(calls[pass].feature != calls[pass - 1].feature, "Passes shared temporal state");
                        Require(calls[pass].colour == calls[pass - 1].output, "Pass did not consume its predecessor");
                    }
                    if (pass >= 2)
                        Require(calls[pass].output == calls[pass - 2].output, "Passes did not alternate outputs");
                }
            }
            retire();
            printf("PASS %u-pass identity, independent histories, guides/reset, anchor and retirement\n", count);
        }
        for (bool creation : {true, false}) {
            for (unsigned failed : {2u, 3u, PipelineSettings::MaxPasses}) {
                Require(configure(creation ? failed : 0, creation ? 0 : failed) == 1, "Fixture remained live");
                settings.passes = PipelineSettings::MaxPasses;
                pipeline = std::make_unique<Pipeline>();
                std::string failure;
                try {
                    pipeline->Initialize(device.Get(), w, h, Encoding::Sdr709, settings);
                    frame.sequence = 1;
                    pipeline->Process(frame, settings.composition);
                } catch (const std::exception &e) { failure = e.what(); }
                Require(failure.find("NR pass " + std::to_string(failed)) != std::string::npos,
                        "Extra-pass failure did not stop and identify the failed pass");
                unchanged();
                retire();
                printf("PASS pass %u %s failure: no partial handoff, all features retired\n",
                       failed, creation ? "creation" : "evaluation");
            }
        }
        for (unsigned invalid : {0u, PipelineSettings::MaxPasses + 1}) {
            settings.passes = invalid;
            pipeline = std::make_unique<Pipeline>();
            bool rejected = false;
            try { pipeline->Initialize(device.Get(), w, h, Encoding::Sdr709, settings); }
            catch (const std::exception &) { rejected = true; }
            Require(rejected && pipeline->Retire(), "Invalid pass count was not safely rejected");
            pipeline.reset();
        }
        FreeLibrary(fixture);
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            messages->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                puts(message->pDescription);
                throw std::runtime_error("D3D12 validation error");
            }
        }
        puts("MULTIPASS LIFECYCLE TEST PASSED; zero D3D12 errors");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "MULTIPASS TEST ERROR: %s\n", e.what());
        if (pipeline && !pipeline->Retire()) pipeline.release();
        return 1;
    }
}
