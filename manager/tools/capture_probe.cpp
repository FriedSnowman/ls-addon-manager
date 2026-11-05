// Invokes only the user's original LS DLL, against this process's animated test window.
// No game or installation is modified. Explicit path required; runs for eight seconds.
#include "src/core/d3d11_hook.h"
#include "src/core/real_frame_hook.h"
#include "src/host/host_impl.h"
#include "src/log/logger.h"
#include "eam/shader_identity.h"
#include "eam/addon_exports.h"
#include "src/host/metrics.h"
#include "imgui.h"
#include <filesystem>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <bcrypt.h>
#include <cstdio>
#include <map>
#include <string>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
using Microsoft::WRL::ComPtr;
namespace {
bool NeuralFrame(const EamRealFrame&,void*);
struct ProbeHost:eam::HostImpl {
    bool neural=false;
    EamRealFrameCallback callback=nullptr;void* user=nullptr;
    bool SetRealFrameCallback(EamRealFrameCallback cb,void* data=nullptr) override {
        if(!neural)return HostImpl::SetRealFrameCallback(cb,data);
        if(!cb){const bool result=HostImpl::SetRealFrameCallback(nullptr);callback=nullptr;user=nullptr;return result;}
        callback=cb;user=data;volatile bool installed=HostImpl::SetRealFrameCallback(NeuralFrame);return installed;
    }
} host;
bool verifyNeural=true;
unsigned neuralAccepted=0,neuralEdited=0;
std::map<ID3D11Resource*,std::vector<unsigned char>> neuralImages;
std::map<eam::ShaderSha256,unsigned> shaderIds;
unsigned passes=0,frame=0,realFrames=0,checked=0,failures=0,epochCount=0,epochFrames=0,clearedHistories=0;
bool firstInterpolation=true;
uint64_t lastEpoch=0;
bool markers=false;
std::map<ID3D11Resource*,unsigned> tags;
std::vector<unsigned char> Pixels(ID3D11DeviceContext* ctx,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return {};
    ctx->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return {};
    const unsigned bpp=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    std::vector<unsigned char> result(size_t(d.Width)*d.Height*bpp);
    for(unsigned y=0;y<d.Height;++y)memcpy(result.data()+size_t(y)*d.Width*bpp,(char*)map.pData+size_t(y)*map.RowPitch,size_t(d.Width)*bpp);
    ctx->Unmap(staging.Get(),0);return result;
}
bool Identity(const EamRealFrame& f,void*) {
    if(lastEpoch!=f.epoch){lastEpoch=f.epoch;++epochCount;epochFrames=0;firstInterpolation=true;printf("Frame epoch %llu size %ux%u\n",f.epoch,f.width,f.height);}++epochFrames;
    auto* ctx=static_cast<ID3D11DeviceContext*>(f.context);auto* original=static_cast<ID3D11Texture2D*>(f.original);auto* out=static_cast<ID3D11Texture2D*>(f.output);
    if(realFrames++==0){const auto a=Pixels(ctx,original),b=Pixels(ctx,out);if(a.empty()||a!=b)++failures;else ++checked;}
    if(markers){ComPtr<ID3D11Device> d;ctx->GetDevice(&d);ComPtr<ID3D11UnorderedAccessView> view;if(FAILED(d->CreateUnorderedAccessView(out,nullptr,&view)))return false;
        const unsigned tag=unsigned(f.sequence%200)+20;const float colour[4]={tag/255.f,.25f,.5f,1};ctx->ClearUnorderedAccessViewFloat(view.Get(),colour);
        tags[original]=tag;tags[out]=tag;
    }
    return true;
}
bool NeuralFrame(const EamRealFrame& f,void*) {
    if(lastEpoch!=f.epoch){lastEpoch=f.epoch;++epochCount;epochFrames=0;firstInterpolation=true;neuralImages.clear();printf("NR epoch %llu %ux%u\n",f.epoch,f.width,f.height);}++epochFrames;++realFrames;
    auto* ctx=static_cast<ID3D11DeviceContext*>(f.context);auto* original=static_cast<ID3D11Texture2D*>(f.original);auto* output=static_cast<ID3D11Texture2D*>(f.output);
    const auto before=verifyNeural?Pixels(ctx,original):std::vector<unsigned char>{};
    bool result=host.callback&&host.callback(f,host.user);if(result)++neuralAccepted;
    if(verifyNeural){const auto after=Pixels(ctx,original),enhanced=Pixels(ctx,output);
        if(before.empty()||before!=after||(!result&&enhanced!=before)){++failures;puts("NR original/bypass provenance failure");}else ++checked;
        if(result&&enhanced!=before)++neuralEdited;
        neuralImages[original]=result?enhanced:before;neuralImages[output]=result?enhanced:before;
    }
    return result;
}
bool Pre(uint32_t x,uint32_t y,uint32_t z,void*) {
    auto* ctx=static_cast<ID3D11DeviceContext*>(host.GetDispatchingContext());
    auto* cs=static_cast<ID3D11ComputeShader*>(host.GetCurrentComputeShader());
    eam::ShaderSha256 sha{};const bool known=eam::ReadShaderSha256(cs,sha);const unsigned id=known?shaderIds[sha]:0;
    if(host.neural&&host.callback&&verifyNeural&&(id==255||id==256||id==249||id==245)) {
        for(unsigned i=0;i<(id==256?2u:1u);i++){ComPtr<ID3D11ShaderResourceView> view;ctx->CSGetShaderResources(i,1,&view);if(!view)continue;ComPtr<ID3D11Resource> r;view->GetResource(&r);
            const auto found=neuralImages.find(r.Get());
            if(found!=neuralImages.end()){ComPtr<ID3D11Texture2D> t;r.As(&t);const auto got=Pixels(ctx,t.Get());if(got!=found->second){++failures;printf("NR consumer mismatch shader=%u slot=%u\n",id,i);}else ++checked;}
            else if(id==255||id==256){ComPtr<ID3D11Texture2D> t;r.As(&t);const auto got=Pixels(ctx,t.Get());if(id==256&&i==0&&firstInterpolation&&epochFrames==1&&!got.empty()&&std::all_of(got.begin(),got.end(),[](unsigned char b){return b==0;}))++clearedHistories;else {++failures;printf("NR untracked consumer shader=%u slot=%u\n",id,i);}}
        }
        if(id==256)firstInterpolation=false;
    }
    if(markers&&(id==255||id==256||id==249||id==245)) {
        for(unsigned i=0;i<(id==256?2u:1u);++i){ComPtr<ID3D11ShaderResourceView> v;ctx->CSGetShaderResources(i,1,&v);if(!v)continue;ComPtr<ID3D11Resource> r;v->GetResource(&r);
            auto found=tags.find(r.Get());if(found!=tags.end()){ComPtr<ID3D11Texture2D> t;r.As(&t);auto p=Pixels(ctx,t.Get());if(p.empty()||p[0]!=found->second){++failures;printf("MARKER FAIL shader=%u slot=%u actual=%u expected=%u\n",id,i,p.empty()?999:p[0],found->second);}else ++checked;}
            else if(id==255||id==256){ComPtr<ID3D11Texture2D> t;r.As(&t);auto p=Pixels(ctx,t.Get());
                // LS creates a cleared previous-history surface for its first resized pair.
                // It is not a captured frame. Accept only this exact transition and all-zero image.
                if(id==256&&i==0&&firstInterpolation&&epochFrames==1&&!p.empty()&&std::all_of(p.begin(),p.end(),[](unsigned char c){return c==0;}))++clearedHistories;
                else {++failures;printf("UNMARKED INPUT shader=%u slot=%u resource=%p pixel=%u,%u,%u\n",id,i,r.Get(),p.empty()?999:p[0],p.empty()?999:p[1],p.empty()?999:p[2]);}}
        }
        if(id==256)firstInterpolation=false;
    }
    if(passes++<(host.neural?25u:900u)) {
        printf("pass %u shader %u groups %u %u %u",passes,id,x,y,z);
        for(unsigned i=0;i<8;++i){ComPtr<ID3D11ShaderResourceView> v;ctx->CSGetShaderResources(i,1,&v);if(!v)continue;ComPtr<ID3D11Resource> r;v->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);if(t){D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);printf(" t%u=%p/%ux%u/f%u/m%u",i,t.Get(),d.Width,d.Height,d.Format,d.MiscFlags);if(passes==1)printf("/mips%u/array%u/samples%u/bind%u",d.MipLevels,d.ArraySize,d.SampleDesc.Count,d.BindFlags);}}
        for(unsigned i=0;i<8;++i){ComPtr<ID3D11UnorderedAccessView> v;ctx->CSGetUnorderedAccessViews(i,1,&v);if(!v)continue;ComPtr<ID3D11Resource> r;v->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);if(t){D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);printf(" u%u=%p/%ux%u/f%u/m%u",i,t.Get(),d.Width,d.Height,d.Format,d.MiscFlags);if(passes==1)printf("/mips%u/array%u/samples%u/bind%u",d.MipLevels,d.ArraySize,d.SampleDesc.Count,d.BindFlags);}}
        puts("");
    }
    return false;
}
BOOL CALLBACK Resource(HMODULE module,LPCWSTR type,LPWSTR name,LONG_PTR) {
    if(reinterpret_cast<uintptr_t>(name)>65535)return TRUE;
    HRSRC r=FindResourceW(module,name,type);DWORD n=SizeofResource(module,r);
    const void* bytes=LockResource(LoadResource(module,r));
    if(n>=4&&!memcmp(bytes,"DXBC",4)) { eam::ShaderSha256 sha{};BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)bytes,n,sha.data(),32);shaderIds[sha]=static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)); }
    return TRUE;
}
void __stdcall Status(int value,HWND window,int iw,int ih,int ow,int oh,uint8_t resized,float scale,int error) { printf("LS status %d hwnd=%p in=%dx%d out=%dx%d scale=%g error=%d\n",value,window,iw,ih,ow,oh,scale,error); }
LRESULT CALLBACK Window(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        for(int y=0;y<480;y+=40)for(int x=0;x<640;x+=40){RECT tile{x+(int)(frame%40),y,x+20+(int)(frame%40),y+20};HBRUSH b=CreateSolidBrush(RGB((x*3)%255,(y*5)%255,180));FillRect(dc,&tile,b);DeleteObject(b);}EndPaint(w,&ps);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
using Apply=void(__fastcall*)(int,int,int,int,float,uint8_t,int,uint8_t,int,int,int,float,float,int,uint8_t,uint8_t,uint8_t,uint8_t,int,int,uint8_t,uint8_t,int,int,uint8_t,int,int,int,int,int,int,uint8_t,int);
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2){puts("usage: eam_captureprobe <Lossless_original.dll> [fg=1] [capture=0]");return 2;}
    setvbuf(stdout,nullptr,_IONBF,0);SetProcessDPIAware();CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(!wcscmp(argv[1],L"--source")) {
        WNDCLASSW wc{};wc.lpfnWndProc=Window;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"EamCaptureProbeSource";RegisterClassW(&wc);
        HWND w=CreateWindowW(wc.lpszClassName,L"LS capture pipeline source",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,660,520,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC sd{};sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferCount=2;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=w;sd.SampleDesc.Count=1;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&device,nullptr,&context)))return 1;
        ComPtr<ID3D11Texture2D> back;swap->GetBuffer(0,IID_PPV_ARGS(&back));ComPtr<ID3D11RenderTargetView> rt;device->CreateRenderTargetView(back.Get(),nullptr,&rt);ComPtr<ID3D11DeviceContext1> c1;context.As(&c1);
        const ULONGLONG until=GetTickCount64()+60000;
        while(IsWindow(w)&&GetTickCount64()<until){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}++frame;
            const float black[4]={0.02f,0.02f,0.02f,1};context->ClearRenderTargetView(rt.Get(),black);
            for(int y=0;y<480;y+=40)for(int x=0;x<640;x+=40){D3D11_RECT tile{x+(int)(frame%40),y,x+20+(int)(frame%40),y+20};const float color[4]={(x%255)/255.f,(y%255)/255.f,0.7f,1};c1->ClearView(rt.Get(),color,&tile,1);}swap->Present(0,0);Sleep(16);}return 0;
    }
    HMODULE ls=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!ls){printf("load failed %lu\n",GetLastError());return 1;}
    EnumResourceNamesW(ls,RT_RCDATA,Resource,0);
    D3D11Hook::Initialize(&host);D3D11Hook::InstallDispatchHooks();host.SetPreDispatchCallback(Pre,nullptr);
    using Init=bool(__fastcall*)(decltype(&Status));using Activate=bool(__fastcall*)(HWND);using Stop=void(__fastcall*)();
    auto init=(Init)GetProcAddress(ls,"Init");auto activate=(Activate)GetProcAddress(ls,"Activate");auto stop=(Stop)GetProcAddress(ls,"UnInit");auto apply=(Apply)GetProcAddress(ls,"ApplySettings");
    if(!init||!activate||!stop||!apply)return 1;
    WNDCLASSW wc{};wc.lpfnWndProc=Window;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"EamCaptureProbe";RegisterClassW(&wc);
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);std::wstring command=L"\""+std::wstring(exe)+L"\" --source";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
    if(!CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 1;
    HWND window=nullptr;for(int i=0;i<100&&!window;++i){Sleep(20);window=FindWindowW(L"EamCaptureProbeSource",nullptr);}if(!window)return 1;
    printf("Init %d\n",init(Status));
    const int fg=argc>2?_wtoi(argv[2]):1,capture=argc>3?_wtoi(argv[3]):0;
    const int scaling=argc>4?_wtoi(argv[4]):0;
    const bool hdr=argc>7&&_wtoi(argv[7])!=0;
    markers=argc>5&&_wtoi(argv[5])!=0;if(hdr&&markers)return 2;
    HMODULE addon=nullptr;AddonShutdown_t shutdown=nullptr;
    host.neural=argc>9;verifyNeural=argc<=11||_wtoi(argv[11])!=0;
    if(host.neural) {
        if(markers)return 2;
        addon=LoadLibraryExW(argv[8],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!addon){printf("Addon load failed %lu\n",GetLastError());return 1;}
        auto initialize=reinterpret_cast<AddonInit_t>(GetProcAddress(addon,"AddonInitialize"));shutdown=reinterpret_cast<AddonShutdown_t>(GetProcAddress(addon,"AddonShutdown"));if(!initialize||!shutdown)return 1;
        auto model=std::filesystem::path(argv[9]).u8string();host.SetConfig("DLSSNR02","modelPath",model.c_str());host.SetConfig("DLSSNR02","enabled","1");
        if(argc>10){auto scale=std::to_string(_wtoi(argv[10]));host.SetConfig("DLSSNR02","scalePercent",scale.c_str());}
        if(argc>12){auto intensity=std::to_string(_wtof(argv[12]));host.SetConfig("DLSSNR02","intensity",intensity.c_str());}
        ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;ImGuiMemAllocFunc alloc;ImGuiMemFreeFunc freeFn;void* user;ImGui::GetAllocatorFunctions(&alloc,&freeFn,&user);
        initialize(&host,ImGui::GetCurrentContext(),reinterpret_cast<void*>(alloc),reinterpret_cast<void*>(freeFn),user);
        auto panel=reinterpret_cast<AddonRenderSettings_t>(GetProcAddress(addon,"AddonRenderSettings"));
        if(panel){auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(1280,900);io.DeltaTime=1.f/60;unsigned char* pixels;int fw,fh;io.Fonts->GetTexDataAsRGBA32(&pixels,&fw,&fh);ImGui::NewFrame();ImGui::Begin("NR test panel");panel();ImGui::End();ImGui::EndFrame();}
    } else host.SetRealFrameCallback(Identity);
    HMODULE scalerAddon=nullptr;AddonShutdown_t scalerShutdown=nullptr;std::string scalerId;
    if(argc>13&&host.neural) {
        scalerId=std::filesystem::path(argv[13]).stem().u8string();host.SetConfig(scalerId.c_str(),"scalerPerGame","0");host.SetConfig(scalerId.c_str(),"hotkeys","0");
        scalerAddon=LoadLibraryExW(argv[13],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!scalerAddon){printf("Scaler load failed %lu\n",GetLastError());return 1;}
        auto initialize=reinterpret_cast<AddonInit_t>(GetProcAddress(scalerAddon,"AddonInitialize"));scalerShutdown=reinterpret_cast<AddonShutdown_t>(GetProcAddress(scalerAddon,"AddonShutdown"));if(!initialize||!scalerShutdown)return 1;
        ImGuiMemAllocFunc alloc;ImGuiMemFreeFunc freeFn;void* user;ImGui::GetAllocatorFunctions(&alloc,&freeFn,&user);
        initialize(&host,ImGui::GetCurrentContext(),reinterpret_cast<void*>(alloc),reinterpret_cast<void*>(freeFn),user);
    }
    double nrStoppedAt=-1,scalerStoppedAt=-1;
    RealFrameHook::SetSettings(fg,capture,scaling,hdr);
    apply(0,0,scaling,0,1.0f,0,0,0,fg,0,0,2.0f,120.0f,100,0,0,0,0,0,2,0,hdr,capture,1,0,0,0,0,0,0,0,0,0);
    SetForegroundWindow(window);printf("Activate %d\n",activate(window));
    const ULONGLONG start=GetTickCount64(),until=start+(scalerAddon?24000:8000);bool changed=false;const int lifecycle=argc>6?_wtoi(argv[6]):0;
    while(GetTickCount64()<until){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(lifecycle&&!changed&&GetTickCount64()>start+(scalerAddon?15000:4000)){changed=true;
            if(lifecycle==1)SetWindowPos(window,nullptr,0,0,820,660,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            else if(lifecycle==3&&shutdown){shutdown();shutdown=nullptr;nrStoppedAt=eam::Metrics::Instance().Now();puts("NR stopped while upscaler remains active");}
            else if(lifecycle==4&&scalerShutdown){scalerShutdown();scalerShutdown=nullptr;scalerStoppedAt=eam::Metrics::Instance().Now();puts("Upscaler stopped while NR remains active");}
            else{printf("Toggle stop %d\n",activate(window));
                const auto pause=GetTickCount64()+500;while(GetTickCount64()<pause){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}
                SetForegroundWindow(window);RealFrameHook::SetSettings(fg,capture,scaling,hdr);printf("Toggle start %d\n",activate(window));}
        }Sleep(16);}
    printf("Frame support %u\n",(unsigned)host.GetRealFrameSupport());
    stop();
    if(host.neural){const auto status=eam::Metrics::Instance().GetStatus("DLSSNR02",60);printf("NR status: %s\n",status.text.c_str());for(const auto& series:eam::Metrics::Instance().Snapshot(60))if(series.addon=="DLSSNR02")printf("NR metric %s %.4f %s\n",series.key.c_str(),series.last(),series.unit.c_str());printf("NR accepted %u edited %u\n",neuralAccepted,neuralEdited);}
    if(scalerAddon){auto metric=eam::Metrics::Instance().Get(scalerId,"dlss_ms",60);printf("Upscaler %s samples=%zu last=%.4f status=%s\n",scalerId.c_str(),metric.samples.size(),metric.last(),eam::Metrics::Instance().GetStatus(scalerId,60).text.c_str());if(metric.samples.empty()||(nrStoppedAt>=0&&metric.samples.back().t<nrStoppedAt+1))++failures;}
    if(scalerStoppedAt>=0){auto metric=eam::Metrics::Instance().Get("DLSSNR02","model_ms",60);if(metric.samples.empty()||metric.samples.back().t<scalerStoppedAt+1){++failures;puts("NR did not publish completed work after upscaler shutdown");}}
    if(shutdown)shutdown();if(scalerShutdown)scalerShutdown();if(scalerAddon)FreeLibrary(scalerAddon);if(addon)FreeLibrary(addon);if(host.neural)ImGui::DestroyContext();
    host.SetPreDispatchCallback(nullptr,nullptr);host.SetRealFrameCallback(nullptr);D3D11Hook::Shutdown();PostMessageW(window,WM_CLOSE,0,0);CloseHandle(child.hProcess);CloseHandle(child.hThread);printf("Total passes %u real frames %u verified %u failures %u epochs %u cleared histories %u\n",passes,realFrames,checked,failures,epochCount,clearedHistories);
    for(const auto& entry:eam::Logger::Instance().GetEntries())printf("[%s] %s\n",entry.source.c_str(),entry.message.c_str());
    return passes&&realFrames&&(checked||(host.neural&&!verifyNeural))&&!failures&&((lifecycle!=1&&lifecycle!=2)||epochCount>=2)&&(!host.neural||neuralAccepted>30)?0:1;
}
