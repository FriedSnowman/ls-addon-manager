#include "system_stats.h"
#include <windows.h>
#include <intrin.h>
#include <cstring>

namespace eam {

namespace {
uint64_t Ticks(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
std::string CpuName() {
    int words[4]{};__cpuid(words,static_cast<int>(0x80000000));
    if (static_cast<unsigned>(words[0]) < 0x80000004) return "unknown CPU";
    char name[49]{};
    for(unsigned i=0;i<3;++i) { __cpuid(words,static_cast<int>(0x80000002+i));memcpy(name+i*16,words,16); }
    std::string result=name;const auto start=result.find_first_not_of(' ');
    if(start==std::string::npos)return "unknown CPU";
    return result.substr(start,result.find_last_not_of(' ')-start+1);
}
}

SystemStats& SystemStats::Instance() {
    static SystemStats instance;
    return instance;
}

SystemStats::Snapshot SystemStats::Get() {
    if (m_injected) return m_snap;
    if (m_snap.cpuName.empty()) m_snap.cpuName = CpuName();
    const uint64_t now = GetTickCount64();
    if (m_takenAtMs && now - m_takenAtMs < 1000) return m_snap;
    m_takenAtMs = now;

    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        m_snap.ramTotalMB = mem.ullTotalPhys / (1024 * 1024);
        m_snap.ramUsedMB = (mem.ullTotalPhys - mem.ullAvailPhys) / (1024 * 1024);
        m_snap.ok = true;
    }
    // processor: the share of time not idle since the last sample (kernel time includes idle time)
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t i = Ticks(idle), total = Ticks(kernel) + Ticks(user);
        if (m_total && total > m_total) {
            const double busy = 1.0 - static_cast<double>(i - m_idle) / static_cast<double>(total - m_total);
            m_snap.cpuPercent = busy < 0 ? 0 : busy > 1 ? 100.0 : busy * 100.0;
        }
        m_idle = i; m_total = total;
    }
    return m_snap;
}

} // namespace eam
