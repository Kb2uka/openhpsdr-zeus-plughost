// host_run_loop.h — Steinberg::Linux::IRunLoop implementation for the
// dedicated GUI thread.
//
// VST3 plugins on Linux need the host to provide an IRunLoop so they can
// register file-descriptor handlers (e.g. socket-driven UIs) and timers.
// The plugin obtains the IRunLoop by calling
//   view->getFrame()->queryInterface(Linux::IRunLoop::iid, ...)
// against the IPlugFrame we hand to view->setFrame().
//
// One HostRunLoop instance lives for the lifetime of the GUI thread;
// HostPlugFrame holds a non-owning pointer to it. All registration /
// dispatch happens on the GUI thread — the public surface is NOT
// thread-safe by design (the SDK contract is that plugins call these
// methods from their own thread, which on Linux means our GUI thread).
//
// Threading discipline:
//   - The plugin calls registerEventHandler / registerTimer on the GUI
//     thread (it received the IRunLoop pointer there during attach()).
//   - The GUI thread's main loop polls all registered fds + tracks
//     timer deadlines, dispatching callbacks back to the plugin's
//     handlers — also on the GUI thread.
//   - Audio thread NEVER touches HostRunLoop.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"

namespace zeus::plughost::vst3 {

// IRunLoop is implemented as a non-destroying singleton-style object;
// the ownership lives with the GuiThread. addRef/release are no-ops so
// the plugin can call them safely without affecting our lifetime.
class HostRunLoop : public Steinberg::Linux::IRunLoop {
public:
    HostRunLoop();
    ~HostRunLoop();

    HostRunLoop(const HostRunLoop&)            = delete;
    HostRunLoop& operator=(const HostRunLoop&) = delete;

    // -- FUnknown --------------------------------------------------------
    Steinberg::tresult PLUGIN_API queryInterface(
        const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override  { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

    // -- IRunLoop --------------------------------------------------------
    Steinberg::tresult PLUGIN_API registerEventHandler(
        Steinberg::Linux::IEventHandler* handler,
        Steinberg::Linux::FileDescriptor fd) override;
    Steinberg::tresult PLUGIN_API unregisterEventHandler(
        Steinberg::Linux::IEventHandler* handler) override;
    Steinberg::tresult PLUGIN_API registerTimer(
        Steinberg::Linux::ITimerHandler* handler,
        Steinberg::Linux::TimerInterval milliseconds) override;
    Steinberg::tresult PLUGIN_API unregisterTimer(
        Steinberg::Linux::ITimerHandler* handler) override;

    // -- GUI thread driver ----------------------------------------------

    // Snapshot the registered fds for select(). Caller fills a fd_set;
    // returns the highest fd registered (or -1 if none).
    int CollectFds(std::vector<int>& outFds) const;

    // Dispatch onFDIsSet for any fd in `readyFds`.
    void DispatchFds(const std::vector<int>& readyFds);

    // Returns the number of milliseconds until the next timer fires, or
    // a sentinel "no timers" value (~kNoTimers) if none. Fires any
    // timers due now and updates their next-fire times.
    static constexpr std::uint64_t kNoTimers =
        static_cast<std::uint64_t>(-1);
    std::uint64_t HandleTimersAndGetNextFireMs();

private:
    // No threading guard inside — registration happens from the GUI
    // thread (the plugin was told "this is your run loop" on the GUI
    // thread). select / dispatch also runs on the GUI thread.
    using EventHandlers = std::unordered_map<
        Steinberg::Linux::FileDescriptor,
        Steinberg::Linux::IEventHandler*>;

    struct Timer {
        Steinberg::Linux::ITimerHandler*    handler;
        Steinberg::Linux::TimerInterval     intervalMs;
        std::uint64_t                       nextFireEpochMs;
    };

    EventHandlers      eventHandlers_;
    std::vector<Timer> timers_;

    static std::uint64_t NowEpochMs();
};

}  // namespace zeus::plughost::vst3
