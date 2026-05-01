// vst2_plugin.cpp — Linux VST 2.4 plugin host implementation.
//
// Drives a Linux-native VST2 plugin through the standard host lifecycle.
// Mirrors PluginHost (VST3) for the parts of the contract that PluginChain
// depends on: Load/Unload, Process, SetParam, ListParams, IsLoaded,
// SetParamChangedCallback. No editor support in this iteration.
//
// Lifetime / threading:
//   - Load/Unload run on the control thread under impl_->controlMutex.
//   - Process runs on the audio thread; reads impl_->active with
//     memory_order_acquire so the swap to nullptr in Unload synchronises.
//   - SetParam / ListParams take controlMutex.
//   - audioMasterCallback is invoked from any thread the plugin chooses —
//     it does the bare minimum (return host info) and forwards
//     audioMasterAutomate to the registered ParamChangedCallback under
//     a separate mutex so it doesn't contend with controlMutex.

#include "vst2/vst2_plugin.h"
#include "vst2/vst2_view.h"
#include "aeffectx.h"   // third_party/vestige/aeffectx.h via include path

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <mutex>

namespace zeus::plughost::vst2 {

namespace {

// Entry-point names hosts try in order. "VSTPluginMain" is the modern
// symbol; "main_plugin" and "main" are legacy aliases still used by some
// older Linux VST2 plugins.
constexpr const char* kEntryPointNames[] = {
    "VSTPluginMain",
    "main_plugin",
    "main"
};

// Static thunk for audioMasterCallback. The plugin gets a single C
// function pointer; we route it back to the Vst2Plugin instance via
// AEffect::user (set by us right after VSTPluginMain returns).
intptr_t HostAudioMasterCallback(struct AEffect* effect,
                                 std::int32_t opcode,
                                 std::int32_t index,
                                 intptr_t value,
                                 void* ptr,
                                 float opt);

// Probe a candidate file: dlopen, look for any of the entry-point names,
// dlclose. Returns true if at least one entry symbol was found. We don't
// actually instantiate the plugin here — that happens during Load().
bool ProbeIsVst2(const std::string& path) {
    void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    bool found = false;
    for (const char* name : kEntryPointNames) {
        if (dlsym(h, name) != nullptr) {
            found = true;
            break;
        }
    }
    dlclose(h);
    return found;
}

}  // namespace

bool LooksLikeVst2(const std::string& path) {
    // Cheap path-extension sniff first so we don't dlopen a VST3 bundle
    // by accident. VST3 bundles end in `.vst3`, plain Linux VST2 plugins
    // are `.so` files (sometimes with a `vst` subdirectory in their
    // install path).
    if (path.size() < 4) return false;
    auto ends = [](const std::string& s, const char* suf) {
        std::size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    if (!ends(path, ".so")) return false;
    return ProbeIsVst2(path);
}

// ---------------------------------------------------------------------------
// Active plugin state — opaque heap object swapped through impl_->active.
// ---------------------------------------------------------------------------

struct ActiveVst2 {
    void*       dlHandle    = nullptr;
    AEffect*    effect      = nullptr;
    std::int32_t maxBlockSize = 0;
    double      sampleRate    = 0.0;
    vst3::LoadInfo info;
    // Pre-allocated stereo scratch buffers used when the plugin is
    // stereo-only. The chain feeds us mono; we duplicate to L+R going in
    // and average L+R going out so a stereo-only effect (LSP, ZAM stereo
    // builds) still processes the operator's mono mic correctly. Sized at
    // Load time to maxBlockSize so Process() never allocates.
    std::vector<float> scratchInL;
    std::vector<float> scratchInR;
    std::vector<float> scratchOutL;
    std::vector<float> scratchOutR;
};

struct Vst2Plugin::Impl {
    std::mutex                 controlMutex;
    std::atomic<ActiveVst2*>   active{nullptr};
    vst3::LoadInfo             currentInfo;

    // Param-changed callback. Same shape as PluginHost. Updated under
    // controlMutex; read on whatever thread the plugin called
    // audioMasterAutomate from. Reads use a snapshot copy so a torn
    // assignment (rare) just costs one missed event.
    ParamChangedCallback paramChangedCallback;
    std::mutex            callbackMutex;  // protects paramChangedCallback

    // Lazily-created editor wrapper. Lifetime is bounded by Acquire / Release
    // pairs from the chain; recreated on demand. Owned by this Impl, never
    // touched on the audio thread.
    std::unique_ptr<Vst2ViewWrapper> editorView;

    // Bound back to the static audioMasterCallback so it can find us via
    // AEffect::user. Set when we publish into `active`.
    static Impl* FromEffect(AEffect* e) {
        return e ? static_cast<Impl*>(e->user) : nullptr;
    }
};

namespace {

intptr_t HostAudioMasterCallback(struct AEffect* effect,
                                 std::int32_t opcode,
                                 std::int32_t /*index*/,
                                 intptr_t /*value*/,
                                 void* /*ptr*/,
                                 float /*opt*/) {
    switch (opcode) {
        case audioMasterVersion:
            return 2400;  // VST 2.4 host
        case audioMasterGetSampleRate: {
            auto* impl = Vst2Plugin::Impl::FromEffect(effect);
            if (!impl) return 48000;
            ActiveVst2* a = impl->active.load(std::memory_order_acquire);
            return a ? static_cast<intptr_t>(a->sampleRate) : 48000;
        }
        case audioMasterGetBlockSize: {
            auto* impl = Vst2Plugin::Impl::FromEffect(effect);
            if (!impl) return 256;
            ActiveVst2* a = impl->active.load(std::memory_order_acquire);
            return a ? a->maxBlockSize : 256;
        }
        case audioMasterGetCurrentProcessLevel:
            // 2 == "realtime process", what most plugins want to see when
            // their processReplacing is being called.
            return 2;
        case audioMasterGetVendorString: {
            // ptr should be a char[64]; fill it.
            // We can't safely write to ptr here because the callback's
            // ptr param isn't typed in the public spec. But many plugins
            // ignore the return when ptr is null anyway. Best-effort.
            return 0;
        }
        case audioMasterAutomate: {
            // Plugin reports a parameter change (typically from its own
            // editor or internal automation). Bridge to the registered
            // callback. `index` is the param index; `opt` is the value.
            auto* impl = Vst2Plugin::Impl::FromEffect(effect);
            if (impl == nullptr) return 0;
            Vst2Plugin::ParamChangedCallback cb;
            {
                std::lock_guard<std::mutex> lk(impl->callbackMutex);
                cb = impl->paramChangedCallback;
            }
            if (cb) {
                // Use a 1:1 mapping between our paramId and VST2 index;
                // the host-side ChainSlot.Parameters list uses the same
                // index as paramId for VST2 plugins.
                // (Cast-shenanigan: opcode signature uses `int32 index`
                //  in this branch, not the function's `index` arg — but
                //  VST2 audioMasterAutomate puts the param index in the
                //  function's `index` param. Re-fetch via the closure.)
                // Per spec: audioMasterAutomate(idx, value-as-float).
                // The function signature names the param `index`, hidden
                // by our switch local; recover by using the function arg.
            }
            return 0;
        }
        case audioMasterCanDo:
        case audioMasterIdle:
        default:
            return 0;
    }
}

// Dedicated callback that knows the actual function-arg names — used so
// the audioMasterAutomate branch can read the real `index` and `opt`.
intptr_t HostAudioMasterCallbackDispatch(struct AEffect* effect,
                                         std::int32_t opcode,
                                         std::int32_t index,
                                         intptr_t value,
                                         void* ptr,
                                         float opt) {
    if (opcode == audioMasterAutomate) {
        auto* impl = Vst2Plugin::Impl::FromEffect(effect);
        if (impl == nullptr) return 0;
        Vst2Plugin::ParamChangedCallback cb;
        {
            std::lock_guard<std::mutex> lk(impl->callbackMutex);
            cb = impl->paramChangedCallback;
        }
        if (cb) {
            cb(static_cast<std::uint32_t>(index),
               static_cast<double>(opt));
        }
        return 0;
    }
    return HostAudioMasterCallback(effect, opcode, index, value, ptr, opt);
}

// Build a ParamInfo from the plugin via dispatcher. VST2 doesn't expose
// default values or step counts, so we fill those in conservatively.
vst3::ParamInfo BuildParamInfo(AEffect* eff, std::int32_t idx) {
    vst3::ParamInfo info;
    info.id           = static_cast<std::uint32_t>(idx);
    info.defaultValue = 0.0;          // VST2 doesn't expose; placeholder
    info.stepCount    = 0;
    info.flags        = 0x02;          // bit1 = automatable

    char nameBuf[kVstMaxLabelLen + 1] = {0};
    eff->dispatcher(eff, effGetParamName, idx, 0, nameBuf, 0.0f);
    info.name = nameBuf;

    char unitsBuf[kVstMaxLabelLen + 1] = {0};
    eff->dispatcher(eff, effGetParamLabel, idx, 0, unitsBuf, 0.0f);
    info.units = unitsBuf;

    info.currentValue = static_cast<double>(eff->getParameter(eff, idx));
    return info;
}

void TearDownActive(ActiveVst2* a) noexcept {
    if (!a) return;
    if (a->effect) {
        // Reverse the activation order: stopProcess → mainsChanged(0) →
        // close → null out.
        a->effect->dispatcher(a->effect, effStopProcess, 0, 0, nullptr, 0.0f);
        a->effect->dispatcher(a->effect, effMainsChanged, 0, 0, nullptr, 0.0f);
        a->effect->dispatcher(a->effect, effClose, 0, 0, nullptr, 0.0f);
        a->effect = nullptr;
    }
    if (a->dlHandle) {
        dlclose(a->dlHandle);
        a->dlHandle = nullptr;
    }
}

}  // namespace

Vst2Plugin::Vst2Plugin() : impl_(new Impl()) {}

Vst2Plugin::~Vst2Plugin() {
    Unload();
    delete impl_;
}

vst3::LoadResult Vst2Plugin::Load(const std::string& path,
                                  double sampleRate,
                                  std::int32_t maxBlockSize) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);

    // dlopen the shared object.
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fclose(f);
            return vst3::LoadError{2, std::string("dlopen failed: ") + (dlerror() ? dlerror() : "(no error string)")};
        }
        return vst3::LoadError{1, std::string("plugin path not found: ") + path};
    }

    // Find the entry point.
    VSTPluginMainFn entry = nullptr;
    for (const char* name : kEntryPointNames) {
        entry = reinterpret_cast<VSTPluginMainFn>(dlsym(h, name));
        if (entry) break;
    }
    if (!entry) {
        dlclose(h);
        return vst3::LoadError{3, "no VST2 entry symbol (VSTPluginMain / main_plugin / main) found"};
    }

    // Instantiate.
    AEffect* eff = entry(&HostAudioMasterCallbackDispatch);
    if (!eff || eff->magic != kEffectMagic) {
        dlclose(h);
        return vst3::LoadError{4, "VSTPluginMain returned null or bad-magic AEffect"};
    }

    // Hook up reverse-pointer so audioMasterCallback can find us.
    eff->user = impl_;

    // Standard suspend/resume sequence for activation.
    eff->dispatcher(eff, effOpen, 0, 0, nullptr, 0.0f);
    eff->dispatcher(eff, effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate));
    eff->dispatcher(eff, effSetBlockSize, 0, maxBlockSize, nullptr, 0.0f);
    eff->dispatcher(eff, effMainsChanged, 0, 1, nullptr, 0.0f);   // resume
    eff->dispatcher(eff, effStartProcess, 0, 0, nullptr, 0.0f);

    // Pull plugin info via dispatcher opcodes.
    char effectName[kVstMaxEffectNameLen + 1] = {0};
    eff->dispatcher(eff, effGetEffectName, 0, 0, effectName, 0.0f);
    char vendor[kVstMaxVendorStrLen + 1]      = {0};
    eff->dispatcher(eff, effGetVendorString, 0, 0, vendor, 0.0f);
    intptr_t vendorVer = eff->dispatcher(eff, effGetVendorVersion, 0, 0, nullptr, 0.0f);
    char verBuf[32];
    std::snprintf(verBuf, sizeof(verBuf), "%lld",
                  static_cast<long long>(vendorVer));

    auto active = std::make_unique<ActiveVst2>();
    active->dlHandle     = h;
    active->effect       = eff;
    active->maxBlockSize = maxBlockSize;
    active->sampleRate   = sampleRate;
    active->info.name    = effectName[0] ? effectName : "(unknown)";
    active->info.vendor  = vendor[0] ? vendor : "(unknown)";
    active->info.version = verBuf;
    // Allocate stereo scratch up front for stereo plugins. Cheap (a few
    // KiB) and lets Process() avoid touching the heap on the audio thread.
    if (eff->numInputs == 2 || eff->numOutputs == 2) {
        const std::size_t cap = static_cast<std::size_t>(maxBlockSize);
        active->scratchInL .assign(cap, 0.0f);
        active->scratchInR .assign(cap, 0.0f);
        active->scratchOutL.assign(cap, 0.0f);
        active->scratchOutR.assign(cap, 0.0f);
    }

    vst3::LoadInfo info = active->info;

    // Drop any editor wrapper bound to the prior AEffect before swapping;
    // its dtor calls effEditClose against the live AEffect.
    impl_->editorView.reset();

    // Publish atomically. Tear down any prior plugin.
    ActiveVst2* prior = impl_->active.exchange(
        active.release(), std::memory_order_acq_rel);
    impl_->currentInfo = info;
    if (prior != nullptr) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        TearDownActive(prior);
        delete prior;
    }
    return info;
}

void Vst2Plugin::Unload() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    // Drop the editor wrapper before tearing down the AEffect — its
    // dtor calls effEditClose if still attached, which would crash on
    // a destroyed AEffect.
    impl_->editorView.reset();
    ActiveVst2* prior = impl_->active.exchange(nullptr, std::memory_order_acq_rel);
    if (!prior) return;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    TearDownActive(prior);
    delete prior;
    impl_->currentInfo = vst3::LoadInfo{};
}

bool Vst2Plugin::IsLoaded() const noexcept {
    return impl_->active.load(std::memory_order_acquire) != nullptr;
}

bool Vst2Plugin::Process(const float* in, float* out, std::int32_t frames) noexcept {
    ActiveVst2* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->effect) return false;
    if (frames <= 0 || frames > a->maxBlockSize) return false;
    AEffect* eff = a->effect;
    if (!(eff->flags & effFlagsCanReplacing) || !eff->processReplacing) {
        // Plugin only supports the legacy `process` (accumulating);
        // reject for safety — operator's chain expects replacing semantics.
        return false;
    }
    const std::int32_t nin  = eff->numInputs;
    const std::int32_t nout = eff->numOutputs;

    // Mono → mono: zero-copy fast path.
    if (nin == 1 && nout == 1) {
        float* mutableIn = const_cast<float*>(in);
        float* inBufs[1]  = { mutableIn };
        float* outBufs[1] = { out };
        eff->processReplacing(eff, inBufs, outBufs, frames);
        return true;
    }

    // Stereo plugins: duplicate mono → L+R going in, average L+R → mono on
    // output. Most LSP plugins and several DPF stereo builds report
    // numInputs/numOutputs = 2; without this they'd be a bypass slot for
    // operators on a mono mic chain.
    if (nin == 2 && nout == 2 &&
        a->scratchInL.size()  >= static_cast<std::size_t>(frames) &&
        a->scratchOutL.size() >= static_cast<std::size_t>(frames)) {
        float* inL  = a->scratchInL.data();
        float* inR  = a->scratchInR.data();
        float* outL = a->scratchOutL.data();
        float* outR = a->scratchOutR.data();
        for (std::int32_t i = 0; i < frames; ++i) {
            inL[i] = inR[i] = in[i];
        }
        float* inBufs[2]  = { inL, inR };
        float* outBufs[2] = { outL, outR };
        eff->processReplacing(eff, inBufs, outBufs, frames);
        for (std::int32_t i = 0; i < frames; ++i) {
            out[i] = 0.5f * (outL[i] + outR[i]);
        }
        return true;
    }

    // Anything else (mono→stereo, stereo→mono, multi-channel) is unusual
    // for an effect plugin; fall through to bypass so the operator hears
    // their unmodified mic instead of silence.
    return false;
}

vst3::LoadInfo Vst2Plugin::CurrentInfo() const {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    return impl_->currentInfo;
}

std::vector<vst3::ParamInfo> Vst2Plugin::ListParams() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    std::vector<vst3::ParamInfo> out;
    ActiveVst2* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->effect) return out;
    AEffect* eff = a->effect;
    int32_t n = eff->numParams;
    if (n <= 0) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        out.push_back(BuildParamInfo(eff, i));
    }
    return out;
}

double Vst2Plugin::SetParam(std::uint32_t paramId, double normalized) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActiveVst2* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->effect) return std::nan("");
    AEffect* eff = a->effect;
    if (paramId >= static_cast<std::uint32_t>(eff->numParams)) {
        return std::nan("");
    }
    if (normalized < 0.0) normalized = 0.0;
    if (normalized > 1.0) normalized = 1.0;
    eff->setParameter(eff, static_cast<int32_t>(paramId), static_cast<float>(normalized));
    // Read back; some plugins quantise.
    return static_cast<double>(eff->getParameter(eff, static_cast<int32_t>(paramId)));
}

void Vst2Plugin::SetParamChangedCallback(ParamChangedCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->callbackMutex);
    impl_->paramChangedCallback = std::move(cb);
}

Steinberg::IPlugView* Vst2Plugin::AcquireEditorView() noexcept {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActiveVst2* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->effect) return nullptr;
    if (!(a->effect->flags & effFlagsHasEditor)) return nullptr;
    if (!impl_->editorView) {
        impl_->editorView = std::make_unique<Vst2ViewWrapper>(a->effect);
    }
    return impl_->editorView.get();
}

void Vst2Plugin::ReleaseEditorView() noexcept {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    impl_->editorView.reset();
}

}  // namespace zeus::plughost::vst2
