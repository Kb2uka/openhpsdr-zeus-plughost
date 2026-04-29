// shm_ring.cpp — SPSC ring impl backed by POSIX shared memory.
//
// Phase 2: real shm_open + ftruncate + mmap. Host creates; sidecar opens.
// The mapping size is rounded up to a 4096-byte page boundary so mmap is
// happy on every Linux kernel we care about. Wire layout (control block +
// slots) matches Phase 1 byte-for-byte.

#include "ipc/shm_ring.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zeus::plughost {

namespace {

constexpr std::size_t kPageBytes = 4096;

bool IsPowerOfTwo(std::uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

std::size_t RoundUpToCacheLine(std::size_t n) {
    constexpr std::size_t kLine = 64;
    return (n + kLine - 1) & ~(kLine - 1);
}

std::size_t RoundUpToPage(std::size_t n) {
    return (n + kPageBytes - 1) & ~(kPageBytes - 1);
}

std::string ErrnoMessage(const char* what) {
    char buf[256];
#if defined(__GLIBC__) && (_GNU_SOURCE)
    char* msg = strerror_r(errno, buf, sizeof(buf));
    return std::string(what) + ": " + (msg ? msg : "(unknown)");
#else
    if (strerror_r(errno, buf, sizeof(buf)) != 0) {
        std::snprintf(buf, sizeof(buf), "errno=%d", errno);
    }
    return std::string(what) + ": " + buf;
#endif
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
    std::atomic_thread_fence(std::memory_order_release);
    control_->head.store(head + 1, std::memory_order_release);
}

BlockHeader* ShmRing::Read() {
    const std::uint64_t tail = control_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = control_->head.load(std::memory_order_acquire);
    if (head == tail) {
        return nullptr;  // empty
    }
    std::atomic_thread_fence(std::memory_order_acquire);
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

ShmRingMapping::ShmRingMapping()
    : name_(),
      mappingBytes_(0),
      mapping_(nullptr),
      control_(nullptr),
      slots_(nullptr),
      ownsName_(false),
      ring_(nullptr, nullptr) {}

ShmRingMapping ShmRingMapping::CreateNew(const std::string& name,
                                         std::uint32_t frames,
                                         std::uint32_t channels,
                                         std::uint32_t sampleRate,
                                         std::uint32_t slotCount) {
    if (!IsPowerOfTwo(slotCount)) {
        throw std::invalid_argument("ShmRingMapping: slotCount must be a power of two");
    }
    if (name.empty() || name[0] != '/') {
        throw std::invalid_argument("ShmRingMapping: shm name must start with '/'");
    }

    const std::size_t slotBytes    = RoundUpToCacheLine(BlockBytes(frames, channels));
    const std::size_t controlBytes = sizeof(RingControlBlock);
    const std::size_t logicalBytes = controlBytes + slotBytes * slotCount;
    const std::size_t totalBytes   = RoundUpToPage(logicalBytes);

    // Create the named region. Unlink first to recover from a previous
    // crash — we are the host (creator) and own the name.
    ::shm_unlink(name.c_str());
    int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        throw std::runtime_error(ErrnoMessage("shm_open(O_CREAT)"));
    }

    if (::ftruncate(fd, static_cast<off_t>(totalBytes)) != 0) {
        const std::string err = ErrnoMessage("ftruncate");
        ::close(fd);
        ::shm_unlink(name.c_str());
        throw std::runtime_error(err);
    }

    void* raw = ::mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    ::close(fd);  // mapping survives close
    if (raw == MAP_FAILED) {
        ::shm_unlink(name.c_str());
        throw std::runtime_error(ErrnoMessage("mmap"));
    }

    std::memset(raw, 0, totalBytes);

    auto* control = new (raw) RingControlBlock();
    control->head.store(0, std::memory_order_relaxed);
    control->tail.store(0, std::memory_order_relaxed);
    control->slotCount  = slotCount;
    control->slotBytes  = static_cast<std::uint32_t>(slotBytes);
    control->frames     = frames;
    control->channels   = channels;
    control->sampleRate = sampleRate;

    auto* slots = static_cast<std::uint8_t*>(raw) + controlBytes;

    ShmRingMapping m;
    m.name_         = name;
    m.mappingBytes_ = totalBytes;
    m.mapping_      = raw;
    m.control_      = control;
    m.slots_        = slots;
    m.ownsName_     = true;
    m.ring_         = ShmRing(control, slots);
    return m;
}

ShmRingMapping ShmRingMapping::OpenExisting(const std::string& name,
                                            std::uint32_t expectedFrames,
                                            std::uint32_t expectedChannels,
                                            std::uint32_t expectedSampleRate,
                                            std::uint32_t expectedSlotCount) {
    if (name.empty() || name[0] != '/') {
        throw std::invalid_argument("ShmRingMapping: shm name must start with '/'");
    }
    if (!IsPowerOfTwo(expectedSlotCount)) {
        throw std::invalid_argument("ShmRingMapping: expected slotCount must be a power of two");
    }

    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) {
        throw std::runtime_error(ErrnoMessage("shm_open(existing)"));
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const std::string err = ErrnoMessage("fstat");
        ::close(fd);
        throw std::runtime_error(err);
    }
    const std::size_t totalBytes = static_cast<std::size_t>(st.st_size);
    if (totalBytes < sizeof(RingControlBlock)) {
        ::close(fd);
        throw std::runtime_error("ShmRingMapping: existing region too small");
    }

    void* raw = ::mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    ::close(fd);
    if (raw == MAP_FAILED) {
        throw std::runtime_error(ErrnoMessage("mmap(existing)"));
    }

    auto* control = static_cast<RingControlBlock*>(raw);

    if (control->slotCount  != expectedSlotCount  ||
        control->frames     != expectedFrames     ||
        control->channels   != expectedChannels   ||
        control->sampleRate != expectedSampleRate) {
        ::munmap(raw, totalBytes);
        throw std::runtime_error(
            "ShmRingMapping: ring geometry mismatch with creator");
    }

    auto* slots = static_cast<std::uint8_t*>(raw) + sizeof(RingControlBlock);

    ShmRingMapping m;
    m.name_         = name;
    m.mappingBytes_ = totalBytes;
    m.mapping_      = raw;
    m.control_      = control;
    m.slots_        = slots;
    m.ownsName_     = false;
    m.ring_         = ShmRing(control, slots);
    return m;
}

ShmRingMapping::~ShmRingMapping() {
    if (mapping_ != nullptr) {
        ::munmap(mapping_, mappingBytes_);
        mapping_ = nullptr;
    }
    if (ownsName_ && !name_.empty()) {
        // Host owns the name. On clean exit we unlink so /dev/shm doesn't
        // accumulate stale entries; on crash the OS leaves them and the
        // host's next CreateNew() unlinks first.
        ::shm_unlink(name_.c_str());
    }
}

ShmRingMapping::ShmRingMapping(ShmRingMapping&& other) noexcept
    : name_(std::move(other.name_)),
      mappingBytes_(other.mappingBytes_),
      mapping_(other.mapping_),
      control_(other.control_),
      slots_(other.slots_),
      ownsName_(other.ownsName_),
      ring_(other.control_, other.slots_) {
    other.mappingBytes_ = 0;
    other.mapping_      = nullptr;
    other.control_      = nullptr;
    other.slots_        = nullptr;
    other.ownsName_     = false;
    other.ring_         = ShmRing(nullptr, nullptr);
}

ShmRingMapping& ShmRingMapping::operator=(ShmRingMapping&& other) noexcept {
    if (this != &other) {
        if (mapping_ != nullptr) {
            ::munmap(mapping_, mappingBytes_);
        }
        if (ownsName_ && !name_.empty()) {
            ::shm_unlink(name_.c_str());
        }
        name_         = std::move(other.name_);
        mappingBytes_ = other.mappingBytes_;
        mapping_      = other.mapping_;
        control_      = other.control_;
        slots_        = other.slots_;
        ownsName_     = other.ownsName_;
        ring_         = ShmRing(other.control_, other.slots_);

        other.mappingBytes_ = 0;
        other.mapping_      = nullptr;
        other.control_      = nullptr;
        other.slots_        = nullptr;
        other.ownsName_     = false;
        other.ring_         = ShmRing(nullptr, nullptr);
    }
    return *this;
}

}  // namespace zeus::plughost
