// plugin_chain.cpp — Phase 3a 8-slot serial plugin chain implementation.

#include "vst3/plugin_chain.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace zeus::plughost::vst3 {

PluginChain::PluginChain(double sampleRate, std::int32_t maxBlockSize)
    : maxBlockSize_(maxBlockSize), sampleRate_(sampleRate) {
    for (int i = 0; i < kMaxSlots; ++i) {
        slots_[i] = std::make_unique<PluginHost>();
        bypass_[i].store(false, std::memory_order_relaxed);
        loaded_[i].store(false, std::memory_order_relaxed);
    }
    // Pre-size scratch buffers to maxBlockSize. We never resize after
    // this; if a future caller sends a larger block we clamp inside
    // Process().
    scratchA_.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
    scratchB_.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
}

PluginChain::~PluginChain() {
    // Take chain mutex — no audio thread can be inside Process here
    // (the supervisor must have stopped the audio loop already), but
    // the chain mutex serialises with any control-thread leftovers.
    std::lock_guard<std::mutex> guard(chainMutex_);
    for (int i = 0; i < kMaxSlots; ++i) {
        loaded_[i].store(false, std::memory_order_release);
        if (slots_[i]) {
            slots_[i]->Unload();
        }
    }
}

LoadResult PluginChain::LoadSlot(int slotIdx, const std::string& path) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) {
        return LoadError{6, "invalid slot index"};
    }
    std::lock_guard<std::mutex> guard(chainMutex_);

    // If the slot is already loaded, mark it not-loaded BEFORE the
    // PluginHost-level Unload+Load happens. The audio thread sees the
    // flag flip first (release on store) and skips the slot during the
    // brief reload window.
    loaded_[slotIdx].store(false, std::memory_order_release);
    // PluginHost::Load is itself atomic (it Unloads any prior plugin
    // before the new one becomes visible to its audio reader). We just
    // call it; on success we re-set the loaded flag.
    LoadResult r = slots_[slotIdx]->Load(path, sampleRate_, maxBlockSize_);
    if (std::holds_alternative<LoadInfo>(r)) {
        loaded_[slotIdx].store(true, std::memory_order_release);
    }
    return r;
}

std::uint8_t PluginChain::UnloadSlot(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) {
        return 6;
    }
    std::lock_guard<std::mutex> guard(chainMutex_);
    bool was = loaded_[slotIdx].exchange(false, std::memory_order_acq_rel);
    slots_[slotIdx]->Unload();
    return was ? 0u : 1u;
}

std::uint8_t PluginChain::SetSlotBypass(int slotIdx, bool bypass) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) {
        return 6;
    }
    bypass_[slotIdx].store(bypass, std::memory_order_release);
    return 0;
}

void PluginChain::SetChainEnabled(bool enabled) {
    masterEnabled_.store(enabled, std::memory_order_release);
}

bool PluginChain::IsChainEnabled() const noexcept {
    return masterEnabled_.load(std::memory_order_acquire);
}

bool PluginChain::IsSlotLoaded(int slotIdx) const noexcept {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return false;
    return loaded_[slotIdx].load(std::memory_order_acquire);
}

bool PluginChain::IsSlotBypassed(int slotIdx) const noexcept {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return false;
    return bypass_[slotIdx].load(std::memory_order_acquire);
}

std::uint8_t PluginChain::ListParams(int slotIdx,
                                     std::vector<ParamInfo>& outParams) {
    outParams.clear();
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return 6;
    std::lock_guard<std::mutex> guard(chainMutex_);
    if (!loaded_[slotIdx].load(std::memory_order_acquire)) {
        return 1;
    }
    auto list = slots_[slotIdx]->ListParams();
    if (list.empty()) {
        // PluginHost returns empty if there is no controller OR if the
        // controller reports zero parameters. We can't distinguish the
        // two without an extra plumb; report controller-unavailable when
        // the plugin is loaded but the list is empty — most plugins that
        // genuinely have zero parameters are still reachable as 0-len
        // lists, but an empty result is rare enough that the operator
        // sees the (correct) hint.
        return 7;
    }
    outParams = std::move(list);
    return 0;
}

std::uint8_t PluginChain::SetParam(int slotIdx, std::uint32_t paramId,
                                   double normalized, double& outActual) {
    outActual = std::nan("");
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return 6;
    std::lock_guard<std::mutex> guard(chainMutex_);
    if (!loaded_[slotIdx].load(std::memory_order_acquire)) {
        return 1;
    }
    double v = slots_[slotIdx]->SetParam(paramId, normalized);
    if (std::isnan(v)) {
        // PluginHost SetParam returns NaN if no controller. We don't
        // strictly know whether the param id was wrong or the controller
        // is missing — return controller-unavailable as the safer
        // diagnostic. In practice if ListParams returned the id, the
        // controller exists.
        return 7;
    }
    outActual = v;
    return 0;
}

// ---------------------------------------------------------------------------
// Audio thread Process. THIS MUST STAY ALLOC-FREE / LOCK-FREE / SYSCALL-FREE.
// ---------------------------------------------------------------------------

void PluginChain::Process(const float* in, float* out,
                          std::int32_t frames) noexcept {
    if (frames <= 0) return;
    // Defensive clamp — caller is supposed to honour maxBlockSize but a
    // misbehaving caller would otherwise scribble past our scratch.
    if (frames > maxBlockSize_) frames = maxBlockSize_;

    const std::size_t bytes =
        static_cast<std::size_t>(frames) * sizeof(float);

    // Fast path: master disabled. Single memcpy, bit-identical.
    if (!masterEnabled_.load(std::memory_order_acquire)) {
        std::memcpy(out, in, bytes);
        return;
    }

    // Walk slots. The "current input" pointer starts at the caller's
    // input; on each loaded+unbypassed slot we run the plugin into one
    // of the two scratch buffers, then make that scratch the next slot's
    // input. Output is whichever buffer the last processing slot wrote
    // into; if no slot processed, we memcpy from the original input.
    const float* cur     = in;
    float*       scratch[2] = { scratchA_.data(), scratchB_.data() };
    int          which   = 0;
    bool         touched = false;

    for (int i = 0; i < kMaxSlots; ++i) {
        // Load order: skip empty slots without touching the PluginHost
        // pointer, since the loaded flag is the publish edge for the
        // PluginHost being safely callable.
        if (!loaded_[i].load(std::memory_order_acquire)) continue;
        if (bypass_[i].load(std::memory_order_acquire)) continue;

        float* dst = scratch[which];
        bool ok = slots_[i]->Process(cur, dst, frames);
        if (!ok) {
            // PluginHost::Process returns false on transient swap or a
            // plugin-reported failure. Keep cur unchanged and move on;
            // the slot effectively bypasses for this block.
            continue;
        }
        cur     = dst;
        which   = which ^ 1;
        touched = true;
    }

    if (touched) {
        // cur points at the last scratch buffer the chain wrote into.
        std::memcpy(out, cur, bytes);
    } else {
        // Chain enabled but every loaded slot was bypassed (or empty).
        // Bit-identical pass-through.
        std::memcpy(out, in, bytes);
    }
}

}  // namespace zeus::plughost::vst3
