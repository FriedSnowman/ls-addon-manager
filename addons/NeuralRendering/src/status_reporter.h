// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace neural {
// Independent of capture and ImGui: a paused first frame must not hide its error.
// Stop before destroying the service/host or unloading the addon.
class StatusReporter {
  public:
    ~StatusReporter() { Stop(); }
    void Start(std::function<void()> report) {
        Stop();
        stopping_ = false;
        thread_ = std::thread([this, report] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_) {
                lock.unlock(); report(); lock.lock();
                condition_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_; });
            }
        });
    }
    void Stop() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    bool stopping_ = false;
};
}
