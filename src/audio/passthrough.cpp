// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// passthrough.cpp — Phase 2 data-plane: read input ring, run the loaded
// VST3 plugin (if any), copy to output ring, post the output wakeup.
//
// REALTIME-AUDIO DISCIPLINE (read this before changing this file):
//
//   - NO malloc / new / delete on the hot path.
//   - NO mutexes, condition_variables, or other userspace locks.
//   - NO syscalls except the wakeup primitives (Wait + Signal).
//   - NO logging, no iostream, no printf — heartbeat stats are written to a
//     plain byte counter that a separate non-realtime thread reads.
//
// Phase 2 plugin dispatch:
//
//   When `pluginHost->IsLoaded()` returns true, the loop hands the input
//   payload + output slot to PluginHost::Process(). The plugin owns the
//   write to `out`. If Process() returns false (no plugin loaded, or a
//   transient swap window), we fall through to the memcpy path so the
//   block survives intact.

#include "audio/passthrough.h"

#include <cstring>
#include <cstdint>

#include "audio/block_format.h"
#include "ipc/shm_ring.h"
#include "ipc/wakeup.h"
#include "vst3/plugin_chain.h"

namespace zeus::plughost {

void RunPassthrough(ShmRing&                 inputRing,
                    ShmRing&                 outputRing,
                    Wakeup&                  inputWakeup,
                    Wakeup&                  outputWakeup,
                    PassthroughStats&        stats,
                    const volatile bool&     stopFlag,
                    const std::atomic<bool>& controlStopFlag,
                    vst3::PluginChain*       pluginChain) {
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

            const float* inPayload  = PayloadOf(in);
            float*       outPayload = PayloadOf(out);

            // Plugin dispatch — single mono channel only in Phase 2/3a.
            // PluginChain::Process is realtime-safe and itself owns the
            // master-disabled / all-empty fast path (single memcpy). It
            // runs allocation-free / lock-free / syscall-free.
            if (pluginChain != nullptr && in->channels == 1) {
                pluginChain->Process(
                    inPayload, outPayload,
                    static_cast<std::int32_t>(in->frames));
            } else {
                std::memcpy(outPayload, inPayload, bytes);
            }

            outputRing.Publish(out);
            inputRing.Release(in);
            stats.processed.fetch_add(1, std::memory_order_relaxed);
            outputWakeup.Signal();
        }
    }
}

}  // namespace zeus::plughost