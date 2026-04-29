// shm_ring.h — SPSC lock-free shared-memory ring of fixed-size audio blocks.
//
// One producer thread, one consumer thread. Slot count must be a power of
// two so the modulo reduces to a bitwise AND. Each slot is sized to hold
// one BlockHeader plus its planar payload.
//
// This skeleton declares the cross-platform interface; the Phase 1 impl in
// shm_ring.cpp uses POSIX shm_open + mmap on Linux/macOS and falls back to
// an anonymous heap allocation on Windows. A real Windows impl will use
// CreateFileMapping in Phase 2 (TODO).
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
struct alignas(64) RingControlBlock {
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

static_assert(sizeof(RingControlBlock) % 64 == 0,
              "RingControlBlock must be a multiple of one cache line");

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

// Owning RAII wrapper around the shared memory mapping. Phase 1 uses an
// anonymous heap allocation as a placeholder; Phase 2 wires this up to
// shm_open / CreateFileMapping keyed by the --shm-name argv flag.
class ShmRingMapping {
public:
    ShmRingMapping(const std::string& name,
                   std::uint32_t frames,
                   std::uint32_t channels,
                   std::uint32_t sampleRate,
                   std::uint32_t slotCount);
    ~ShmRingMapping();

    ShmRingMapping(const ShmRingMapping&) = delete;
    ShmRingMapping& operator=(const ShmRingMapping&) = delete;

    ShmRing& Ring() { return ring_; }
    const std::string& Name() const { return name_; }

private:
    std::string         name_;
    std::size_t         mappingBytes_;
    void*               mapping_;     // owns RingControlBlock + slots
    RingControlBlock*   control_;
    std::uint8_t*       slots_;
    ShmRing             ring_;
};

}  // namespace zeus::plughost
