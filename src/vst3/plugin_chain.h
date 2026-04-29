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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vst3/plugin_host.h"

namespace Steinberg {
class IPlugView;
}

namespace zeus::plughost::vst3 {

class PluginChain {
public:
    static constexpr int kMaxSlots = 8;

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

    // ---- Audio thread API -------------------------------------------------

    // Process one block. `in` and `out` are mono planar pointers. `frames`
    // <= maxBlockSize. Output equals input if the chain is disabled or all
    // loaded slots are bypassed (no plugin processes the buffer); the
    // memcpy still runs in that case so the caller gets a clean copy.
    void Process(const float* in, float* out, std::int32_t frames) noexcept;

private:
    // Per-slot bypass + loaded flag. Both written by control thread, read
    // by audio thread. Loaded flag is set ONLY after PluginHost::Load
    // returns success (so the audio thread can safely call Process on the
    // PluginHost without seeing a half-initialised slot).
    std::array<std::unique_ptr<PluginHost>, kMaxSlots> slots_;
    std::array<std::atomic<bool>,            kMaxSlots> bypass_{};
    std::array<std::atomic<bool>,            kMaxSlots> loaded_{};
    std::atomic<bool> masterEnabled_{false};

    std::mutex chainMutex_;  // serialises Load/Unload/SetParam against each other.

    // Hot scratch buffers — two ping-pong buffers so the audio loop can
    // chain N plugins without allocating. alignas(64) keeps them on a
    // cache line. Sized at construction by maxBlockSize.
    std::int32_t maxBlockSize_;
    double       sampleRate_;
    alignas(64) std::vector<float> scratchA_;
    alignas(64) std::vector<float> scratchB_;
};

}  // namespace zeus::plughost::vst3
