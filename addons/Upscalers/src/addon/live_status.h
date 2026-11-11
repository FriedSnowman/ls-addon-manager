#pragma once
#include <eam/ihost.h>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

namespace nr {
// Publishes cached text only. The reporter never touches the GPU, frame lock,
// engine or ImGui, so initialization and a blocked first frame remain visible.
class LiveStatus {
public:
    struct Message { std::string text; int level = 0; };
    ~LiveStatus() { Stop(); }
    void Set(const std::string& text, int level) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_message = {text, level};
    }
    void ResetObservations() { m_firstDispatch = 0; m_lastDispatch = 0; m_lastNis = 0; }
    void Dispatch(uint64_t now) {
        uint64_t empty = 0;
        m_firstDispatch.compare_exchange_strong(empty, now);
        m_lastDispatch = now;
    }
    void Nis(uint64_t now) { m_lastNis = now; }
    Message Snapshot(uint64_t now) {
        Message message;
        { std::lock_guard<std::mutex> guard(m_mutex); message = m_message; }
        if (message.level >= 2) return message;
        const auto first = m_firstDispatch.load(), last = m_lastDispatch.load(), nis = m_lastNis.load();
        if (last && now > last && now - last >= 3000)
            return {"No new LS compute passes for 3 s; scaling may be stopped, paused or the source unavailable", 0};
        const auto since = nis ? nis : first;
        if (since && now > since && now - since >= 3000)
            return {"LS compute passes are active, but no supported NIS pass was found. Select NIS in Lossless Scaling; if already selected, restart LS and check Logs", 2};
        return message;
    }
    void Start(IHost* host, const char* addon) {
        Stop();
        { std::lock_guard<std::mutex> guard(m_mutex); m_stopping = false; }
        m_thread = std::thread([this, host, id = std::string(addon)] {
            for (;;) {
                const auto message = Snapshot(GetTickCount64());
                host->SetStatus(id.c_str(), message.text.c_str(), message.level);
                std::unique_lock<std::mutex> guard(m_mutex);
                if (m_wake.wait_for(guard, std::chrono::milliseconds(500), [this] { return m_stopping; })) break;
            }
        });
    }
    void Stop() {
        { std::lock_guard<std::mutex> guard(m_mutex); m_stopping = true; }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    Message m_message;
    bool m_stopping = true;
    std::atomic<uint64_t> m_firstDispatch{0}, m_lastDispatch{0}, m_lastNis{0};
};
}
