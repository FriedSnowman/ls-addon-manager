// SPDX-License-Identifier: GPL-3.0-only
// Exercises the actual addon with no GPU/model, Settings UI, or capture progress.
#include <eam/addon_sdk.h>
#include <eam/real_frame_status.h>
#include "diagnostic_text.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
struct Host : IHost {
    std::atomic<EamFrameSupport> support{EamFrameSupport::UnsupportedCapture};
    std::mutex statusMutex, callbackMutex;
    std::string status;
    int level=0;
    std::atomic<unsigned> reports{0};
    EamRealFrameCallback callback=nullptr;void* user=nullptr;
    uint32_t api=0x010400;bool conflict=false;
    void Log(EamLogLevel,const char*) override {}
    const char* GetConfig(const char*,const char*,const char* d) override{return d;}
    void SetConfig(const char*,const char*,const char*) override {}
    void SaveConfig() override {}
    uint32_t GetHostVersion() override{return api;}
    void SubscribeEvent(uint32_t,EamEventCallback,void*) override {}
    void UnsubscribeEvent(uint32_t,EamEventCallback) override {}
    void PublishEvent(uint32_t,const void*,uint32_t) override {}
    void* GetD3D11Device() override{return nullptr;}
    void* GetD3D11DeviceContext() override{return nullptr;}
    void SetPreDispatchCallback(EamPreDispatchCallback,void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback,void*) override {}
    void* GetCurrentComputeShader() override{return nullptr;}
    uint32_t GetDispatchCount() override{return 0;}
    void SetStatus(const char*,const char* s,int l) override{std::lock_guard<std::mutex> lock(statusMutex);status=s;level=l;++reports;}
    void PublishMetric(const char*,const char*,double,const char*) override {}
    void* GetDispatchingContext() override{return nullptr;}
    void* CreateImage(const void*,uint32_t,uint32_t,uint32_t) override{return nullptr;}
    void ReleaseImage(void*) override {}
    bool SetRealFrameCallback(EamRealFrameCallback f,void* u) override{std::lock_guard<std::mutex> lock(callbackMutex);if(conflict&&f)return false;callback=f;user=u;return true;}
    EamFrameSupport GetRealFrameSupport() override{return support;}
    void RequireRealFrameProcessing(bool) override {}
    bool Has(const char* text,int severity) {std::lock_guard<std::mutex> lock(statusMutex);return status.find(text)!=std::string::npos&&level==severity;}
};
int main() {
    unsigned failures=0;
    auto check=[&](const char* name,bool pass){printf("%s %s\n",pass?"PASS":"FAIL",name);if(!pass)++failures;};
    auto until=[](auto predicate){const auto end=GetTickCount64()+2500;while(!predicate()&&GetTickCount64()<end)Sleep(10);return predicate();};
    for(int fg: {0,1})for(int scaler: {0,3})check("supported profile",EamProfileSupport(true,true,fg,1,scaler)==EamFrameSupport::Ready);
    check("DXGI rejected before a frame",EamProfileSupport(true,true,1,0,0)==EamFrameSupport::UnsupportedCapture);
    check("other capture rejected",EamProfileSupport(true,true,1,9,0)==EamFrameSupport::UnsupportedCapture);
    check("FG 2 rejected",EamProfileSupport(true,true,2,1,0)==EamFrameSupport::UnsupportedFrameGeneration);
    check("FG off scaler rejected",EamProfileSupport(true,true,0,1,2)==EamFrameSupport::UnsupportedScaler);
    check("FG on allows later scaler",EamProfileSupport(true,true,1,1,2)==EamFrameSupport::Ready);
    check("runtime takes precedence",EamProfileSupport(false,true,1,0,0)==EamFrameSupport::UnsupportedRuntime);
    check("settings transition explicit",EamProfileSupport(true,false,1,1,0)==EamFrameSupport::SettingsChanging);
    for(unsigned i=0;i<=unsigned(EamFrameSupport::CallbackFailed);++i)if(i!=unsigned(EamFrameSupport::Ready))check("support state has guidance",EamSupportText(EamFrameSupport(i))!=nullptr);
    check("missing model remedy",neural::FailureGuidance("User-provided nvngx_dlssnr.dll was not found").find("Browse")!=std::string::npos);
    check("flow uses capture size",neural::FailureGuidance("Optical flow resolution is outside driver capabilities").find("Working resolution does not")!=std::string::npos);
    check("allocation remedy",neural::FailureGuidance("Create image failed (0x8007000E)").find("Multipass off")!=std::string::npos);
    check("device removed remedy",neural::FailureGuidance("Create image failed (0x887A0005)").find("Restart")!=std::string::npos);
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    HMODULE dll=LoadLibraryW((std::filesystem::path(exe).parent_path()/L"DLSSNR02.dll").c_str());
    if(!dll){puts("FAIL load addon");return 1;}
    const auto init=reinterpret_cast<AddonInit_t>(GetProcAddress(dll,"AddonInitialize"));
    const auto shutdown=reinterpret_cast<AddonShutdown_t>(GetProcAddress(dll,"AddonShutdown"));
    if(!init||!shutdown){puts("FAIL addon exports");return 1;}
    auto* context=ImGui::CreateContext();ImGuiMemAllocFunc alloc;ImGuiMemFreeFunc free;void* data;
    ImGui::GetAllocatorFunctions(&alloc,&free,&data);
    Host host;
    init(&host,context,reinterpret_cast<void*>(alloc),reinterpret_cast<void*>(free),data);
    check("DXGI error updates without Settings or a frame",until([&]{return host.Has("select WGC",2);}));
    const auto first=host.reports.load();
    check("blocked status keeps refreshing",until([&]{return host.reports.load()>first;}));
    host.support=EamFrameSupport::UnsupportedRuntime;
    check("runtime mismatch names supported version",until([&]{return host.Has("supported version 3.2.2.0",2);}));
    host.support=EamFrameSupport::Ready;
    auto pending=std::async(std::launch::async,[&]{std::lock_guard<std::mutex> lock(host.callbackMutex);return host.callback(EamRealFrame{},host.user);});
    check("first-frame failure visible while callback is blocked",until([&]{return host.Has("Invalid real capture",3);}));
    check("failure does not bypass required frame",pending.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout);
    shutdown();
    check("shutdown releases blocked callback",!pending.get());
    const auto stopped=host.reports.load();Sleep(550);
    check("no reporter survives shutdown",host.reports.load()==stopped);
    host.api=0x010300;
    init(&host,context,reinterpret_cast<void*>(alloc),reinterpret_cast<void*>(free),data);
    check("API mismatch remains visible",until([&]{return host.Has("Manager API 1.4",3);}));shutdown();
    host.api=0x010400;host.conflict=true;
    init(&host,context,reinterpret_cast<void*>(alloc),reinterpret_cast<void*>(free),data);
    check("callback ownership conflict remains visible",until([&]{return host.Has("Another addon owns",3);}));shutdown();
    FreeLibrary(dll);ImGui::DestroyContext(context);
    printf("%u failures\n",failures);return failures?1:0;
}
