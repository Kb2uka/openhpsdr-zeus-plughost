// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// plugin_chain.h — Phase 3a serial plugin chain.
//
// One PluginChain owns up to kMaxSlots (8) PluginHost instances. The audio
// thread walks slot 0..7 in order, feeding each loaded + unbypassed slot
// the running buffer (ping-ponging between two scratch buffers so we never
// allocate or copy in the audio loop). When the master toggle is OFF, the
// chain is a single memcpy from input to output (bit-identical pass-through).
//
// Threading model:
//
//   - Slot membership (which slots are loaded) is owned by the control
//     thread. The audio thread reads through std::atomic<bool> masks, so
//     it never touches a slot that isn't fully populated.
//   - Per-slot bypass + master enable are std::atomic<bool>, written from
//     the control thread, read once per Process call.
//   - LoadSlot / UnloadSlot serialise on a chain-level mutex (in addition
//     to PluginHost's own internal control mutex) so two concurrent loads
//     into the same slot can't fight.
//   - Process() is allocation-free, lock-free, syscall-free. Every check
//     it does is on a relaxed atomic.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vst3/plugin_host.h"

namespace zeus::plughost::vst2 { class Vst2Plugin; }
namespace zeus::plughost::clap { class ClapPlugin; }

namespace Steinberg {
class IPlugView;
}

namespace zeus::plughost::vst3 {

class IEditorIdlePump;

class PluginChain {
public:
    static constexpr int kMaxSlots = 8;

    // Per-slot format tag — exposed so the dispatcher in plugin_chain.cpp
    // (and any future status query) can name them. Operator-facing UI
    // doesn't see this directly; the slot info travels back to the host
    // through ChainSlot.Plugin.Path on the IPC seam.
    enum class SlotFormat : std::uint8_t {
        Empty = 0,
        Vst3  = 1,
        Vst2  = 2,
        Clap  = 3
    };

    // sampleRate + maxBlockSize are the same geometry the host negotiated
    // at the Hello handshake; Phase 3a is hardcoded to 48000 / 256.
    PluginChain(double sampleRate, std::int32_t maxBlockSize);
    ~PluginChain();

    PluginChain(const PluginChain&)            = delete;
    PluginChain& operator=(const PluginChain&) = delete;

    // ---- Control thread API -----------------------------------------------

    // Load a plugin into slot `slotIdx`. Returns the same LoadResult shape
    // as PluginHost::Load. If the slot is already loaded, the existing
    // plugin is unloaded first. Returns LoadError{6,...} on out-of-range
    // slot index.
    LoadResult LoadSlot(int slotIdx, const std::string& path);

    // Unload slot `slotIdx`. Returns:
    //   0 = ok, the slot was loaded and is now empty.
    //   1 = no-plugin-loaded (slot was already empty).
    //   6 = invalid-slot-index.
    std::uint8_t UnloadSlot(int slotIdx);

    // Set per-slot bypass. Bypassed slots are skipped on the audio thread.
    // Returns 0 ok, 6 invalid-slot.
    std::uint8_t SetSlotBypass(int slotIdx, bool bypass);

    // Master enable. When false, Process() is a single memcpy.
    void SetChainEnabled(bool enabled);
    bool IsChainEnabled() const noexcept;

    bool IsSlotLoaded(int slotIdx) const noexcept;
    bool IsSlotBypassed(int slotIdx) const noexcept;

    // Parameter introspection.
    //   status:
    //     0 = ok
    //     1 = no-plugin-loaded
    //     5 = other
    //     6 = invalid-slot-index
    //     7 = controller-unavailable
    std::uint8_t ListParams(int slotIdx, std::vector<ParamInfo>& outParams);

    // Returns:
    //   0 = ok, outActual carries the post-clamp/quantise value.
    //   1 = no-plugin-loaded
    //   5 = other (paramId not found)
    //   6 = invalid-slot-index
    //   7 = controller-unavailable
    std::uint8_t SetParam(int slotIdx, std::uint32_t paramId,
                          double normalized, double& outActual);

    // Phase 3 GUI: acquire the slot's plugin editor view. Returns null
    // if the slot is empty or the plugin has no editor / controller.
    // The pointer is owned by the slot's PluginHost; do NOT call
    // release() / FUnknownPtr::release() yourself — pair with
    // ReleaseEditorView when done.
    Steinberg::IPlugView* AcquireEditorView(int slotIdx);

    // Idempotent. Drops the slot's editor view if held.
    void ReleaseEditorView(int slotIdx);

    // Returns the slot's IEditorIdlePump (the same object reachable
    // through the wrapper) or nullptr for VST3 slots / empty slots.
    // Lifetime is the same as AcquireEditorView's: the pointer is valid
    // until ReleaseEditorView is called for this slot. The GUI thread
    // uses this to avoid having to dynamic_cast across the plugin DSO
    // boundary, which can SEGV with plugins that ship hidden RTTI.
    IEditorIdlePump* AcquireEditorIdlePump(int slotIdx);

    // Wave 7: install a chain-level callback that fires on every editor-
    // driven (or plugin-internal-automation-driven) parameter change. The
    // sidecar wraps each slot's PluginHost callback with the slot index so
    // main.cpp can serialise a single ParamChanged frame per fire. Setting
    // null disconnects from every slot. Idempotent.
    using ParamChangedCallback = std::function<void(
        int slotIdx, std::uint32_t paramId, double normalizedValue)>;
    void SetParamChangedCallback(ParamChangedCallback cb);

    // ---- Audio thread API -------------------------------------------------

    // Process one block. `in` and `out` are mono planar pointers. `frames`
    // <= maxBlockSize. Output equals input if the chain is disabled or all
    // loaded slots are bypassed (no plugin processes the buffer); the
    // memcpy still runs in that case so the caller gets a clean copy.
    void Process(const float* in, float* out, std::int32_t frames) noexcept;

private:
    // Per-slot format tag. Set under chainMutex_ at LoadSlot time;
    // read on the audio thread under the loaded_ atomic gate so the
    // audio thread sees a coherent (format, populated-pointer) pair.
    std::array<std::atomic<SlotFormat>, kMaxSlots> format_{};

    // Per-format slot pointers. Exactly one is non-null per slot, paired
    // with format_[i] != Empty. Each format owns its own array so we
    // don't need a tagged union with non-trivial destructors.
    std::array<std::unique_ptr<PluginHost>,           kMaxSlots> slotsVst3_;
    std::array<std::unique_ptr<vst2::Vst2Plugin>,     kMaxSlots> slotsVst2_;
    std::array<std::unique_ptr<clap::ClapPlugin>,     kMaxSlots> slotsClap_;

    std::array<std::atomic<bool>,            kMaxSlots> bypass_{};
    std::array<std::atomic<bool>,            kMaxSlots> loaded_{};
    std::atomic<bool> masterEnabled_{false};

    std::mutex chainMutex_;  // serialises Load/Unload/SetParam against each other.

    // Cached chain-level callback. Stored under chainMutex_; re-installed
    // on every PluginHost when a slot loads (LoadSlot does this). Per-slot
    // wrapper closures capture the slot index so the host (main.cpp) gets
    // (slot, paramId, value) tuples without a per-fire heap alloc.
    ParamChangedCallback paramChangedCb_;

    // Hot scratch buffers — two ping-pong buffers so the audio loop can
    // chain N plugins without allocating. alignas(64) keeps them on a
    // cache line. Sized at construction by maxBlockSize.
    std::int32_t maxBlockSize_;
    double       sampleRate_;
    alignas(64) std::vector<float> scratchA_;
    alignas(64) std::vector<float> scratchB_;
};

}  // namespace zeus::plughost::vst3