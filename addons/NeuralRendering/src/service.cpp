// SPDX-License-Identifier: GPL-3.0-only
#include "service.h"
namespace neural {
namespace {
void ModuleAnchor() {}
} // namespace
Service::Service() {
    worker_ = std::thread(&Service::Worker, this);
}
Service::~Service() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        condition_.notify_one();
    }
    worker_.join();
}
void Service::Configure(const PipelineSettings &settings, bool enabled, bool rebuild) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    if (rebuild || enabled_ != enabled) {
        ++revision_;
        failed_ = false;
        dropped_ = true;
    }
    enabled_ = enabled;
    snapshot_.enabled = enabled;
    if (!enabled) {
        desired_.run = false;
        desired_.serial = ++serial_;
        pending_ = true;
        snapshot_.status = "Disabled";
        snapshot_.ready = false;
        condition_.notify_one();
    }
}
ServiceSnapshot Service::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
bool Service::Process(const EamRealFrame &frame) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        dropped_ = true;
        return false;
    }
    if (!enabled_ || poisoned_)
        return false;
    if (frame.size < sizeof(EamRealFrame) || !frame.context || !frame.original || !frame.output || !frame.width ||
        !frame.height)
        return false;
    ComPtr<ID3D11Device> device;
    static_cast<ID3D11DeviceContext *>(frame.context)->GetDevice(&device);
    const Encoding encoding = frame.encoding == EamFrameEncoding::ScRgb ? Encoding::ScRgb : Encoding::Sdr709;
    if (!desired_.run || desired_.device != device || desired_.width != frame.width ||
        desired_.height != frame.height || desired_.encoding != encoding || desired_.epoch != frame.epoch ||
        desired_.revision != revision_) {
        desired_ = {device, frame.width, frame.height, encoding, frame.epoch, revision_, ++serial_, true, settings_};
        pending_ = true;
        waitingBypasses_ = 0;
        snapshot_.metrics = {};
        failed_ = false;
        snapshot_.ready = false;
        snapshot_.failed = false;
        snapshot_.status = "Preparing NVIDIA pipeline; original frames pass through";
        condition_.notify_one();
    }
    if (!active_ || activeSerial_ != desired_.serial || failed_) {
        snapshot_.metrics.bypassed = ++waitingBypasses_;
        return false;
    }
    try {
        if (dropped_.exchange(false))
            active_->Invalidate();
        const bool result = active_->Process(frame, settings_.composition);
        snapshot_.metrics = active_->Metrics();
        snapshot_.metrics.bypassed += waitingBypasses_;
        snapshot_.ready = true;
        snapshot_.status = result ? "Running before LSFG" : "Frame bypassed; resetting history";
        return result;
    } catch (const std::exception &e) {
        failed_ = true;
        pending_ = true;
        snapshot_.ready = false;
        snapshot_.failed = true;
        snapshot_.status = e.what();
        condition_.notify_one();
        return false;
    }
}
bool Service::Retire(std::unique_ptr<Pipeline> &p) {
    if (!p)
        return true;
    if (p->Retire()) {
        p.reset();
        return true;
    }
    // Preserve the complete object graph, queues, allocators, fences and registrations.
    // Pin this module as well; a driver callback must not target unloaded code.
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module);
    p.release();
    return false;
}
void Service::Worker() {
    for (;;) {
        Request request;
        std::unique_ptr<Pipeline> retired;
        bool stop = false, build = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || pending_; });
            stop = stopping_;
            pending_ = false;
            request = desired_;
            build = !stop && request.run && !failed_ && !poisoned_;
            retired = std::move(active_);
        }
        if (!Retire(retired)) {
            std::lock_guard<std::mutex> lock(mutex_);
            poisoned_ = true;
            build = false;
            snapshot_.ready = false;
            snapshot_.failed = true;
            snapshot_.status = "GPU retirement failed; restart Lossless Scaling before retrying";
        }
        if (stop)
            return;
        if (!build)
            continue;
        auto created = std::make_unique<Pipeline>();
        std::string error;
        try {
            created->Initialize(request.device.Get(), request.width, request.height, request.encoding,
                                request.settings);
        } catch (const std::exception &e) {
            error = e.what();
        }
        bool keep = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_ && desired_.serial == request.serial) {
                if (error.empty()) {
                    active_ = std::move(created);
                    activeSerial_ = request.serial;
                    keep = true;
                    snapshot_.status = "Ready for real capture";
                    snapshot_.metrics = active_->Metrics();
                    snapshot_.metrics.bypassed = waitingBypasses_;
                    snapshot_.ready = true;
                    snapshot_.failed = false;
                    dropped_ = true;
                } else {
                    failed_ = true;
                    snapshot_.status = error;
                    snapshot_.failed = true;
                    snapshot_.ready = false;
                }
            }
        }
        if (!keep && !Retire(created)) {
            std::lock_guard<std::mutex> lock(mutex_);
            poisoned_ = true;
            snapshot_.ready = false;
            snapshot_.failed = true;
            snapshot_.status = "GPU retirement failed; restart Lossless Scaling before retrying";
        }
    }
}
} // namespace neural
