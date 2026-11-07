// SPDX-License-Identifier: GPL-3.0-only
#include "exception_diagnostics.h"
#include "pipeline.h"
#include "service.h"
#include <algorithm>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <future>
#include <thread>
using namespace neural;
static std::vector<unsigned char> Read(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    unsigned bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&d, nullptr, &staging), "Create test readback");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read test output");
    std::vector<unsigned char> bytes(size_t(d.Width) * d.Height * bpp);
    for (unsigned y = 0; y < d.Height; y++)
        memcpy(bytes.data() + size_t(y) * d.Width * bpp,
               static_cast<char *>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(d.Width) * bpp);
    context->Unmap(staging.Get(), 0);
    return bytes;
}
int wmain(int argc, wchar_t **argv) {
    InstallExceptionDiagnostics();
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto usage = [] {
        puts("usage: neural_pipelinetest <model DLL> [--debug] [--preset 0|1|2|3]");
        return 2;
    };
    if (argc < 2)
        return usage();
    bool enableDebug = false;
    unsigned preset = 0;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--debug")) {
            enableDebug = true;
        } else if (!wcscmp(argv[i], L"--preset") && i + 1 < argc) {
            const auto value = argv[++i];
            if (value[0] < L'0' || value[0] > L'3' || value[1] != 0)
                return usage();
            preset = unsigned(value[0] - L'0');
        } else {
            return usage();
        }
    }
    printf("Model preset %u\n", preset);
    ComPtr<ID3D12Debug> debug;
    if (enableDebug) {
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "Enable test debug layer");
        debug->EnableDebugLayer();
    }
    std::unique_ptr<Pipeline> pipeline;
    try {
        Gpu audit;
        audit.Initialize();
        D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY prioritySupport{D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                 D3D12_COMMAND_QUEUE_PRIORITY_HIGH, FALSE};
        Check(audit.device->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &prioritySupport,
                                               sizeof(prioritySupport)),
              "Query test queue priorities");
        ComPtr<ID3D12InfoQueue> messages;
        if (debug) {
            audit.device.As(&messages);
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create test capture device");
        ComPtr<IDXGIDevice> captureDxgi;
        Check(device.As(&captureDxgi), "Query test capture priority");
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto folder = std::filesystem::path(exe).parent_path();
        for (unsigned scenario = 0; scenario < 6; scenario++) {
            const int capturePriority = scenario % 2 ? 7 : 0;
            Check(captureDxgi->SetGPUThreadPriority(capturePriority), "Set test capture priority");
            unsigned w = scenario >= 4 ? 1920 : scenario == 3 ? 804 : 644, h = scenario >= 4 ? 1080 : scenario == 3 ? 621 : 481;
            bool hdr = scenario == 2;
            unsigned bpp = hdr ? 8 : 4;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> original, output;
            Check(device->CreateTexture2D(&d, nullptr, &original), "Create original");
            Check(device->CreateTexture2D(&d, nullptr, &output), "Create output");
            PipelineSettings settings;
            settings.modelPath = std::filesystem::absolute(argv[1]).wstring();
            settings.forwarderPath = (folder / L"NeuralRendering_nvngx.dll").wstring();
            settings.cachePath = (folder / L"pipeline-test-cache").wstring();
            settings.scalePercent = (scenario == 1 || scenario == 5) ? 50 : 100;
            settings.model.intensity = scenario == 0 ? 0 : 1;
            settings.model.preset = preset;
            pipeline = std::make_unique<Pipeline>();
            pipeline->Initialize(device.Get(), w, h, hdr ? Encoding::ScRgb : Encoding::Sdr709, settings);
            const auto priorities = pipeline->Metrics();
            const int expectedPriority = capturePriority > 0 && prioritySupport.PriorityForTypeIsSupported
                                             ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH
                                             : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            if (priorities.captureGpuPriority != capturePriority || priorities.nrQueuePriority != expectedPriority)
                throw std::runtime_error("NR queue did not follow capture GPU priority");
            printf("PASS GPU priority: capture=%d NR=%d\n", priorities.captureGpuPriority, priorities.nrQueuePriority);
            std::vector<unsigned char> bytes(size_t(w) * h * bpp);
            unsigned accepted = 0;
            double edits = 0;
            for (unsigned frame = 0; frame < 18; frame++) {
                // Translation, repetition, deliberate scene cut and explicit long gap reset.
                unsigned shift = frame < 6 ? frame * 3 : 18;
                settings.composition.apply = !(hdr && frame == 17);
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++) {
                        unsigned i = (y * w + x) * bpp;
                        float colour[4];
                        colour[0] = .08f + float(((x + shift) / 17 + y / 23) % 9) / 20;
                        colour[1] = .12f + float((y / 19) % 7) / 25;
                        colour[2] = .2f + float((x / 31) % 6) / 30;
                        colour[3] = 1;
                        if (frame >= 9) {
                            colour[0] = .95f - colour[0] * .1f;
                            colour[1] = .95f - colour[1] * .1f;
                            colour[2] = .95f - colour[2] * .1f;
                        }
                        if (hdr)
                            for (unsigned ch = 0; ch < 4; ch++)
                                reinterpret_cast<uint16_t *>(&bytes[i])[ch] =
                                    DirectX::PackedVector::XMConvertFloatToHalf((frame == 17 && ch == 0 ? -.05f : colour[ch]) * (ch == 3 ? 1 : 3));
                        else
                            for (unsigned ch = 0; ch < 4; ch++)
                                bytes[i + ch] = static_cast<unsigned char>(colour[ch] * 255 + .5f);
                    }
                context->UpdateSubresource(original.Get(), 0, nullptr, bytes.data(), w * bpp, 0);
                context->CopyResource(output.Get(), original.Get());
                EamRealFrame f;
                f.context = context.Get();
                f.original = original.Get();
                f.output = output.Get();
                f.width = w;
                f.height = h;
                f.encoding = hdr ? EamFrameEncoding::ScRgb : EamFrameEncoding::Sdr709;
                f.sequence = frame + 1 + (frame >= 14 ? 3 : 0);
                f.epoch = scenario + 1;
                pipeline->Process(f, settings.composition);
                accepted++;
                const auto out = Read(device.Get(), context.Get(), output.Get());
                const auto unchanged = Read(device.Get(), context.Get(), original.Get());
                if (unchanged != bytes)
                    throw std::runtime_error("Original capture was modified");
                if ((scenario == 0 || !settings.composition.apply) && out != bytes)
                    throw std::runtime_error("Apply-off/intensity-zero identity failed");
                if (hdr)
                    for (size_t p = 0; p < out.size(); p += 2)
                        if (!std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(
                                *reinterpret_cast<const uint16_t *>(&out[p]))))
                            throw std::runtime_error("Non-finite HDR output");
                if (out != bytes)
                    edits++;
            }
            const auto m = pipeline->Metrics();
            printf("Scenario %u: %ux%u work=%ux%u processed=%u edits=%.0f cuts=%llu resets=%llu "
                   "prepare=%.3f flow=%.3f model=%.3f composition=%.3f summarywait=%.3f ms\n",
                   scenario, w, h, m.workingWidth, m.workingHeight, accepted, edits, m.cuts, m.resets,
                   m.prepareMs, m.flowMs, m.modelMs, m.compositionMs, m.summaryWaitMs);
            if (accepted != 18 || m.accepted != 18 || m.cuts < 1 || m.resets < 3 || (scenario > 0 && edits < 10))
                throw std::runtime_error("Pipeline coverage requirements failed");
            if (scenario == 1) {
                // Exercise both scene-summary and full-slot waits. Delays beyond the old
                // deadline must still process every frame, with the model result applied.
                ComPtr<ID3D11Device5> d5;
                Check(device.As(&d5), "Test fence device");
                ComPtr<ID3D11DeviceContext4> c4;
                Check(context.As(&c4), "Test fence context");
                ComPtr<ID3D11Fence> hold11;
                Check(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&hold11)), "Test hold fence");
                Gpu testGpu;
                testGpu.Initialize();
                HANDLE handle = nullptr;
                Check(hold11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "Share test hold");
                ComPtr<ID3D12Fence> hold12;
                HRESULT opened = testGpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&hold12));
                CloseHandle(handle);
                Check(opened, "Open test hold");
                for (unsigned phase = 0; phase < 2; ++phase) {
                const uint64_t releaseValue = phase + 1;
                Check(c4->Wait(hold11.Get(), releaseValue), "Hold capture queue");
                context->Flush();
                std::thread release([hold12, releaseValue] {
                    Sleep(100);
                    hold12->Signal(releaseValue);
                });
                const auto before = pipeline->Metrics().accepted;
                double summaryWait = 0, slotWait = 0;
                try {
                    for (unsigned n = 0; n < 6; n++) {
                        EamRealFrame f;
                        f.context = context.Get();
                        f.original = original.Get();
                        f.output = output.Get();
                        f.width = w;
                        f.height = h;
                        f.sequence = 22 + phase * 6 + n;
                        f.epoch = 1;
                        if (phase == 1) pipeline->Invalidate();
                        pipeline->Process(f, settings.composition);
                        summaryWait = std::max(summaryWait, pipeline->Metrics().summaryWaitMs);
                        slotWait = std::max(slotWait, pipeline->Metrics().slotWaitMs);
                    }
                } catch (...) {
                    hold12->Signal(releaseValue);
                    release.join();
                    throw;
                }
                hold12->Signal(releaseValue);
                release.join();
                const auto enhanced = Read(device.Get(), context.Get(), output.Get());
                const auto overloaded = pipeline->Metrics();
                if (overloaded.accepted != before + 6 || enhanced == bytes ||
                    (phase == 0 ? summaryWait : slotWait) < 20)
                    throw std::runtime_error("Overload did not wait and enhance every frame");
                printf("PASS overload phase %u: all 6 processed, summary/slot waits %.2f/%.2f ms; queue recovered\n",
                       phase, summaryWait, slotWait);
                }
            }
            if (!pipeline->Retire())
                throw std::runtime_error("Pipeline retirement failed");
            pipeline.reset();
            if (scenario == 3) {
                Service service;
                settings.modelPath = L"missing-user-model.dll";
                service.Configure(settings, true, true);
                unsigned attempts = 0;
                auto process = [&](uint64_t epoch) {
                        EamRealFrame f;
                        f.context = context.Get();
                        f.original = original.Get();
                        f.output = output.Get();
                        f.width = w;
                        f.height = h;
                        f.sequence = ++attempts;
                        f.epoch = epoch;
                        return service.Process(f);
                };
                auto waitFor = [&](uint64_t epoch) {
                    if (!process(epoch)) throw std::runtime_error("Enabled service returned an original frame");
                    Read(device.Get(), context.Get(), output.Get());
                };
                auto pending = std::async(std::launch::async, [&] { return process(1); });
                const auto until = GetTickCount64() + 10000;
                while (!service.Snapshot().failed && GetTickCount64() < until) Sleep(5);
                if (!service.Snapshot().failed || pending.wait_for(std::chrono::milliseconds(50)) != std::future_status::timeout) {
                    service.Configure(settings, false, false);
                    pending.wait();
                    throw std::runtime_error("Initialization failure did not pause capture");
                };
                printf("PASS failure pauses capture: %s\n", service.Snapshot().status.c_str());
                settings.modelPath = std::filesystem::absolute(argv[1]).wstring();
                service.Configure(settings, true, true);
                if (!pending.get()) throw std::runtime_error("Retry bypassed the waiting capture");
                Read(device.Get(), context.Get(), output.Get());
                waitFor(1);
                settings.model.intensity = 0;
                service.Configure(settings, true, true);
                waitFor(1);
                service.Configure(settings, false, false);
                if (service.Snapshot().enabled)
                    throw std::runtime_error("Disable failed");
                service.Configure(settings, true, true);
                waitFor(2);
                puts("PASS worker initialization failure/retry, settings recreation, disable/re-enable and epoch "
                     "change");
                settings.model.intensity = 1;
                for (unsigned selected : {1u, 2u, 0u}) {
                    settings.model.preset = selected;
                    service.Configure(settings, true, true);
                    waitFor(2);
                }
                puts("PASS live Original -> Fast -> default model recreation");
                settings.modelPath = L"missing-user-model.dll";
                service.Configure(settings, true, true);
                auto disabled = std::async(std::launch::async, [&] { return process(2); });
                const auto disableUntil = GetTickCount64() + 10000;
                while (!service.Snapshot().failed && GetTickCount64() < disableUntil) Sleep(5);
                const bool paused = service.Snapshot().failed &&
                                    disabled.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
                service.Configure(settings, false, false);
                if (disabled.get() || !paused || service.Snapshot().failed)
                    throw std::runtime_error("Explicit disable did not release the failed capture");
                puts("PASS explicit disable releases paused initialization and clears the error");
            }
        }
        if (messages) {
            unsigned errors = 0;
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); i++) {
                SIZE_T size = 0;
                messages->GetMessage(i, nullptr, &size);
                std::vector<unsigned char> bytes(size);
                auto *m = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
                messages->GetMessage(i, m, &size);
                if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    printf("GPU VALIDATION: %s\n", m->pDescription);
                    errors++;
                }
            }
            if (errors)
                throw std::runtime_error("D3D12 validation errors");
            puts("PASS D3D12 debug layer: zero errors");
        }
        puts("PIPELINE TEST PASSED");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "PIPELINE TEST ERROR: %s\n", e.what());
        if (pipeline && !pipeline->Retire())
            pipeline.release();
        return 1;
    }
}
