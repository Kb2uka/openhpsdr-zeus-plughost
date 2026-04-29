// passthrough.h — Phase 1 data-plane entry point.

#pragma once

#include <atomic>
#include <cstdint>

namespace zeus::plughost {

class ShmRing;
class Wakeup;

// Heartbeat counters updated from the audio thread. Plain atomics so a
// non-realtime logging thread can sample without locks.
struct PassthroughStats {
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> dropped{0};
};

// Phase 1 audio loop: read input, memcpy into output, repeat. Returns when
// `stopFlag` becomes true. Caller owns rings and wakeup.
void RunPassthrough(ShmRing&          inputRing,
                    ShmRing&          outputRing,
                    Wakeup&           inputWakeup,
                    PassthroughStats& stats,
                    const volatile bool& stopFlag);

}  // namespace zeus::plughost
