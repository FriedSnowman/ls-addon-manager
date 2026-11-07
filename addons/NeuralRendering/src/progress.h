// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <windows.h>
namespace neural {
enum class Stage { Idle, Resources, FlowInit, ModelInit, ModelInitWait, Retiring, FrameSlot, Prepare, Summary, Flow, Model, Compose, Handoff };
inline const char* StageName(Stage s) {
    switch (s) {
    case Stage::Idle: return "waiting for next capture";
    case Stage::Resources: return "creating GPU resources/shared fences";
    case Stage::FlowInit: return "initializing NVIDIA Optical Flow";
    case Stage::ModelInit: return "loading/creating the NR model";
    case Stage::ModelInitWait: return "waiting for model initialization on GPU";
    case Stage::Retiring: return "releasing the previous GPU pipeline";
    case Stage::FrameSlot: return "waiting for a free GPU frame slot";
    case Stage::Prepare: return "preparing the current capture";
    case Stage::Summary: return "waiting for the scene summary on GPU";
    case Stage::Flow: return "NVIDIA Optical Flow";
    case Stage::Model: return "evaluating the NR model";
    case Stage::Compose: return "composing the enhanced image";
    case Stage::Handoff: return "handing the enhanced image to LS";
    }
    return "unknown operation";
}
struct Progress {
    // One atomic keeps stage and start time coherent. No GPU queries or waits.
    std::atomic<uint64_t> value{0};
    void Set(Stage stage) { value.store((GetTickCount64()<<8)|uint64_t(stage), std::memory_order_relaxed); }
};
}
