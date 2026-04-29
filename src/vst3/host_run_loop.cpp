// host_run_loop.cpp — IRunLoop implementation for the GUI thread.

#include "vst3/host_run_loop.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace zeus::plughost::vst3 {

using Steinberg::FUnknown;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kResultFalse;
using Steinberg::kResultTrue;
using Steinberg::Linux::FileDescriptor;
using Steinberg::Linux::IEventHandler;
using Steinberg::Linux::IRunLoop;
using Steinberg::Linux::ITimerHandler;
using Steinberg::Linux::TimerInterval;

HostRunLoop::HostRunLoop()  = default;
HostRunLoop::~HostRunLoop() = default;

Steinberg::tresult PLUGIN_API HostRunLoop::queryInterface(
    const Steinberg::TUID iid, void** obj) {
    if (obj == nullptr) return kInvalidArgument;
    if (Steinberg::FUnknownPrivate::iidEqual(iid, IRunLoop::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IRunLoop*>(this);
        addRef();
        return kResultTrue;
    }
    *obj = nullptr;
    return kNoInterface;
}

Steinberg::tresult PLUGIN_API HostRunLoop::registerEventHandler(
    IEventHandler* handler, FileDescriptor fd) {
    if (handler == nullptr) return kInvalidArgument;
    if (eventHandlers_.find(fd) != eventHandlers_.end()) {
        return kInvalidArgument;
    }
    eventHandlers_.emplace(fd, handler);
    return kResultTrue;
}

Steinberg::tresult PLUGIN_API HostRunLoop::unregisterEventHandler(
    IEventHandler* handler) {
    if (handler == nullptr) return kInvalidArgument;
    auto it = std::find_if(eventHandlers_.begin(), eventHandlers_.end(),
        [&](const auto& kv) { return kv.second == handler; });
    if (it == eventHandlers_.end()) return kResultFalse;
    eventHandlers_.erase(it);
    return kResultTrue;
}

Steinberg::tresult PLUGIN_API HostRunLoop::registerTimer(
    ITimerHandler* handler, TimerInterval milliseconds) {
    if (handler == nullptr || milliseconds == 0) return kInvalidArgument;
    Timer t;
    t.handler         = handler;
    t.intervalMs      = milliseconds;
    t.nextFireEpochMs = NowEpochMs() + milliseconds;
    timers_.push_back(t);
    return kResultTrue;
}

Steinberg::tresult PLUGIN_API HostRunLoop::unregisterTimer(
    ITimerHandler* handler) {
    if (handler == nullptr) return kInvalidArgument;
    auto it = std::remove_if(timers_.begin(), timers_.end(),
        [&](const Timer& t) { return t.handler == handler; });
    if (it == timers_.end()) return kResultFalse;
    timers_.erase(it, timers_.end());
    return kResultTrue;
}

int HostRunLoop::CollectFds(std::vector<int>& outFds) const {
    int maxFd = -1;
    for (const auto& kv : eventHandlers_) {
        outFds.push_back(kv.first);
        if (kv.first > maxFd) maxFd = kv.first;
    }
    return maxFd;
}

void HostRunLoop::DispatchFds(const std::vector<int>& readyFds) {
    // Take a snapshot of (fd -> handler) pairs to avoid invalidation if
    // a handler unregisters itself during onFDIsSet.
    std::vector<std::pair<FileDescriptor, IEventHandler*>> snapshot;
    snapshot.reserve(readyFds.size());
    for (int fd : readyFds) {
        auto it = eventHandlers_.find(fd);
        if (it != eventHandlers_.end()) {
            snapshot.emplace_back(fd, it->second);
        }
    }
    for (auto& p : snapshot) {
        // The handler may unregister itself during this call.
        if (eventHandlers_.find(p.first) == eventHandlers_.end()) continue;
        if (p.second != nullptr) {
            p.second->onFDIsSet(p.first);
        }
    }
}

std::uint64_t HostRunLoop::HandleTimersAndGetNextFireMs() {
    if (timers_.empty()) return kNoTimers;

    const std::uint64_t now = NowEpochMs();
    // Fire any due timers, refresh their nextFireEpochMs.
    // Snapshot the handler pointers we intend to call so a self-
    // unregistration during onTimer does not invalidate iteration.
    std::vector<ITimerHandler*> toFire;
    toFire.reserve(timers_.size());
    for (auto& t : timers_) {
        if (t.nextFireEpochMs <= now) {
            toFire.push_back(t.handler);
            // Schedule next fire — accumulate so a slow GUI thread
            // doesn't pile up missed fires forever.
            t.nextFireEpochMs = now + t.intervalMs;
        }
    }
    for (auto* h : toFire) {
        // Confirm the timer is still registered.
        auto it = std::find_if(timers_.begin(), timers_.end(),
            [&](const Timer& t) { return t.handler == h; });
        if (it == timers_.end()) continue;
        h->onTimer();
    }
    if (timers_.empty()) return kNoTimers;

    // Find the soonest next-fire and return ms-until.
    std::uint64_t soonest = std::numeric_limits<std::uint64_t>::max();
    for (const auto& t : timers_) {
        if (t.nextFireEpochMs < soonest) soonest = t.nextFireEpochMs;
    }
    const std::uint64_t now2 = NowEpochMs();
    if (soonest <= now2) return 0;
    return soonest - now2;
}

std::uint64_t HostRunLoop::NowEpochMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count());
}

}  // namespace zeus::plughost::vst3
