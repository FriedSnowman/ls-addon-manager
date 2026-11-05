// Diagnostic only: timestamp Presents and asynchronously sample one output row.
// Never linked into the shipping manager or addon. No CPU GPU-completion waits.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>
#include <wrl/client.h>
namespace pacing {
using Microsoft::WRL::ComPtr;
inline int Env(const char *name, int fallback) {
    char text[64]{};
    return GetEnvironmentVariableA(name, text, sizeof(text)) ? atoi(text) : fallback;
}
inline double Now() {
    LARGE_INTEGER t{}, f{};
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) / f.QuadPart;
}
struct Sample {
    double time, position;
    uint64_t gpu = 0;
};
struct Pending {
    ComPtr<ID3D11Texture2D> row;
    ComPtr<ID3D11Query> stamp;
    bool busy = false;
    double time = 0;
};
inline std::array<Pending, 16> pending;
inline std::vector<Sample> samples;
inline std::vector<double> presents, captures, callbacks;
inline std::vector<DXGI_FRAME_STATISTICS> display;
inline HRESULT statisticsResult = E_FAIL;
inline ComPtr<ID3D11Device> device;
inline ComPtr<ID3D11DeviceContext> context;
inline ComPtr<ID3D11Query> disjoint;
inline unsigned cursor = 0, dropped = 0, width = 0;
inline double start = 0;
inline bool warmed = false, timingEnded = false;
inline double timingEnd = 0;
inline D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timingResult{};
inline bool enabled = false;
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
inline PresentFn original = nullptr;
inline Present1Fn original1 = nullptr;
inline thread_local bool inside = false;
inline void SampleOutput(IDXGISwapChain *swap) {
    const double now = Now();
    if (!start)
        start = now;
    presents.push_back(now - start);
    DXGI_FRAME_STATISTICS stats{};
    statisticsResult = swap->GetFrameStatistics(&stats);
    if (SUCCEEDED(statisticsResult) && (display.empty() || display.back().PresentCount != stats.PresentCount))
        display.push_back(stats);
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        return;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return;
    ComPtr<ID3D11Device> current;
    back->GetDevice(&current);
    if (device != current || width != d.Width) {
        pending = {};
        device = current;
        device->GetImmediateContext(&context);
        width = d.Width;
        auto rowDesc = d;
        rowDesc.Height = 1;
        rowDesc.MipLevels = 1;
        rowDesc.ArraySize = 1;
        rowDesc.Usage = D3D11_USAGE_STAGING;
        rowDesc.BindFlags = 0;
        rowDesc.MiscFlags = 0;
        rowDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device->CreateQuery(&q, &disjoint);
        context->Begin(disjoint.Get());
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto &p : pending) {
            if (FAILED(device->CreateTexture2D(&rowDesc, nullptr, &p.row)))
                return;
            device->CreateQuery(&q, &p.stamp);
        }
        ComPtr<IDXGIDevice> dxgi;
        int priority = 0;
        if (SUCCEEDED(device.As(&dxgi)))
            dxgi->GetGPUThreadPriority(&priority);
        printf("PACING output %ux%u format %u priority=%d\n", d.Width, d.Height, d.Format, priority);
    }
    if (!warmed && now - start > 3) {
        context->End(disjoint.Get());
        disjoint.Reset();
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device->CreateQuery(&q, &disjoint);
        context->Begin(disjoint.Get());
        warmed = true;
    }
    if (warmed && !timingEnded && now - start > Env("EAM_PROBE_SECONDS", 15) - 3) {
        context->End(disjoint.Get());
        timingEnd = now - start;
        timingEnded = true;
    }
    if (timingEnded && !timingResult.Frequency)
        context->GetData(disjoint.Get(), &timingResult, sizeof(timingResult), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    for (auto &p : pending)
        if (p.busy) {
            uint64_t ticks = 0;
            if (context->GetData(p.stamp.Get(), &ticks, sizeof(ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (context->Map(p.row.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
                double weight = 0, sum = 0;
                auto *bytes = static_cast<unsigned char *>(m.pData);
                for (unsigned x = 0; x < width; ++x) {
                    double v = (std::max)(0, int(bytes[x * 4]) - 128);
                    sum += v * x;
                    weight += v;
                }
                samples.push_back({p.time, weight > 100 ? sum / weight : -1, ticks});
                context->Unmap(p.row.Get(), 0);
                p.busy = false;
            }
        }
    auto &p = pending[cursor++ % pending.size()];
    if (p.busy || !p.row) {
        ++dropped;
        return;
    }
    D3D11_BOX box{0, d.Height / 2, 0, d.Width, d.Height / 2 + 1, 1};
    context->CopySubresourceRegion(p.row.Get(), 0, 0, 0, 0, back.Get(), 0, &box);
    context->End(p.stamp.Get());
    p.busy = true;
    p.time = now - start;
}
inline HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    const bool outer = !inside;
    inside = true;
    if (outer && enabled && !(flags & DXGI_PRESENT_TEST))
        SampleOutput(sc);
    const auto hr = original(sc, sync, flags);
    if (outer)
        inside = false;
    return hr;
}
inline HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS *p) {
    const bool outer = !inside;
    inside = true;
    if (outer && enabled && !(flags & DXGI_PRESENT_TEST))
        SampleOutput(sc);
    const auto hr = original1(sc, sync, flags, p);
    if (outer)
        inside = false;
    return hr;
}
inline void Install() {
    if (!Env("EAM_PROBE_PACING", 0))
        return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EamPacingDummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferCount = 2;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> sc;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx))) {
        auto patch = [](void **slot, void *replacement) {
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                return (void *)nullptr;
            void *previous = InterlockedExchangePointer(slot, replacement);
            DWORD ignored;
            VirtualProtect(slot, sizeof(void *), old, &ignored);
            return previous;
        };
        auto table = *reinterpret_cast<void ***>(sc.Get());
        original = reinterpret_cast<PresentFn>(patch(table + 8, reinterpret_cast<void *>(&Present)));
        ComPtr<IDXGISwapChain1> sc1;
        if (SUCCEEDED(sc.As(&sc1))) {
            auto t = *reinterpret_cast<void ***>(sc1.Get());
            original1 = reinterpret_cast<Present1Fn>(patch(t + 22, reinterpret_cast<void *>(&Present1)));
        }
        enabled = original != nullptr;
    }
    DestroyWindow(hwnd);
}
inline void Stats(const char *label, std::vector<double> values, const char *unit = "ms") {
    if (values.empty()) {
        printf("PACING %s no samples\n", label);
        return;
    }
    double sum = 0;
    for (double x : values)
        sum += x;
    std::sort(values.begin(), values.end());
    auto q = [&](double f) { return values[size_t(f * (values.size() - 1))]; };
    printf("PACING %s n=%zu mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f %s\n", label, values.size(),
           sum / values.size(), q(.5), q(.95), q(.99), values.back(), unit);
}
inline void Report() {
    if (!enabled)
        return;
    enabled = false;
    auto intervals = [](const std::vector<double> &times) {
        std::vector<double> v;
        for (size_t i = 1; i < times.size(); ++i)
            if (times[i - 1] > 3)
                v.push_back(1000 * (times[i] - times[i - 1]));
        return v;
    };
    Stats("Present intervals", intervals(presents));
    Stats("capture intervals", intervals(captures));
    Stats("callback CPU", callbacks);
    std::sort(samples.begin(), samples.end(), [](auto a, auto b) { return a.time < b.time; });
    const auto timing = timingResult;
    std::vector<double> gpuIntervals;
    if (!timing.Disjoint && timing.Frequency)
        for (size_t i = 1; i < samples.size(); ++i)
            if (samples[i - 1].time > 3 && samples[i].time < timingEnd && samples[i - 1].gpu && samples[i].gpu)
                gpuIntervals.push_back(1000.0 * (samples[i].gpu - samples[i - 1].gpu) / timing.Frequency);
    printf("PACING timestamp disjoint=%u frequency=%llu\n", timing.Disjoint, timing.Frequency);
    Stats("GPU output intervals", gpuIntervals);
    unsigned skippedDisplay = 0;
    for (size_t n = 1; n < display.size(); ++n)
        if (display[n].PresentCount > display[n - 1].PresentCount + 1)
            skippedDisplay += display[n].PresentCount - display[n - 1].PresentCount - 1;
    printf("PACING display statistics hr=%08x records=%zu present-count gaps=%u\n", unsigned(statisticsResult),
           display.size(), skippedDisplay);
    unsigned repeated = 0, moving = 0;
    std::vector<double> steps;
    for (size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1].time > 3 && samples[i - 1].position >= 0 && samples[i].position >= 0) {
            double step = samples[i].position - samples[i - 1].position;
            if (std::abs(step) < .25)
                ++repeated;
            else
                ++moving;
            if (std::abs(step) < 100)
                steps.push_back(step);
        }
    printf("PACING output repeated=%u moving=%u readbackDrops=%u\n", repeated, moving, dropped);
    char path[32768]{};
    if (GetEnvironmentVariableA("EAM_PROBE_CSV", path, sizeof(path))) {
        FILE *file = nullptr;
        fopen_s(&file, path, "w");
        if (file) {
            fprintf(file, "seconds,bar_center,gpu_seconds\n");
            for (auto s : samples)
                fprintf(file, "%.9f,%.4f,%.9f\n", s.time, s.position,
                        timing.Frequency ? double(s.gpu - samples[0].gpu) / timing.Frequency : 0);
            fclose(file);
        }
    }
    Stats("motion step", steps, "pixels");
}
struct Workload {
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> uav;
    void Init(ID3D11Device *d) {
        if (!Env("EAM_PROBE_GPU_LOAD", 0))
            return;
        const char *code =
            "RWTexture2D<float> O:register(u0);[numthreads(16,16,1)]void main(uint3 p:SV_DispatchThreadID){float "
            "x=(p.x+p.y*0.123)*0.001;[loop]for(uint i=0;i<ITERATIONS;i++)x=sin(x*1.37+0.01);O[p.xy]=x;}";
        const auto iterations = std::to_string((std::clamp)(Env("EAM_PROBE_GPU_ITERATIONS", 128), 1, 16384));
        const D3D_SHADER_MACRO macros[] = {{"ITERATIONS", iterations.c_str()}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> blob, error;
        HRESULT compile = D3DCompile(code, strlen(code), nullptr, macros, nullptr, "main", "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
        FILE *log = nullptr;
        fopen_s(&log, "build/pacing-source-load.log", "w");
        if (log) {
            fprintf(log, "pid=%lu repeats=%d compile=%08x %s\n", GetCurrentProcessId(), Env("EAM_PROBE_GPU_LOAD", 0),
                    unsigned(compile), error ? (char *)error->GetBufferPointer() : "");
            fclose(log);
        }
        if (FAILED(compile))
            return;
        d->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1024;
        desc.Height = 1024;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d->CreateTexture2D(&desc, nullptr, &texture);
        d->CreateUnorderedAccessView(texture.Get(), nullptr, &uav);
    }
    void Run(ID3D11DeviceContext *c) {
        if (!shader)
            return;
        c->CSSetShader(shader.Get(), nullptr, 0);
        auto *v = uav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &v, nullptr);
        for (int n = 0; n < Env("EAM_PROBE_GPU_LOAD", 0); ++n)
            c->Dispatch(64, 64, 1);
        v = nullptr;
        c->CSSetUnorderedAccessViews(0, 1, &v, nullptr);
    }
};
} // namespace pacing
