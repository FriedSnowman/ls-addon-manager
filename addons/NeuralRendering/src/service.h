// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pipeline.h"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace neural {
struct ServiceSnapshot {
    std::string status;
    PipelineMetrics metrics;
    bool ready = false, failed = false, enabled = false;
};
class Service {
  public:
    Service();
    ~Service();
    void Configure(const PipelineSettings &, bool enabled, bool rebuild);
    bool Process(const EamRealFrame &);
    ServiceSnapshot Snapshot();

  private:
    struct Request {
        ComPtr<ID3D11Device> device;
        unsigned width = 0, height = 0;
        Encoding encoding = Encoding::Sdr709;
        uint64_t epoch = 0, revision = 0, serial = 0;
        bool run = false;
        PipelineSettings settings;
    };
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false, pending_ = false, processing_ = false, enabled_ = false, failed_ = false, poisoned_ = false;
    uint64_t revision_ = 0, serial_ = 0, activeSerial_ = 0;
    PipelineSettings settings_;
    Request desired_;
    std::unique_ptr<Pipeline> active_;
    ServiceSnapshot snapshot_;
    void Worker();
    bool Retire(std::unique_ptr<Pipeline> &);
};
} // namespace neural
