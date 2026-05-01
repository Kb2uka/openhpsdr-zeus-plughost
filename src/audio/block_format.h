// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// block_format.h — wire layout for the audio data plane.
//
// Each block in the SPSC shared-memory ring is a 64-byte cache-line header
// followed by `frames * channels` float32 samples in PLANAR order:
//
//   [BlockHeader (64B)] [ch0 sample0..sampleN-1] [ch1 sample0..sampleN-1] ...
//
// PLANAR (not interleaved) was chosen because most VST3 / CLAP plugins want
// per-channel pointers; interleaving would force an extra deinterleave copy
// in the realtime audio thread, which is a malloc / SIMD trap we want to
// avoid.
//
// Phase 1 fixed parameters (compile-time constants):
//
//   - frames per block : 256
//   - sample rate      : 48000 Hz
//   - channels         : 1 (mono)
//   - ring depth       : 8 blocks per direction (input + output)
//
// TODO(phase2): replace the FRAMES / RATE / CHANNELS constants with values
// negotiated through the control-channel `Hello` message so Zeus can pick
// the format at startup.

#pragma once

#include <cstddef>
#include <cstdint>

namespace zeus::plughost {

// ---- Phase 1 fixed format ------------------------------------------------
inline constexpr std::uint32_t kPhase1Frames     = 256;
inline constexpr std::uint32_t kPhase1Channels   = 1;
inline constexpr std::uint32_t kPhase1SampleRate = 48000;
inline constexpr std::uint32_t kPhase1RingDepth  = 8;

// ---- Block header flag bits ----------------------------------------------
//
// bit0 : bypass   — host should pass input straight through (no plugin run)
// bit1 : silence  — payload may be undefined; consumer should treat as zeros
// bit2..31        — reserved, MUST be zero on write.
inline constexpr std::uint32_t kFlagBypass  = 1u << 0;
inline constexpr std::uint32_t kFlagSilence = 1u << 1;

// ---- 64-byte cache-line header ------------------------------------------
//
// Field order is wire-stable across host architectures; do not reorder
// without bumping the protocol version (added via control-plane Hello in
// Phase 2). All fields are little-endian on the only supported targets
// (x86_64, arm64-LE).
struct alignas(64) BlockHeader {
    std::uint64_t seq;          // monotonic, set by writer before publishing
    std::uint32_t frames;       // samples per channel
    std::uint32_t channels;     // 1 for mono Phase 1
    std::uint32_t sampleRate;   // 48000 Phase 1
    std::uint32_t flags;        // bit0 = bypass, bit1 = silence, bit2..31 reserved
    std::uint8_t  reserved[40]; // must be zeroed on write
};

static_assert(sizeof(BlockHeader) == 64, "BlockHeader must be one cache line");
static_assert(alignof(BlockHeader) == 64, "BlockHeader must be cache-line aligned");

// Bytes occupied by one full block (header + planar payload) for a given
// frame / channel count. Used to size the shared-memory ring at startup.
constexpr std::size_t BlockBytes(std::uint32_t frames, std::uint32_t channels) {
    return sizeof(BlockHeader)
         + static_cast<std::size_t>(frames)
         * static_cast<std::size_t>(channels)
         * sizeof(float);
}

// Pointer to the planar payload that follows a header in memory.
// Caller is responsible for ensuring `header` actually has a payload behind
// it (i.e. lives inside a properly-sized ring slot).
inline float* PayloadOf(BlockHeader* header) {
    return reinterpret_cast<float*>(header + 1);
}

inline const float* PayloadOf(const BlockHeader* header) {
    return reinterpret_cast<const float*>(header + 1);
}

}  // namespace zeus::plughost