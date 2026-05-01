// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// shm_ring.h — SPSC lock-free shared-memory ring of fixed-size audio blocks.
//
// One producer thread, one consumer thread. Slot count must be a power of
// two so the modulo reduces to a bitwise AND. Each slot is sized to hold
// one BlockHeader plus its planar payload.
//
// Phase 2: backing is real POSIX shared memory (shm_open + ftruncate +
// mmap). The host CreateNew()s the named region; the sidecar
// OpenExisting()s the same name. Wire layout is identical to Phase 1's
// heap-backed mapping so the existing wire-format guards still apply.
//
// Realtime contract: Acquire/Publish/Read/Release perform NO syscalls,
// NO malloc, NO locks. Only atomic loads / stores on the head and tail
// indices.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/block_format.h"

namespace zeus::plughost {

// Header that lives at the start of the shared mapping. Producer writes
// `head`; consumer writes `tail`. Both monotonic; modular indexing is done
// at the read sites.
//
// Layout MUST be exactly 160 bytes — it is shared with the C# side
// (Zeus.PluginHost.Ipc.RingControlBlock has [StructLayout(Size = 160)])
// and the bytes-per-slot offsets at both ends would diverge if the C++
// side padded to 192 via alignas(64). The mapping is mmap'd at a page
// boundary so head/tail naturally land on a 64-byte boundary; cache-line
// padding between head and tail is provided explicitly via pad0/pad1.
struct RingControlBlock {
    std::atomic<std::uint64_t> head;       // next slot to be written
    std::uint8_t pad0[64 - sizeof(std::atomic<std::uint64_t>)];

    std::atomic<std::uint64_t> tail;       // next slot to be read
    std::uint8_t pad1[64 - sizeof(std::atomic<std::uint64_t>)];

    std::uint32_t slotCount;               // power-of-two
    std::uint32_t slotBytes;               // BlockBytes(frames, channels)
    std::uint32_t frames;
    std::uint32_t channels;
    std::uint32_t sampleRate;
    std::uint32_t reserved[3];
};

static_assert(sizeof(RingControlBlock) == 160,
              "RingControlBlock must be exactly 160 bytes — wire-shared with C#");

// SPSC ring view. The actual storage is owned by ShmRingMapping (below); a
// ShmRing is a non-owning façade that the audio thread interacts with.
class ShmRing {
public:
    ShmRing(RingControlBlock* control, std::uint8_t* slots);

    // Producer side.
    // Acquire a writeable slot, or nullptr if the ring is full. Caller
    // fills the BlockHeader and payload, then calls Publish().
    BlockHeader* Acquire();
    void Publish(BlockHeader* header);

    // Consumer side.
    // Peek the next readable slot, or nullptr if empty. Caller consumes,
    // then calls Release() to advance the tail.
    BlockHeader* Read();
    void Release(BlockHeader* header);

    std::uint32_t SlotCount() const { return control_->slotCount; }
    std::uint32_t SlotBytes() const { return control_->slotBytes; }

private:
    RingControlBlock* control_;
    std::uint8_t*     slots_;

    BlockHeader* SlotAt(std::uint64_t index);
};

// Owning RAII wrapper around a POSIX shared-memory mapping. The host calls
// CreateNew() to create + size the region; the sidecar calls OpenExisting()
// to attach to a region the host already created.
class ShmRingMapping {
public:
    // Create a new POSIX shm region named `name`, size it to hold the
    // given ring geometry, and zero-initialize the control block.
    // `name` MUST start with '/' and contain no further '/'.
    static ShmRingMapping CreateNew(const std::string& name,
                                    std::uint32_t frames,
                                    std::uint32_t channels,
                                    std::uint32_t sampleRate,
                                    std::uint32_t slotCount);

    // Open an already-created POSIX shm region named `name`. Reads the
    // existing ring geometry from the control block; the geometry args
    // are validated against what the creator already wrote.
    static ShmRingMapping OpenExisting(const std::string& name,
                                       std::uint32_t expectedFrames,
                                       std::uint32_t expectedChannels,
                                       std::uint32_t expectedSampleRate,
                                       std::uint32_t expectedSlotCount);

    ~ShmRingMapping();

    ShmRingMapping(const ShmRingMapping&)            = delete;
    ShmRingMapping& operator=(const ShmRingMapping&) = delete;

    // Move-only so callers can return-by-value from CreateNew/OpenExisting.
    ShmRingMapping(ShmRingMapping&& other) noexcept;
    ShmRingMapping& operator=(ShmRingMapping&& other) noexcept;

    ShmRing& Ring() { return ring_; }
    const std::string& Name() const { return name_; }

    // True if this mapping owns the name (i.e. created it). Sidecar
    // mappings opened via OpenExisting return false; they MUST NOT
    // shm_unlink.
    bool OwnsName() const { return ownsName_; }

private:
    ShmRingMapping();  // empty/moved-from sentinel

    std::string         name_;
    std::size_t         mappingBytes_;
    void*               mapping_;     // owns RingControlBlock + slots
    RingControlBlock*   control_;
    std::uint8_t*       slots_;
    bool                ownsName_;
    ShmRing             ring_;
};

}  // namespace zeus::plughost