#include "real_frame_hook.h"
#include "eam/shader_identity.h"
#include "eam/real_frame_status.h"
#include "../host/host_impl.h"
#include "../addon/file_hash.h"
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <cstdio>
#pragma comment(lib, "version.lib")
using Microsoft::WRL::ComPtr;
namespace RealFrameHook {
namespace {
// Bytecode identities of the independently traced LS 3.2.2 WGC path. No proprietary
// shader bytes are distributed. Required transforms pause on unsupported profiles.
constexpr char kCapture[] = "214a8ad496c0ffff58e9739d4be085a450d82eaaf23e2ad21dea8b251872880f";
constexpr char kCopy[] = "14e1e62485505bced3d2d9946fc591255ee43b49927f50b4153fe56cc0242ce8";
constexpr char kNis[] = "fc8f0300fec6f80250b8ec605265ba91c80f1d897fe799004f2cb95e14ca3d02";
std::atomic<uint64_t> settings{0}, serial{0}, epochs{0};
std::atomic<bool> runtimeVerified{false};
std::atomic<bool> runtimeChecked{false}, hookFailed{false};
std::atomic<uint64_t> dispatches{0}, captures{0}, lastDispatch{0}, lastCapture{0}, settingsAt{0};
std::mutex diagnosticMutex;
Diagnostics diagnostic;
std::atomic<EamFrameSupport> support{EamFrameSupport::Waiting};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> transformed, candidate;
    ComPtr<ID3D11ShaderResourceView> transformedView, savedView;
    uint64_t settings = 0, epoch = 0, candidateSettings = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool replace = false;
};
thread_local State state;
void PauseUnsupported(eam::HostImpl* host, EamFrameSupport reason, const char* detail = "") {
    { std::lock_guard<std::mutex> lock(diagnosticMutex);diagnostic.captureError=detail; }
    if(*detail)host->Log(EAM_LOG_WARN,detail);
    support=reason;
    while(host->RealFrameRequired())Sleep(25);
}
bool Match(const eam::ShaderSha256& sha, const char* hex) {
    auto digit=[](char c){ return c<='9'?c-'0':c-'a'+10; };
    for(size_t i=0;i<sha.size();++i)if(sha[i]!=(digit(hex[i*2])*16+digit(hex[i*2+1])))return false;
    return true;
}
ComPtr<ID3D11Texture2D> Texture(ID3D11View* view) {
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    if(view){view->GetResource(&resource);resource.As(&texture);}return texture;
}
bool Transform(eam::HostImpl* host, ID3D11DeviceContext* ctx, ID3D11Texture2D* original, uint64_t config) {
    D3D11_TEXTURE2D_DESC d{};original->GetDesc(&d);
    { std::lock_guard<std::mutex> lock(diagnosticMutex); diagnostic.width=d.Width;diagnostic.height=d.Height;diagnostic.format=d.Format;diagnostic.captureError.clear(); }
    const bool hdr = ((config>>24)&1)!=0;
    const bool sdr = d.Format==DXGI_FORMAT_R8G8B8A8_UNORM && !hdr;
    const bool scrgb = d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && hdr;
    if((!sdr&&!scrgb)||d.SampleDesc.Count!=1||d.ArraySize!=1||!d.MipLevels||!d.Width||!d.Height) {
        char detail[200];snprintf(detail,sizeof detail,"Unsupported capture: %ux%u, format %u, HDR %d, samples %u, array %u, mips %u",d.Width,d.Height,unsigned(d.Format),hdr,d.SampleDesc.Count,d.ArraySize,d.MipLevels);
        PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,detail);
        return false;
    }
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(state.device!=device||state.settings!=config||state.width!=d.Width||state.height!=d.Height||state.format!=d.Format) {
        ObserveDevice(device.Get());
        state.transformed.Reset();state.transformedView.Reset();state.device=device;
        state.settings=config;state.width=d.Width;state.height=d.Height;state.format=d.Format;state.epoch=++epochs;
    }
    if(!state.transformed) {
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.MiscFlags=0;d.MipLevels=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        HRESULT result=device->CreateTexture2D(&d,nullptr,&state.transformed);
        if(SUCCEEDED(result))result=device->CreateShaderResourceView(state.transformed.Get(),nullptr,&state.transformedView);
        if(FAILED(result)) {
            state.transformed.Reset();state.transformedView.Reset();
            char detail[160];snprintf(detail,sizeof detail,"Capture handoff allocation failed (0x%08X); reduce capture size/check VRAM",unsigned(result));
            PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,detail);
            return false;
        }
    }
    // WGC's originals have a mip chain; these verified consumers sample mip zero.
    ctx->CopySubresourceRegion(state.transformed.Get(),0,0,0,0,original,0,nullptr);
    EamRealFrame frame{};frame.context=ctx;frame.original=original;frame.output=state.transformed.Get();
    frame.width=d.Width;frame.height=d.Height;frame.encoding=sdr?EamFrameEncoding::Sdr709:EamFrameEncoding::ScRgb;
    frame.sequence=++serial;frame.epoch=state.epoch;
    support=EamFrameSupport::Ready;
    ++captures; lastCapture=GetTickCount64();
    return host->InvokeRealFrame(frame);
}
}
void VerifyRuntime(HMODULE module) {
    wchar_t path[32768]{};
    runtimeVerified=module && GetModuleFileNameW(module,path,32768) &&
        eam::FileSHA256(path)==EamSupportedLsSha256;
    DWORD ignored=0;
    const DWORD size=GetFileVersionInfoSizeW(path,&ignored);
    std::vector<unsigned char> bytes(size);
    VS_FIXEDFILEINFO* info=nullptr;UINT count=0;
    std::lock_guard<std::mutex> lock(diagnosticMutex);
    if(size&&GetFileVersionInfoW(path,0,size,bytes.data())&&VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&count)&&count>=sizeof(*info)) {
        char version[64];snprintf(version,sizeof version,"%u.%u.%u.%u",HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS));
        diagnostic.runtimeVersion=version;
    }
    runtimeChecked=true;
}
void SettingsChanging(){settings=0;settingsAt=GetTickCount64();support=EamFrameSupport::SettingsChanging;}
void SetSettings(int fg,int capture,int scaler,bool hdr) {
    const uint64_t generation=(++epochs)<<32;
    settings=generation|(uint64_t(fg&255))|(uint64_t(capture&255)<<8)|(uint64_t(scaler&255)<<16)|(uint64_t(hdr)<<24);
    dispatches=0;captures=0;lastDispatch=0;lastCapture=0;settingsAt=GetTickCount64();
    const auto profile=EamProfileSupport(runtimeVerified,true,fg,capture,scaler);
    support=profile==EamFrameSupport::Ready?EamFrameSupport::Waiting:profile;
    { std::lock_guard<std::mutex> lock(diagnosticMutex);diagnostic.captureError.clear();diagnostic.width=0;diagnostic.height=0;diagnostic.format=0; }
}
EamFrameSupport Support(){
    if(runtimeChecked&&!runtimeVerified)return EamFrameSupport::UnsupportedRuntime;
    if(hookFailed)return EamFrameSupport::HookUnavailable;
    return support.load();
}
void HookFailed(const char* reason) {
    { std::lock_guard<std::mutex> lock(diagnosticMutex); diagnostic.hookError=reason; }
    hookFailed=true;
}
void CallbackFailed(){support=EamFrameSupport::CallbackFailed;}
void ObserveDevice(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(!device||FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&desc)))return;
    char name[512]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof name,nullptr,nullptr);
    LARGE_INTEGER version{};char driver[64]{};
    if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&version)))
        snprintf(driver,sizeof driver,"%u.%u.%u.%u",HIWORD(version.HighPart),LOWORD(version.HighPart),HIWORD(version.LowPart),LOWORD(version.LowPart));
    std::lock_guard<std::mutex> lock(diagnosticMutex);diagnostic.adapter=name;diagnostic.driver=driver;
    if(diagnostic.hookError.rfind("LS D3D11 device creation failed",0)==0){diagnostic.hookError.clear();hookFailed=false;}
}
Diagnostics GetDiagnostics() {
    std::lock_guard<std::mutex> lock(diagnosticMutex);auto d=diagnostic;
    const auto config=settings.load();d.configured=config!=0;d.fg=config&255;d.capture=(config>>8)&255;d.scaler=(config>>16)&255;d.hdr=((config>>24)&1)!=0;
    d.runtimeChecked=runtimeChecked;d.runtimeVerified=runtimeVerified;d.dispatches=dispatches;d.captures=captures;
    d.lastDispatch=lastDispatch;d.lastCapture=lastCapture;d.settingsAt=settingsAt;return d;
}
void Before(eam::HostImpl* host,ID3D11DeviceContext* ctx,UINT x,UINT y,UINT z) {
    state.candidate.Reset();state.savedView.Reset();state.replace=false;
    // Dispatch observation is also used by standalone upscalers. Only the
    // transform itself depends on an NR callback being registered.
    ++dispatches;lastDispatch=GetTickCount64();
    if(!host->HasRealFrameCallback())return;
    uint64_t config=0;
    for (;;) {
        config=settings.load();
        const unsigned fg=config&255,capture=(config>>8)&255,scaler=(config>>16)&255;
        const EamFrameSupport problem=EamProfileSupport(runtimeVerified,config!=0,fg,capture,scaler);
        if(problem==EamFrameSupport::Ready)break;
        support=problem;
        if(!host->RealFrameRequired())return;
        Sleep(25);
    }
    const unsigned fg=config&255,scaler=(config>>16)&255;
    ComPtr<ID3D11ComputeShader> cs;ctx->CSGetShader(&cs,nullptr,nullptr);eam::ShaderSha256 sha{};
    if(!eam::ReadShaderSha256(cs.Get(),sha)){
        PauseUnsupported(host,EamFrameSupport::UnrecognizedShader,"No bytecode identity on the LS compute shader; restart LS and check hook installation");
        return;
    }
    if(fg==1&&Match(sha,kCapture)) {
        ComPtr<ID3D11UnorderedAccessView> output;ctx->CSGetUnorderedAccessViews(0,1,&output);
        auto t=Texture(output.Get());
        if(!t){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,"LS capture output texture is missing");return;}
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(x!=(d.Width+7)/8||y!=(d.Height+7)/8||z!=1||d.MiscFlags!=0){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,"LS capture dispatch/texture shape differs from the supported path");return;}
        state.candidate=t;state.candidateSettings=config; // before any luminance/flow/history consumer
    } else if(fg==0&&Match(sha,scaler==0?kCopy:kNis)) {
        ctx->CSGetShaderResources(0,1,&state.savedView);auto t=Texture(state.savedView.Get());
        if(!t){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,"LS scaler input texture is missing");return;}
        if(state.savedView){D3D11_SHADER_RESOURCE_VIEW_DESC v{};state.savedView->GetDesc(&v);if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||v.Texture2D.MostDetailedMip!=0){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture,"LS scaler input view is not a mip-zero 2D texture");return;}}
        if(Transform(host,ctx,t.Get(),config)) {
            ID3D11ShaderResourceView* v=state.transformedView.Get();ctx->CSSetShaderResources(0,1,&v);state.replace=true;
        }
    }
}
void After(eam::HostImpl* host,ID3D11DeviceContext* ctx,UINT,UINT,UINT,bool executed) {
    if(state.replace) { ID3D11ShaderResourceView* v=state.savedView.Get();ctx->CSSetShaderResources(0,1,&v);state.replace=false; }
    if(!executed||!state.candidate)return;
    // Finish the exact capture identified by Before even if settings changed during
    // the dispatch. The next capture will enter a new epoch; this one still needs NR.
    const uint64_t config=state.candidateSettings;
    ComPtr<ID3D11UnorderedAccessView> uav;ctx->CSGetUnorderedAccessViews(0,1,&uav);
    ID3D11UnorderedAccessView* empty=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&empty,nullptr);
    if(Transform(host,ctx,state.candidate.Get(),config))ctx->CopyResource(state.candidate.Get(),state.transformed.Get());
    auto* restore=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&restore,nullptr);state.candidate.Reset();
}
void Shutdown(){SettingsChanging();}
}
