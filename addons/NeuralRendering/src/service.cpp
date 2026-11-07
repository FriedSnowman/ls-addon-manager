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
        condition_.notify_all();
    }
    worker_.join();
}
void Service::Configure(const PipelineSettings &settings, bool enabled, bool rebuild) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    if (rebuild || enabled_ != enabled) {
        ++revision_;
        failed_ = false;
    }
    enabled_ = enabled;
    snapshot_.enabled = enabled;
    if (!enabled) {
        desired_.run = false;
        desired_.serial = ++serial_;
        pending_ = true;
        snapshot_.status = "Disabled";
        snapshot_.ready = false;
        snapshot_.failed = false;
    }
    condition_.notify_all();
}
ServiceSnapshot Service::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
bool Service::Process(const EamRealFrame &frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (enabled_ && !stopping_) {
        if (poisoned_ || (failed_ && desired_.revision == revision_)) {
            // Keep this capture pending. Only an explicit retry/configuration change or
            // disable can leave an error; failure never selects the original image.
            const auto revision = revision_;
            condition_.wait(lock, [&] { return stopping_ || !enabled_ || (!poisoned_ && revision_ != revision); });
            continue;
        }
        if (frame.size < sizeof(EamRealFrame) || !frame.context || !frame.original || !frame.output ||
            !frame.width || !frame.height) {
            failed_ = snapshot_.failed = true;
            snapshot_.ready = false;
            desired_.revision = revision_;
            snapshot_.status = "Invalid real capture; processing paused. Disable NR to continue.";
            continue;
        }
        ComPtr<ID3D11Device> device;
        static_cast<ID3D11DeviceContext *>(frame.context)->GetDevice(&device);
        const Encoding encoding = frame.encoding == EamFrameEncoding::ScRgb ? Encoding::ScRgb : Encoding::Sdr709;
        if (!desired_.run || desired_.device != device || desired_.width != frame.width ||
            desired_.height != frame.height || desired_.encoding != encoding || desired_.epoch != frame.epoch ||
            desired_.revision != revision_) {
            desired_ = {device, frame.width, frame.height, encoding, frame.epoch, revision_, ++serial_, true, settings_};
            pending_ = true;
            snapshot_.metrics = {};
            failed_ = snapshot_.failed = false;
            snapshot_.ready = false;
            snapshot_.status = "Preparing NVIDIA pipeline; capture is waiting";
            condition_.notify_all();
        }
        if (!active_ || activeSerial_ != desired_.serial) {
            condition_.wait(lock, [&] {
                return stopping_ || !enabled_ || failed_ || poisoned_ || desired_.revision != revision_ ||
                       (active_ && activeSerial_ == desired_.serial);
            });
            continue;
        }
        auto *pipeline = active_.get();
        const auto composition = settings_.composition;
        const auto picture = settings_.picture;
        processing_ = true;
        lock.unlock();
        std::string error;
        try {
            pipeline->Process(frame, composition, picture);
        } catch (const std::exception &e) {
            error = e.what();
        }
        lock.lock();
        processing_ = false;
        condition_.notify_all();
        snapshot_.metrics = pipeline->Metrics();
        if (error.empty()) {
            if (enabled_) {
                snapshot_.ready = true;
                snapshot_.status = "Running before LSFG; every capture processed";
            }
            return true;
        }
        failed_ = pending_ = snapshot_.failed = true;
        snapshot_.ready = false;
        snapshot_.status = error + "; processing paused. Retry initialization or disable NR.";
    }
    return false; // Only an explicit disable/shutdown permits an original handoff.
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
            condition_.wait(lock, [&] { return !processing_ && (stopping_ || pending_); });
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
            condition_.notify_all();
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
                    snapshot_.ready = true;
                    snapshot_.failed = false;
                } else {
                    failed_ = true;
                    snapshot_.status = error + "; processing paused. Retry initialization or disable NR.";
                    snapshot_.failed = true;
                    snapshot_.ready = false;
                }
            }
            condition_.notify_all();
        }
        if (!keep && !Retire(created)) {
            std::lock_guard<std::mutex> lock(mutex_);
            poisoned_ = true;
            snapshot_.ready = false;
            snapshot_.failed = true;
            snapshot_.status = "GPU retirement failed; restart Lossless Scaling before retrying";
            condition_.notify_all();
        }
    }
}
} // namespace neural
