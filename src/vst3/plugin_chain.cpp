// plugin_chain.cpp — multi-format 8-slot serial plugin chain.
//
// Each slot can hold a VST3, a Linux VST2, or a CLAP plugin. The format
// is detected at LoadSlot time from the path (`.vst3` / `.so` / `.clap`)
// and the appropriate format-specific host class is instantiated. The
// audio thread walks slots 0..7 dispatching by format under each loaded_
// gate atomic.

#include "vst3/plugin_chain.h"
#include "vst2/vst2_plugin.h"
#include "vst2/vst2_view.h"
#include "clap/clap_plugin.h"
#include "clap/clap_view.h"
#include "vst3/editor_idle_pump.h"

#include "pluginterfaces/gui/iplugview.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace zeus::plughost::vst3 {

namespace {

// Decide which format a path looks like. We sniff cheap path-extension
// signals first; deeper probes (VST2 entry-symbol presence) happen
// inside the format's own LooksLike helper. Returns Empty if nothing
// matches.
PluginChain::SlotFormat DetectFormat(const std::string& path) {
    // VST3 bundle directories end in `.vst3`. The VST3 SDK's
    // Module::create handles validation downstream; we just route there.
    if (path.size() >= 5 &&
        path.compare(path.size() - 5, 5, ".vst3") == 0) {
        return PluginChain::SlotFormat::Vst3;
    }
    if (clap::LooksLikeClap(path)) {
        return PluginChain::SlotFormat::Clap;
    }
    // VST2 last because it requires a dlopen probe.
    if (vst2::LooksLikeVst2(path)) {
        return PluginChain::SlotFormat::Vst2;
    }
    return PluginChain::SlotFormat::Empty;
}

}  // namespace

PluginChain::PluginChain(double sampleRate, std::int32_t maxBlockSize)
    : maxBlockSize_(maxBlockSize), sampleRate_(sampleRate) {
    for (int i = 0; i < kMaxSlots; ++i) {
        format_[i].store(SlotFormat::Empty, std::memory_order_relaxed);
        bypass_[i].store(false, std::memory_order_relaxed);
        loaded_[i].store(false, std::memory_order_relaxed);
    }
    scratchA_.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
    scratchB_.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
}

PluginChain::~PluginChain() {
    std::lock_guard<std::mutex> guard(chainMutex_);
    for (int i = 0; i < kMaxSlots; ++i) {
        loaded_[i].store(false, std::memory_order_release);
        switch (format_[i].load(std::memory_order_relaxed)) {
            case SlotFormat::Vst3:
                if (slotsVst3_[i]) {
                    slotsVst3_[i]->ReleaseEditorView();
                    slotsVst3_[i]->Unload();
                    slotsVst3_[i].reset();
                }
                break;
            case SlotFormat::Vst2:
                if (slotsVst2_[i]) {
                    slotsVst2_[i]->Unload();
                    slotsVst2_[i].reset();
                }
                break;
            case SlotFormat::Clap:
                if (slotsClap_[i]) {
                    slotsClap_[i]->Unload();
                    slotsClap_[i].reset();
                }
                break;
            case SlotFormat::Empty:
                break;
        }
        format_[i].store(SlotFormat::Empty, std::memory_order_relaxed);
    }
}

LoadResult PluginChain::LoadSlot(int slotIdx, const std::string& path) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) {
        return LoadError{6, "invalid slot index"};
    }
    std::lock_guard<std::mutex> guard(chainMutex_);

    // Mark not-loaded BEFORE swapping in the new plugin so the audio
    // thread skips this slot during the brief reload window.
    loaded_[slotIdx].store(false, std::memory_order_release);

    // Tear down whatever was previously in this slot, regardless of
    // its old format — a re-load may switch formats.
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) {
                slotsVst3_[slotIdx]->ReleaseEditorView();
                slotsVst3_[slotIdx]->Unload();
                slotsVst3_[slotIdx].reset();
            }
            break;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) {
                slotsVst2_[slotIdx]->Unload();
                slotsVst2_[slotIdx].reset();
            }
            break;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) {
                slotsClap_[slotIdx]->Unload();
                slotsClap_[slotIdx].reset();
            }
            break;
        case SlotFormat::Empty:
            break;
    }
    format_[slotIdx].store(SlotFormat::Empty, std::memory_order_relaxed);

    // Detect format and route to the appropriate factory.
    SlotFormat fmt = DetectFormat(path);
    LoadResult r;
    switch (fmt) {
        case SlotFormat::Vst3: {
            slotsVst3_[slotIdx] = std::make_unique<PluginHost>();
            r = slotsVst3_[slotIdx]->Load(path, sampleRate_, maxBlockSize_);
            break;
        }
        case SlotFormat::Vst2: {
            slotsVst2_[slotIdx] = std::make_unique<vst2::Vst2Plugin>();
            r = slotsVst2_[slotIdx]->Load(path, sampleRate_, maxBlockSize_);
            break;
        }
        case SlotFormat::Clap: {
            slotsClap_[slotIdx] = std::make_unique<clap::ClapPlugin>();
            r = slotsClap_[slotIdx]->Load(path, sampleRate_, maxBlockSize_);
            break;
        }
        case SlotFormat::Empty:
        default:
            return LoadError{2, "unrecognised plugin format (expected .vst3 / .so / .clap)"};
    }

    if (std::holds_alternative<LoadInfo>(r)) {
        format_[slotIdx].store(fmt, std::memory_order_release);
        loaded_[slotIdx].store(true, std::memory_order_release);

        // Re-install the chain-level performEdit callback wrapped with
        // the slot index so each format reports back through the same
        // (slot, paramId, value) channel.
        if (paramChangedCb_) {
            auto cbCopy = paramChangedCb_;
            auto wrap = [cbCopy = std::move(cbCopy), slotIdx](
                std::uint32_t paramId, double valueNormalized) {
                cbCopy(slotIdx, paramId, valueNormalized);
            };
            switch (fmt) {
                case SlotFormat::Vst3:
                    slotsVst3_[slotIdx]->SetParamChangedCallback(wrap);
                    break;
                case SlotFormat::Vst2:
                    slotsVst2_[slotIdx]->SetParamChangedCallback(wrap);
                    break;
                case SlotFormat::Clap:
                    slotsClap_[slotIdx]->SetParamChangedCallback(wrap);
                    break;
                case SlotFormat::Empty:
                    break;
            }
        }
    } else {
        // Load failed — drop the format-specific slot pointer.
        switch (fmt) {
            case SlotFormat::Vst3: slotsVst3_[slotIdx].reset(); break;
            case SlotFormat::Vst2: slotsVst2_[slotIdx].reset(); break;
            case SlotFormat::Clap: slotsClap_[slotIdx].reset(); break;
            case SlotFormat::Empty: break;
        }
    }
    return r;
}

void PluginChain::SetParamChangedCallback(ParamChangedCallback cb) {
    std::lock_guard<std::mutex> guard(chainMutex_);
    paramChangedCb_ = std::move(cb);
    for (int i = 0; i < kMaxSlots; ++i) {
        SlotFormat fmt = format_[i].load(std::memory_order_relaxed);
        if (fmt == SlotFormat::Empty) continue;
        std::function<void(std::uint32_t, double)> wrap;
        if (paramChangedCb_) {
            auto cbCopy = paramChangedCb_;
            wrap = [cbCopy = std::move(cbCopy), i](
                std::uint32_t paramId, double valueNormalized) {
                cbCopy(i, paramId, valueNormalized);
            };
        }
        switch (fmt) {
            case SlotFormat::Vst3:
                if (slotsVst3_[i]) slotsVst3_[i]->SetParamChangedCallback(wrap);
                break;
            case SlotFormat::Vst2:
                if (slotsVst2_[i]) slotsVst2_[i]->SetParamChangedCallback(wrap);
                break;
            case SlotFormat::Clap:
                if (slotsClap_[i]) slotsClap_[i]->SetParamChangedCallback(wrap);
                break;
            case SlotFormat::Empty: break;
        }
    }
}

std::uint8_t PluginChain::UnloadSlot(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return 6;
    std::lock_guard<std::mutex> guard(chainMutex_);
    bool was = loaded_[slotIdx].exchange(false, std::memory_order_acq_rel);
    SlotFormat fmt = format_[slotIdx].load(std::memory_order_relaxed);
    switch (fmt) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) {
                slotsVst3_[slotIdx]->ReleaseEditorView();
                slotsVst3_[slotIdx]->Unload();
                slotsVst3_[slotIdx].reset();
            }
            break;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) {
                slotsVst2_[slotIdx]->Unload();
                slotsVst2_[slotIdx].reset();
            }
            break;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) {
                slotsClap_[slotIdx]->Unload();
                slotsClap_[slotIdx].reset();
            }
            break;
        case SlotFormat::Empty:
            break;
    }
    format_[slotIdx].store(SlotFormat::Empty, std::memory_order_release);
    return was ? 0u : 1u;
}

std::uint8_t PluginChain::SetSlotBypass(int slotIdx, bool bypass) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return 6;
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
    std::vector<ParamInfo> list;
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) list = slotsVst3_[slotIdx]->ListParams();
            break;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) list = slotsVst2_[slotIdx]->ListParams();
            break;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) list = slotsClap_[slotIdx]->ListParams();
            break;
        case SlotFormat::Empty:
            return 1;
    }
    if (list.empty()) {
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
    double v = std::nan("");
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) v = slotsVst3_[slotIdx]->SetParam(paramId, normalized);
            break;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) v = slotsVst2_[slotIdx]->SetParam(paramId, normalized);
            break;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) v = slotsClap_[slotIdx]->SetParam(paramId, normalized);
            break;
        case SlotFormat::Empty:
            return 1;
    }
    if (std::isnan(v)) return 7;
    outActual = v;
    return 0;
}

// ---------------------------------------------------------------------------
// Audio thread Process. Lock-free / alloc-free / syscall-free.
// ---------------------------------------------------------------------------

void PluginChain::Process(const float* in, float* out,
                          std::int32_t frames) noexcept {
    if (frames <= 0) return;
    if (frames > maxBlockSize_) frames = maxBlockSize_;

    const std::size_t bytes =
        static_cast<std::size_t>(frames) * sizeof(float);

    if (!masterEnabled_.load(std::memory_order_acquire)) {
        std::memcpy(out, in, bytes);
        return;
    }

    const float* cur     = in;
    float*       scratch[2] = { scratchA_.data(), scratchB_.data() };
    int          which   = 0;
    bool         touched = false;

    for (int i = 0; i < kMaxSlots; ++i) {
        if (!loaded_[i].load(std::memory_order_acquire)) continue;
        if (bypass_[i].load(std::memory_order_acquire)) continue;

        float* dst = scratch[which];
        bool ok = false;
        switch (format_[i].load(std::memory_order_acquire)) {
            case SlotFormat::Vst3:
                if (slotsVst3_[i]) ok = slotsVst3_[i]->Process(cur, dst, frames);
                break;
            case SlotFormat::Vst2:
                if (slotsVst2_[i]) ok = slotsVst2_[i]->Process(cur, dst, frames);
                break;
            case SlotFormat::Clap:
                if (slotsClap_[i]) ok = slotsClap_[i]->Process(cur, dst, frames);
                break;
            case SlotFormat::Empty:
                break;
        }
        if (!ok) continue;
        cur     = dst;
        which   = which ^ 1;
        touched = true;
    }

    if (touched) {
        std::memcpy(out, cur, bytes);
    } else {
        std::memcpy(out, in, bytes);
    }
}

// ---------------------------------------------------------------------------
// Editor-view facade — dispatches by slot format. VST3 returns its
// IEditController-driven IPlugView; VST2 / CLAP return their wrapper that
// presents the native editor through the same Steinberg::IPlugView shape.
// Nullptr means "no editor for this slot" (slot empty, or plugin doesn't
// expose an editor).
// ---------------------------------------------------------------------------

Steinberg::IPlugView* PluginChain::AcquireEditorView(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return nullptr;
    std::lock_guard<std::mutex> guard(chainMutex_);
    if (!loaded_[slotIdx].load(std::memory_order_acquire)) return nullptr;
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) return slotsVst3_[slotIdx]->AcquireEditorView();
            return nullptr;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) return slotsVst2_[slotIdx]->AcquireEditorView();
            return nullptr;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) return slotsClap_[slotIdx]->AcquireEditorView();
            return nullptr;
        case SlotFormat::Empty:
            return nullptr;
    }
    return nullptr;
}

void PluginChain::ReleaseEditorView(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return;
    std::lock_guard<std::mutex> guard(chainMutex_);
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst3:
            if (slotsVst3_[slotIdx]) slotsVst3_[slotIdx]->ReleaseEditorView();
            break;
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) slotsVst2_[slotIdx]->ReleaseEditorView();
            break;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) slotsClap_[slotIdx]->ReleaseEditorView();
            break;
        case SlotFormat::Empty:
            break;
    }
}

IEditorIdlePump* PluginChain::AcquireEditorIdlePump(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return nullptr;
    std::lock_guard<std::mutex> guard(chainMutex_);
    if (!loaded_[slotIdx].load(std::memory_order_acquire)) return nullptr;
    switch (format_[slotIdx].load(std::memory_order_relaxed)) {
        case SlotFormat::Vst2:
            if (slotsVst2_[slotIdx]) {
                // The wrapper has already been instantiated by the
                // matching AcquireEditorView call. AcquireEditorView
                // would lazily create it again here, but that's safe.
                auto* view = slotsVst2_[slotIdx]->AcquireEditorView();
                return static_cast<vst2::Vst2ViewWrapper*>(view);
            }
            return nullptr;
        case SlotFormat::Clap:
            if (slotsClap_[slotIdx]) {
                auto* view = slotsClap_[slotIdx]->AcquireEditorView();
                return static_cast<clap::ClapViewWrapper*>(view);
            }
            return nullptr;
        case SlotFormat::Vst3:
        case SlotFormat::Empty:
            return nullptr;
    }
    return nullptr;
}

}  // namespace zeus::plughost::vst3
