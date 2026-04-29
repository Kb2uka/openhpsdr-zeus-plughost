// passthrough.cpp — Phase 2 data-plane: read input ring, copy to output
// ring, post the output wakeup.
//
// REALTIME-AUDIO DISCIPLINE (read this before changing this file):
//
//   - NO malloc / new / delete on the hot path.
//   - NO mutexes, condition_variables, or other userspace locks.
//   - NO syscalls except the wakeup primitives (Wait + Signal).
//   - NO logging, no iostream, no printf — heartbeat stats are written to a
//     plain byte counter that a separate non-realtime thread reads.
//
// Phase 2 only does a planar memcpy from the input slot to the output
// slot. The plugin chain (Phase 3) will splice in here, between Acquire()
// of the output slot and Publish().

#include "audio/passthrough.h"

#include <cstring>
#include <cstdint>

#include "audio/block_format.h"
#include "ipc/shm_ring.h"
#include "ipc/wakeup.h"

namespace zeus::plughost {

void RunPassthrough(ShmRing&                 inputRing,
                    ShmRing&                 outputRing,
                    Wakeup&                  inputWakeup,
                    Wakeup&                  outputWakeup,
                    PassthroughStats&        stats,
                    const volatile bool&     stopFlag,
                    const std::atomic<bool>& controlStopFlag) {
    while (!stopFlag && !controlStopFlag.load(std::memory_order_relaxed)) {
        // Block on the input wakeup with a short timeout so we can poll
        // the stop flags. The host posts this semaphore once per block.
        if (!inputWakeup.Wait(50)) {
            // Timeout (or interrupt). Loop back and check stop flags.
            continue;
        }

        // The host may have posted multiple times; drain the input ring
        // until empty before sleeping again. Bounded by slotCount so we
        // can never spin indefinitely.
        for (;;) {
            BlockHeader* in = inputRing.Read();
            if (in == nullptr) break;

            BlockHeader* out = outputRing.Acquire();
            if (out == nullptr) {
                // Output ring full; drop input to keep latency bounded.
                inputRing.Release(in);
                stats.dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Preserve seq, frames, channels, sampleRate, flags. Zero
            // the reserved field on write per BlockHeader contract.
            out->seq        = in->seq;
            out->frames     = in->frames;
            out->channels   = in->channels;
            out->sampleRate = in->sampleRate;
            out->flags      = in->flags;
            std::memset(out->reserved, 0, sizeof(out->reserved));

            const std::size_t bytes = static_cast<std::size_t>(in->frames)
                                    * static_cast<std::size_t>(in->channels)
                                    * sizeof(float);
            std::memcpy(PayloadOf(out), PayloadOf(in), bytes);

            outputRing.Publish(out);
            inputRing.Release(in);
            stats.processed.fetch_add(1, std::memory_order_relaxed);
            outputWakeup.Signal();
        }
    }
}

}  // namespace zeus::plughost
