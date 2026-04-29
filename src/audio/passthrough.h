// passthrough.h — Phase 2 data-plane entry point.

#pragma once

#include <atomic>
#include <cstdint>

namespace zeus::plughost {

class ShmRing;
class Wakeup;

namespace vst3 { class PluginHost; }

// Heartbeat counters updated from the audio thread. Plain atomics so a
// non-realtime logging thread can sample without locks.
struct PassthroughStats {
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> dropped{0};
};

// Phase 2 audio loop: wait for an input block on `inputWakeup`, run the
// plugin if one is loaded (otherwise straight memcpy), publish the
// output ring, post `outputWakeup` so the host wakes. Returns when
// either `stopFlag` (signal-driven) or `controlStopFlag` (Goodbye / EOF)
// becomes true. `pluginHost` may be nullptr — same as IsLoaded() returning
// false; the loop falls back to memcpy.
void RunPassthrough(ShmRing&                    inputRing,
                    ShmRing&                    outputRing,
                    Wakeup&                     inputWakeup,
                    Wakeup&                     outputWakeup,
                    PassthroughStats&           stats,
                    const volatile bool&        stopFlag,
                    const std::atomic<bool>&    controlStopFlag,
                    vst3::PluginHost*           pluginHost);

}  // namespace zeus::plughost
