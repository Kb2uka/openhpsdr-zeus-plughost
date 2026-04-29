// shm_ring.cpp — SPSC ring impl.
//
// Phase 1 uses an anonymous heap allocation as the backing store. The
// ring's wire layout is identical to what a real shm_open / mmap mapping
// would produce, so Phase 2 just swaps the allocation strategy.
//
// TODO(phase2): on Linux/macOS use shm_open(name, O_CREAT|O_RDWR) +
// ftruncate + mmap; on Windows use CreateFileMappingA + MapViewOfFile.
// The --shm-name argv flag will then be a real, cross-process name.

#include "ipc/shm_ring.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace zeus::plughost {

namespace {

bool IsPowerOfTwo(std::uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

std::size_t RoundUpToCacheLine(std::size_t n) {
    constexpr std::size_t kLine = 64;
    return (n + kLine - 1) & ~(kLine - 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// ShmRing
// ---------------------------------------------------------------------------

ShmRing::ShmRing(RingControlBlock* control, std::uint8_t* slots)
    : control_(control), slots_(slots) {}

BlockHeader* ShmRing::SlotAt(std::uint64_t index) {
    const std::uint32_t mask = control_->slotCount - 1;
    const std::uint64_t i = index & mask;
    return reinterpret_cast<BlockHeader*>(
        slots_ + static_cast<std::size_t>(i) * control_->slotBytes);
}

BlockHeader* ShmRing::Acquire() {
    const std::uint64_t head = control_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = control_->tail.load(std::memory_order_acquire);
    if (head - tail >= control_->slotCount) {
        return nullptr;  // full
    }
    return SlotAt(head);
}

void ShmRing::Publish(BlockHeader* header) {
    (void)header;  // header pointer is implied by current head index
    const std::uint64_t head = control_->head.load(std::memory_order_relaxed);
    control_->head.store(head + 1, std::memory_order_release);
}

BlockHeader* ShmRing::Read() {
    const std::uint64_t tail = control_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = control_->head.load(std::memory_order_acquire);
    if (head == tail) {
        return nullptr;  // empty
    }
    return SlotAt(tail);
}

void ShmRing::Release(BlockHeader* header) {
    (void)header;
    const std::uint64_t tail = control_->tail.load(std::memory_order_relaxed);
    control_->tail.store(tail + 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ShmRingMapping
// ---------------------------------------------------------------------------

ShmRingMapping::ShmRingMapping(const std::string& name,
                               std::uint32_t frames,
                               std::uint32_t channels,
                               std::uint32_t sampleRate,
                               std::uint32_t slotCount)
    : name_(name),
      mappingBytes_(0),
      mapping_(nullptr),
      control_(nullptr),
      slots_(nullptr),
      ring_(nullptr, nullptr) {
    if (!IsPowerOfTwo(slotCount)) {
        throw std::invalid_argument("ShmRingMapping: slotCount must be a power of two");
    }

    const std::size_t slotBytes   = RoundUpToCacheLine(BlockBytes(frames, channels));
    const std::size_t controlBytes = sizeof(RingControlBlock);
    const std::size_t totalBytes  = controlBytes + slotBytes * slotCount;

    // Phase 1: heap-backed. Use aligned alloc so the control block sits on
    // a cache-line boundary, matching what mmap will give us in Phase 2.
    void* raw = nullptr;
#if defined(_MSC_VER)
    raw = _aligned_malloc(totalBytes, 64);
#else
    if (posix_memalign(&raw, 64, totalBytes) != 0) {
        raw = nullptr;
    }
#endif
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    std::memset(raw, 0, totalBytes);

    mapping_      = raw;
    mappingBytes_ = totalBytes;
    control_      = new (raw) RingControlBlock();
    control_->head.store(0, std::memory_order_relaxed);
    control_->tail.store(0, std::memory_order_relaxed);
    control_->slotCount  = slotCount;
    control_->slotBytes  = static_cast<std::uint32_t>(slotBytes);
    control_->frames     = frames;
    control_->channels   = channels;
    control_->sampleRate = sampleRate;

    slots_ = static_cast<std::uint8_t*>(raw) + controlBytes;
    ring_  = ShmRing(control_, slots_);
}

ShmRingMapping::~ShmRingMapping() {
    if (mapping_ != nullptr) {
#if defined(_MSC_VER)
        _aligned_free(mapping_);
#else
        std::free(mapping_);
#endif
        mapping_ = nullptr;
    }
}

}  // namespace zeus::plughost
