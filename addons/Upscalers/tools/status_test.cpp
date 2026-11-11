#include "addon/live_status.h"
#include <cstdio>

struct Host : IHost {
    std::atomic<unsigned> reports{0};
    std::mutex mutex;
    std::string status;
    int level=0;
    void Log(EamLogLevel,const char*) override {}
    const char* GetConfig(const char*,const char*,const char* fallback) override { return fallback; }
    void SetConfig(const char*,const char*,const char*) override {}
    void SaveConfig() override {}
    uint32_t GetHostVersion() override { return 0x010400; }
    void SubscribeEvent(uint32_t,EamEventCallback,void*) override {}
    void UnsubscribeEvent(uint32_t,EamEventCallback) override {}
    void PublishEvent(uint32_t,const void*,uint32_t) override {}
    void* GetD3D11Device() override { return nullptr; }
    void* GetD3D11DeviceContext() override { return nullptr; }
    void SetPreDispatchCallback(EamPreDispatchCallback,void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback,void*) override {}
    void* GetCurrentComputeShader() override { return nullptr; }
    uint32_t GetDispatchCount() override { return 0; }
    void SetStatus(const char*,const char* text,int severity) override {
        std::lock_guard<std::mutex> guard(mutex);status=text;level=severity;++reports;
    }
    void PublishMetric(const char*,const char*,double,const char*) override {}
    void* GetDispatchingContext() override { return nullptr; }
    void* CreateImage(const void*,uint32_t,uint32_t,uint32_t) override { return nullptr; }
    void ReleaseImage(void*) override {}
    bool SetRealFrameCallback(EamRealFrameCallback,void*) override { return false; }
    EamFrameSupport GetRealFrameSupport() override { return EamFrameSupport::Waiting; }
    void RequireRealFrameProcessing(bool) override {}
    bool Has(const char* expected,int severity) {
        std::lock_guard<std::mutex> guard(mutex);return status==expected&&level==severity;
    }
};
int main() {
    unsigned failures=0;
    auto check=[&](const char* name,bool ok){printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
    auto until=[](auto predicate){const auto end=GetTickCount64()+2000;while(!predicate()&&GetTickCount64()<end)Sleep(10);return predicate();};
    nr::LiveStatus status;
    status.Set("Waiting for NIS",0);
    check("waiting status exists before any frame",status.Snapshot(100).text=="Waiting for NIS");
    status.Dispatch(100);status.Dispatch(3200);
    auto missing=status.Snapshot(3200);
    check("active dispatches without NIS give an actionable warning",missing.level==2&&missing.text.find("Select NIS")!=std::string::npos);
    status.Nis(3200);status.Set("Preparing first frame",0);
    check("a recognized NIS pass clears the computed warning",status.Snapshot(3200).text=="Preparing first frame");
    status.Set("Running",1);
    check("stopped scaling does not retain a running claim",status.Snapshot(6300).level==0&&status.Snapshot(6300).text.find("No new LS")!=std::string::npos);
    status.Set("Runtime failed",3);
    check("runtime failure survives a lack of frames",status.Snapshot(20000).text=="Runtime failed"&&status.Snapshot(20000).level==3);
    status.Set("Viewport unsupported",2);status.Dispatch(20000);
    check("a specific block is not replaced by generic no-NIS advice",status.Snapshot(20000).text=="Viewport unsupported");
    status.ResetObservations();status.Set("Starting runtime",0);
    check("restart clears observations from the previous capture",status.Snapshot(30000).text=="Starting runtime");
    Host host;
    status.Start(&host,"FSR3UPSC");
    check("startup is published without a Settings panel or capture",until([&]{return host.Has("Starting runtime",0);}));
    const auto initial=host.reports.load();
    check("status refreshes while no frames arrive",until([&]{return host.reports.load()>=initial+2;}));
    status.Set("Runtime failed",3);
    check("first-frame failure is published by the independent reporter",until([&]{return host.Has("Runtime failed",3);}));
    status.Stop();const auto stopped=host.reports.load();Sleep(550);
    check("shutdown leaves no publisher using the host",host.reports.load()==stopped);
    status.Set("Retrying",0);status.Start(&host,"FSR3UPSC");
    check("reporting restarts after a disable and re-enable",until([&]{return host.Has("Retrying",0);}));
    status.Stop();
    printf("%u failures\n",failures);return failures?1:0;
}
