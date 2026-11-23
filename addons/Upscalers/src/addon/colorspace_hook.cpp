#include "addon/colorspace_hook.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

// As in present_hook.cpp: the slots are patched in DXGI's static function tables (shared by every object of the kind, never refreshed per
// object), once for the life of the process, and this DLL is pinned so the functions stay in memory after the manager unloads the addon. They
// only note what they see, and call on to what was in the slot before (the real function, or a hook placed before ours).
namespace {
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                     IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
// IDXGIFactory::CreateSwapChain, IDXGIFactory2::CreateSwapChainForHwnd and ::CreateSwapChainForComposition, IDXGISwapChain3::SetColorSpace1
constexpr int kCreateSwapChainSlot = 10, kCreateForHwndSlot = 15, kCreateForCompositionSlot = 24, kSetColorSpace1Slot = 38;

CreateSwapChainFn g_createSwapChain = nullptr;
CreateForHwndFn g_createForHwnd = nullptr;
CreateForCompositionFn g_createForComposition = nullptr;
SetColorSpace1Fn g_setColorSpace1 = nullptr;
std::atomic<bool> g_installed{ false };
std::mutex g_mutex;
std::unordered_map<const void*, DXGI_COLOR_SPACE_TYPE> g_spaces;   // by the swap chain's IUnknown (a released chain's entry stays until
                                                                   // a new chain at its address is made, which replaces it)

const void* Identity(IUnknown* object) {
    IUnknown* id = nullptr;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&id))) || !id) return nullptr;
    id->Release();
    return id;
}
void Remember(IUnknown* chain, DXGI_COLOR_SPACE_TYPE space) {
    const void* id = Identity(chain);
    if (!id) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_spaces.size() > 256) g_spaces.clear();   // (swap chains made by the hundred: start over rather than grow)
    g_spaces[id] = space;
}
DXGI_COLOR_SPACE_TYPE DefaultFor(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

HRESULT STDMETHODCALLTYPE OnCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const HRESULT hr = g_createSwapChain(f, dev, desc, out);
    if (SUCCEEDED(hr) && desc && out && *out) Remember(*out, DefaultFor(desc->BufferDesc.Format));
    return hr;
}
HRESULT STDMETHODCALLTYPE OnCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = g_createForHwnd(f, dev, hwnd, desc, full, output, out);
    if (SUCCEEDED(hr) && desc && out && *out) Remember(*out, DefaultFor(desc->Format));
    return hr;
}
HRESULT STDMETHODCALLTYPE OnCreateForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = g_createForComposition(f, dev, desc, output, out);
    if (SUCCEEDED(hr) && desc && out && *out) Remember(*out, DefaultFor(desc->Format));
    return hr;
}
HRESULT STDMETHODCALLTYPE OnSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space) {
    const HRESULT hr = g_setColorSpace1(sc, space);
    if (SUCCEEDED(hr)) Remember(sc, space);
    return hr;
}

// Puts fn into the table's slot, the function it replaces first into *original (so a call that comes in at once finds it). False: the
// page could not be made writable, or the slot was empty.
template <class Fn> bool Patch(void** table, int slot, void* fn, Fn* original) {
    if (!table || !table[slot]) return false;
    DWORD protect = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *original = reinterpret_cast<Fn>(table[slot]);
    table[slot] = fn;
    VirtualProtect(table + slot, sizeof(void*), protect, &protect);
    return true;
}
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
} // namespace

bool ColorSpaceHook::Install(LogFn log) {
    if (g_installed.load()) return true;
    // a device of our own (none is taken from the host at the start), its factory and a window swap chain like Lossless Scaling's
    ID3D11Device* dev = nullptr; IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr; IDXGISwapChain3* sc3 = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrColorSpaceProbe";
    RegisterClassExW(&wc);   // fails harmlessly when a previous install registered it
    const HWND hwnd = SUCCEEDED(hr) ? CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr) : nullptr;
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 16; desc.Height = 16; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.Scaling = DXGI_SCALING_NONE; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = hwnd ? factory->CreateSwapChainForHwnd(dev, hwnd, &desc, nullptr, nullptr, &sc) : E_FAIL;
        if (SUCCEEDED(hr)) hr = sc->QueryInterface(IID_PPV_ARGS(&sc3));
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        // the tables of the very interfaces the calls go through (IDXGIFactory's may be its own if the factory's interfaces do not share one)
        IDXGIFactory* factory0 = nullptr; factory->QueryInterface(IID_PPV_ARGS(&factory0));
        void** const factoryTable = *reinterpret_cast<void***>(factory);
        void** const factory0Table = factory0 ? *reinterpret_cast<void***>(factory0) : factoryTable;
        void** const chainTable = *reinterpret_cast<void***>(sc3);
        SafeRelease(factory0);
        ok = Patch(chainTable, kSetColorSpace1Slot, reinterpret_cast<void*>(&OnSetColorSpace1), &g_setColorSpace1);
        const bool created = ok && Patch(factory0Table, kCreateSwapChainSlot, reinterpret_cast<void*>(&OnCreateSwapChain), &g_createSwapChain) &&
                             Patch(factoryTable, kCreateForHwndSlot, reinterpret_cast<void*>(&OnCreateForHwnd), &g_createForHwnd) &&
                             Patch(factoryTable, kCreateForCompositionSlot, reinterpret_cast<void*>(&OnCreateForComposition), &g_createForComposition);
        char text[160];
        snprintf(text, sizeof text, "ColorSpaceHook: %s", !ok ? "SetColorSpace1 could not be patched; 10-bit frames are judged by the display's HDR state"
                                                       : created ? "following the swap chains' colour spaces" : "following SetColorSpace1 (not every swap chain made)");
        log(text);
        if (ok) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnSetColorSpace1), &self);
            g_installed = true;
        }
    } else {
        char text[128]; snprintf(text, sizeof text, "ColorSpaceHook: the probe swap chain failed (0x%08x); 10-bit frames are judged by the display's HDR state", (unsigned)hr);
        log(text);
    }
    SafeRelease(sc3); SafeRelease(sc); SafeRelease(factory); SafeRelease(adapter); SafeRelease(dxgi); SafeRelease(dev);
    if (hwnd) DestroyWindow(hwnd);
    return ok;
}

bool ColorSpaceHook::Installed() { return g_installed.load(); }

bool ColorSpaceHook::Known(IUnknown* chain, DXGI_COLOR_SPACE_TYPE& space) {
    if (!g_installed.load()) return false;
    const void* id = Identity(chain);
    if (!id) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_spaces.find(id);
    if (it == g_spaces.end()) return false;
    space = it->second;
    return true;
}
