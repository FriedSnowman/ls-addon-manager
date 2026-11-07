#include "real_frame_hook.h"
#include "eam/shader_identity.h"
#include "../host/host_impl.h"
#include "../addon/file_hash.h"
#include <wrl/client.h>
#include <atomic>
#include <cstring>
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
void PauseUnsupported(eam::HostImpl* host, EamFrameSupport reason) {
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
    const bool hdr = ((config>>24)&1)!=0;
    const bool sdr = d.Format==DXGI_FORMAT_R8G8B8A8_UNORM && !hdr;
    const bool scrgb = d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && hdr;
    if((!sdr&&!scrgb)||d.SampleDesc.Count!=1||d.ArraySize!=1||!d.MipLevels||!d.Width||!d.Height) {
        PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);
        return false;
    }
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(state.device!=device||state.settings!=config||state.width!=d.Width||state.height!=d.Height||state.format!=d.Format) {
        state.transformed.Reset();state.transformedView.Reset();state.device=device;
        state.settings=config;state.width=d.Width;state.height=d.Height;state.format=d.Format;state.epoch=++epochs;
    }
    if(!state.transformed) {
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.MiscFlags=0;d.MipLevels=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.transformed)) ||
           FAILED(device->CreateShaderResourceView(state.transformed.Get(),nullptr,&state.transformedView))) {
            PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);
            return false;
        }
    }
    // WGC's originals have a mip chain; these verified consumers sample mip zero.
    ctx->CopySubresourceRegion(state.transformed.Get(),0,0,0,0,original,0,nullptr);
    EamRealFrame frame{};frame.context=ctx;frame.original=original;frame.output=state.transformed.Get();
    frame.width=d.Width;frame.height=d.Height;frame.encoding=sdr?EamFrameEncoding::Sdr709:EamFrameEncoding::ScRgb;
    frame.sequence=++serial;frame.epoch=state.epoch;
    support=EamFrameSupport::Ready;
    return host->InvokeRealFrame(frame);
}
}
void VerifyRuntime(HMODULE module) {
    wchar_t path[32768]{};
    runtimeVerified=module && GetModuleFileNameW(module,path,32768) &&
        eam::FileSHA256(path)=="626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb";
}
void SettingsChanging(){settings=0;support=EamFrameSupport::SettingsChanging;}
void SetSettings(int fg,int capture,int scaler,bool hdr) {
    const uint64_t generation=(++epochs)<<32;
    settings=generation|(uint64_t(fg&255))|(uint64_t(capture&255)<<8)|(uint64_t(scaler&255)<<16)|(uint64_t(hdr)<<24);
    support=EamFrameSupport::Waiting;
}
EamFrameSupport Support(){return support.load();}
void Before(eam::HostImpl* host,ID3D11DeviceContext* ctx,UINT x,UINT y,UINT z) {
    state.candidate.Reset();state.savedView.Reset();state.replace=false;
    if(!host->HasRealFrameCallback())return;
    uint64_t config=0;
    for (;;) {
        config=settings.load();
        const unsigned fg=config&255,capture=(config>>8)&255,scaler=(config>>16)&255;
        EamFrameSupport problem=EamFrameSupport::Ready;
        if(!runtimeVerified.load())problem=EamFrameSupport::UnsupportedRuntime;
        else if(!config)problem=EamFrameSupport::SettingsChanging;
        else if(capture!=1)problem=EamFrameSupport::UnsupportedCapture;
        else if(fg!=0&&fg!=1)problem=EamFrameSupport::UnsupportedFrameGeneration;
        else if(fg==0&&scaler!=0&&scaler!=3)problem=EamFrameSupport::UnsupportedScaler;
        if(problem==EamFrameSupport::Ready)break;
        support=problem;
        if(!host->RealFrameRequired())return;
        Sleep(25);
    }
    const unsigned fg=config&255,scaler=(config>>16)&255;
    ComPtr<ID3D11ComputeShader> cs;ctx->CSGetShader(&cs,nullptr,nullptr);eam::ShaderSha256 sha{};
    if(!eam::ReadShaderSha256(cs.Get(),sha)){
        PauseUnsupported(host,EamFrameSupport::UnrecognizedShader);
        return;
    }
    if(fg==1&&Match(sha,kCapture)) {
        ComPtr<ID3D11UnorderedAccessView> output;ctx->CSGetUnorderedAccessViews(0,1,&output);
        auto t=Texture(output.Get());
        if(!t){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);return;}
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(x!=(d.Width+7)/8||y!=(d.Height+7)/8||z!=1||d.MiscFlags!=0){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);return;}
        state.candidate=t;state.candidateSettings=config; // before any luminance/flow/history consumer
    } else if(fg==0&&Match(sha,scaler==0?kCopy:kNis)) {
        ctx->CSGetShaderResources(0,1,&state.savedView);auto t=Texture(state.savedView.Get());
        if(!t){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);return;}
        if(state.savedView){D3D11_SHADER_RESOURCE_VIEW_DESC v{};state.savedView->GetDesc(&v);if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||v.Texture2D.MostDetailedMip!=0){PauseUnsupported(host,EamFrameSupport::UnsupportedTexture);return;}}
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
