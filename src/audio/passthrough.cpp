// passthrough.cpp — Phase 1 data-plane: read input ring, copy to output ring.
//
// REALTIME-AUDIO DISCIPLINE (read this before changing this file):
//
//   - NO malloc / new / delete on the hot path.
//   - NO mutexes, condition_variables, or other userspace locks.
//   - NO syscalls except the ONE wakeup primitive (Wakeup::Wait).
//   - NO logging, no iostream, no printf — heartbeat stats are written to a
//     plain byte counter that a separate non-realtime thread reads.
//
// Phase 1 only does a planar memcpy from the input slot to the output
// slot. The plugin chain (Phase 2) will splice in here, between Acquire()
// of the output slot and Publish().

#include "audio/passthrough.h"

#include <cstring>
#include <cstdint>

#include "audio/block_format.h"
#include "ipc/shm_ring.h"
#include "ipc/wakeup.h"

namespace zeus::plughost {

void RunPassthrough(ShmRing&    inputRing,
                    ShmRing&    outputRing,
                    Wakeup&     inputWakeup,
                    PassthroughStats& stats,
                    const volatile bool& stopFlag) {
    while (!stopFlag) {
        BlockHeader* in = inputRing.Read();
        if (in == nullptr) {
            // No work — block on the wakeup. 10 ms timeout so we can poll
            // stopFlag without leaning on a global signal.
            inputWakeup.Wait(10);
            continue;
        }

        BlockHeader* out = outputRing.Acquire();
        if (out == nullptr) {
            // Output ring is full; the consumer (Zeus) is behind. Drop the
            // input block to keep latency bounded — the alternative is
            // unbounded queueing, which is worse in a realtime path.
            inputRing.Release(in);
            stats.dropped++;
            continue;
        }

        // Copy the header — preserve seq, frames, channels, sampleRate,
        // flags. Zero the reserved field on write per BlockHeader contract.
        out->seq        = in->seq;
        out->frames     = in->frames;
        out->channels   = in->channels;
        out->sampleRate = in->sampleRate;
        out->flags      = in->flags;
        std::memset(out->reserved, 0, sizeof(out->reserved));

        // Copy the planar payload as a single memcpy — Phase 1 has no
        // plugin to run, so input == output.
        const std::size_t bytes = static_cast<std::size_t>(in->frames)
                                * static_cast<std::size_t>(in->channels)
                                * sizeof(float);
        std::memcpy(PayloadOf(out), PayloadOf(in), bytes);

        outputRing.Publish(out);
        inputRing.Release(in);
        stats.processed++;
    }
}

}  // namespace zeus::plughost
