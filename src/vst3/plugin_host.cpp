// plugin_host.cpp — Phase 2 single-slot VST 3 plugin host implementation.
//
// Threading model:
//
//   - Load() / Unload() / CurrentInfo() run on the control thread.
//   - Process() / IsLoaded() run on the audio thread.
//   - The two communicate via std::atomic<Active*>. Load publishes a new
//     Active* with memory_order_release; the audio thread reads it with
//     memory_order_acquire. Unload swaps the slot to null, then drops
//     the previous Active* AFTER an atomic_thread_fence so the audio
//     thread cannot still be inside Process() when destruction starts.
//
// VST 3 SDK lifecycle:
//
//   1. VST3::Hosting::Module::create(path, errorString)  — dlopen + read factory.
//   2. Walk classInfos(); pick the first kVstAudioEffectClass entry.
//   3. factory.createInstance<IComponent>(uid).
//   4. component->initialize(hostContext)   — IHostApplication ptr.
//   5. queryInterface IAudioProcessor.
//   6. component->activateBus(kAudio, kInput,  0, true);
//      component->activateBus(kAudio, kOutput, 0, true);
//   7. processor->setBusArrangements(mono, 1, mono, 1)   — best-effort.
//   8. processor->setupProcessing(ProcessSetup{kRealtime, kSample32,
//                                              maxBlockSize, sampleRate}).
//   9. component->setActive(true).
//   10. processor->setProcessing(true).
//
// Reverse on Unload(). All teardown failures are swallowed — the plugin
// is going away; we just need to free our refcounts.

#include "vst3/plugin_host.h"
#include "vst3/sdk_includes.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace zeus::plughost::vst3 {

namespace {

using Steinberg::FUnknown;
using Steinberg::IPtr;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::tresult;
using Steinberg::TUID;
using Steinberg::FUID;
using Steinberg::Vst::HostApplication;
using Steinberg::Vst::IAudioProcessor;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IConnectionPoint;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::ParameterInfo;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::ProcessSetup;
using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::SpeakerArrangement;
using Steinberg::Vst::SymbolicSampleSizes;
using Steinberg::Vst::ProcessModes;

// One fully-initialized + activated plugin. Allocated on the control
// thread, published into PluginHost::Impl::active_ via release-store, and
// freed by the control thread after taking the slot away with an
// acquire-load + matching release-store.
struct ActivePlugin {
    VST3::Hosting::Module::Ptr module;
    IPtr<IComponent>           component;
    IPtr<IAudioProcessor>      processor;
    IPtr<IEditController>      controller;     // may be null
    bool                       singleComponent = false;  // controller == component
    IPtr<IConnectionPoint>     componentCp;
    IPtr<IConnectionPoint>     controllerCp;
    LoadInfo                   info;
    std::int32_t               maxBlockSize;
    double                     sampleRate;
};

// Translate an SDK tresult into the wire-spec error code on the
// activate/setup path.
LoadError MakeActivateFailed(const char* stage, tresult code) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "VST3 activate failed at %s (tresult=0x%08x)",
        stage, static_cast<unsigned>(code));
    return LoadError{4, std::string(buf)};
}

// Bring up an ActivePlugin from a freshly-created Module + chosen class.
// Returns ownership on success, nullptr on failure (and writes the error
// out-param). Caller is responsible for calling TearDown() on a null
// return — the helper unwinds anything it allocated before failing.
std::unique_ptr<ActivePlugin> InstantiateAndActivate(
    VST3::Hosting::Module::Ptr            module,
    const VST3::Hosting::ClassInfo&       classInfo,
    HostApplication*                      hostContext,
    double                                sampleRate,
    std::int32_t                          maxBlockSize,
    LoadError&                            outError) {

    auto active = std::make_unique<ActivePlugin>();
    active->module       = std::move(module);
    active->maxBlockSize = maxBlockSize;
    active->sampleRate   = sampleRate;
    active->info.name    = classInfo.name();
    active->info.vendor  = classInfo.vendor();
    if (active->info.vendor.empty()) {
        active->info.vendor = active->module->getFactory().info().vendor();
    }
    active->info.version = classInfo.version();

    // 1. Create IComponent via the factory's typed createInstance helper.
    //    Returns null on any factory failure (kNoInterface / kInvalidArg /
    //    null pointer). Caller has already validated that the chosen
    //    classInfo has category "Audio Module Class", so a null here
    //    means activate-failed at instantiation, not a missing interface.
    active->component = active->module->getFactory()
        .createInstance<IComponent>(classInfo.ID());
    if (!active->component) {
        outError = LoadError{4,
            "factory.createInstance<IComponent>(classInfo.ID()) returned null"};
        return nullptr;
    }

    // 2. initialize(hostContext) — must succeed before any other call.
    tresult tr = active->component->initialize(
        static_cast<FUnknown*>(hostContext));
    if (tr != kResultOk) {
        // Plugin may have partially initialized; terminate to be safe.
        active->component->terminate();
        active->component = nullptr;
        outError = MakeActivateFailed("component->initialize", tr);
        return nullptr;
    }

    // 3. queryInterface IAudioProcessor.
    {
        IAudioProcessor* rawProc = nullptr;
        if (active->component->queryInterface(
                IAudioProcessor::iid,
                reinterpret_cast<void**>(&rawProc)) != kResultOk
            || rawProc == nullptr) {
            active->component->terminate();
            active->component = nullptr;
            outError = LoadError{4, "queryInterface(IAudioProcessor) failed"};
            return nullptr;
        }
        active->processor = Steinberg::owned(rawProc);
    }

    // 3a. Try to obtain the IEditController. Two shapes:
    //     - Single-component plugin: IComponent IS the controller. We
    //       just queryInterface on the component pointer.
    //     - Two-class plugin: IComponent::getControllerClassId returns
    //       a separate CID; we instantiate that class via the factory
    //       and call initialize on it.
    //     Failure to obtain a controller is NOT fatal — the audio path
    //     still works; ListParams() returns empty.
    {
        IEditController* rawCtrl = nullptr;
        if (active->component->queryInterface(
                IEditController::iid,
                reinterpret_cast<void**>(&rawCtrl)) == kResultOk
            && rawCtrl != nullptr) {
            // queryInterface returns an AddRef'd pointer; wrap it directly.
            active->controller      = Steinberg::owned(rawCtrl);
            active->singleComponent = true;
        } else {
            TUID controllerCID;
            std::memset(controllerCID, 0, sizeof(controllerCID));
            if (active->component->getControllerClassId(controllerCID) == kResultOk) {
                FUID cid = FUID::fromTUID(controllerCID);
                if (cid.isValid()) {
                    auto& factory = active->module->getFactory();
                    // PluginFactory::createInstance<T> returns IPtr<T>
                    // (already owned). Assigning to active->controller
                    // copies the IPtr, keeping the refcount alive.
                    active->controller = factory.createInstance<IEditController>(
                        VST3::UID::fromTUID(controllerCID));
                    if (active->controller) {
                        // Initialise the controller; some plugins fail
                        // here (e.g. need a specific host context) — in
                        // that case drop the controller and continue.
                        if (active->controller->initialize(
                                static_cast<FUnknown*>(hostContext))
                            != kResultOk) {
                            active->controller = nullptr;
                        } else {
                            active->singleComponent = false;
                        }
                    }
                }
            }
        }
    }

    // 3b. If we have a separate controller, connect it to the component
    //     via IConnectionPoint so they can exchange messages. Many
    //     plugins ignore this; some require it (state sync). Best-effort.
    if (active->controller && !active->singleComponent) {
        IConnectionPoint* rawCompCp = nullptr;
        IConnectionPoint* rawCtrlCp = nullptr;
        if (active->component->queryInterface(
                IConnectionPoint::iid,
                reinterpret_cast<void**>(&rawCompCp)) == kResultOk
            && rawCompCp != nullptr) {
            active->componentCp = Steinberg::owned(rawCompCp);
        }
        if (active->controller->queryInterface(
                IConnectionPoint::iid,
                reinterpret_cast<void**>(&rawCtrlCp)) == kResultOk
            && rawCtrlCp != nullptr) {
            active->controllerCp = Steinberg::owned(rawCtrlCp);
        }
        if (active->componentCp && active->controllerCp) {
            active->componentCp->connect(active->controllerCp);
            active->controllerCp->connect(active->componentCp);
        }
    }

    // 4. Best-effort: tell the plugin we want mono in / mono out. Many
    //    plugins enforce a fixed channel count regardless of this; the
    //    test contract is "no crash", so a non-OK return is informational.
    {
        SpeakerArrangement mono = Steinberg::Vst::SpeakerArr::kMono;
        active->processor->setBusArrangements(&mono, 1, &mono, 1);
    }

    // 5. Activate exactly one input + one output audio bus. Some plugins
    //    only expose 0 buses on a side; we still try, and tolerate
    //    kInvalidArgument as "fine, that side has no bus".
    using Steinberg::Vst::kAudio;
    using Steinberg::Vst::kInput;
    using Steinberg::Vst::kOutput;
    active->component->activateBus(kAudio, kInput,  0, true);
    active->component->activateBus(kAudio, kOutput, 0, true);

    // 6. setupProcessing with our wire geometry.
    ProcessSetup setup{};
    setup.processMode        = static_cast<Steinberg::int32>(ProcessModes::kRealtime);
    setup.symbolicSampleSize = static_cast<Steinberg::int32>(SymbolicSampleSizes::kSample32);
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate         = sampleRate;
    tr = active->processor->setupProcessing(setup);
    if (tr != kResultOk) {
        active->component->setActive(false);
        active->component->terminate();
        active->processor = nullptr;
        active->component = nullptr;
        outError = MakeActivateFailed("processor->setupProcessing", tr);
        return nullptr;
    }

    // 7. setActive + setProcessing.
    tr = active->component->setActive(true);
    if (tr != kResultOk && tr != kResultTrue) {
        active->component->terminate();
        active->processor = nullptr;
        active->component = nullptr;
        outError = MakeActivateFailed("component->setActive(true)", tr);
        return nullptr;
    }
    tr = active->processor->setProcessing(true);
    if (tr != kResultOk && tr != kResultTrue) {
        active->component->setActive(false);
        active->component->terminate();
        active->processor = nullptr;
        active->component = nullptr;
        outError = MakeActivateFailed("processor->setProcessing(true)", tr);
        return nullptr;
    }

    return active;
}

void TearDownActive(ActivePlugin* a) noexcept {
    if (a == nullptr) return;
    if (a->processor) {
        a->processor->setProcessing(false);
    }
    if (a->component) {
        a->component->setActive(false);
        // Bus deactivation is courtesy — most plugins are happy without.
        using Steinberg::Vst::kAudio;
        using Steinberg::Vst::kInput;
        using Steinberg::Vst::kOutput;
        a->component->activateBus(kAudio, kInput,  0, false);
        a->component->activateBus(kAudio, kOutput, 0, false);
    }
    // Disconnect the controller<->component message bus before terminate.
    if (a->componentCp && a->controllerCp) {
        a->componentCp->disconnect(a->controllerCp);
        a->controllerCp->disconnect(a->componentCp);
    }
    a->componentCp  = nullptr;
    a->controllerCp = nullptr;
    if (a->controller && !a->singleComponent) {
        a->controller->terminate();
    }
    a->controller = nullptr;
    if (a->component) {
        a->component->terminate();
    }
    a->processor = nullptr;
    a->component = nullptr;
    a->module    = nullptr;
}

bool IsAudioEffect(const VST3::Hosting::ClassInfo& info) {
    // Steinberg's category string for audio effects is exactly "Audio
    // Module Class" — see pluginterfaces/vst/ivstaudioprocessor.h
    // (#define kVstAudioEffectClass "Audio Module Class").
    return info.category() == "Audio Module Class";
}

}  // namespace

struct PluginHost::Impl {
    // Long-lived host application; the SDK's HostApplication helper
    // covers IHostApplication + IAttributeList factory so plugins that
    // need either at initialize() time get them.
    std::unique_ptr<HostApplication> host;

    // Active-plugin slot. Audio thread reads with acquire; control
    // thread publishes with release. The control thread owns destruction
    // of any prior contents.
    std::atomic<ActivePlugin*> active{nullptr};

    // Serializes Load/Unload across multiple control-thread callers (we
    // do not expect concurrent Load calls in Phase 2, but the wire
    // contract permits LoadPlugin while a previous one is loaded — we
    // unload then load).
    std::mutex controlMutex;

    // Snapshot of the most recent successful load info, returned by
    // CurrentInfo(). Updated under controlMutex.
    LoadInfo currentInfo;

    Impl() {
        host = std::make_unique<HostApplication>();
    }
};

PluginHost::PluginHost() : impl_(new Impl()) {}

PluginHost::~PluginHost() {
    Unload();
    delete impl_;
}

LoadResult PluginHost::Load(const std::string& path,
                            double sampleRate,
                            std::int32_t maxBlockSize) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);

    // 1. Module::create — handles file existence + bundle structure.
    std::string errorString;
    auto module = VST3::Hosting::Module::create(path, errorString);
    if (!module) {
        // Distinguish "file/dir not found" from "wrong format". The
        // Module::create error string varies by platform; we look at
        // the path directly to decide.
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f != nullptr) {
            std::fclose(f);
            return LoadError{2, "VST3::Hosting::Module::create failed: " + errorString};
        }
        // Could be a bundle directory; tolerate that path.
        return LoadError{1, "plugin path not found or not a valid VST3 module: "
                            + path + " (" + errorString + ")"};
    }

    // 2. Walk class infos for the first kVstAudioEffectClass. classInfos()
    //    returns a vector by value, so we copy the matching entry into a
    //    local — taking a pointer into the returned temporary would be
    //    a use-after-free as soon as this scope ends.
    VST3::Hosting::ClassInfo chosen;
    bool found = false;
    {
        const auto infos = module->getFactory().classInfos();
        for (const auto& info : infos) {
            if (IsAudioEffect(info)) {
                chosen = info;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return LoadError{3, "module has no kVstAudioEffectClass entries"};
    }

    // 3. Inform the factory of our host context so plugins can call back.
    module->getFactory().setHostContext(
        static_cast<FUnknown*>(impl_->host.get()));

    // 4. Build + activate the new plugin.
    LoadError activateError{0, {}};
    auto fresh = InstantiateAndActivate(
        std::move(module), chosen, impl_->host.get(),
        sampleRate, maxBlockSize, activateError);
    if (!fresh) {
        return activateError;
    }
    LoadInfo info = fresh->info;

    // 5. Swap the active slot. Take the prior pointer with acquire so we
    //    are guaranteed to see whatever the audio thread last published.
    ActivePlugin* prior = impl_->active.exchange(
        fresh.release(), std::memory_order_acq_rel);
    impl_->currentInfo = info;

    // 6. Tear down the old plugin AFTER swap. The audio thread's next
    //    acquire-load on impl_->active will see the new pointer; a
    //    fence after the swap guarantees no in-flight Process call on
    //    the old pointer can outlive this point on weakly-ordered hosts
    //    (x86_64 is strong; arm64 needs the fence).
    if (prior != nullptr) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        TearDownActive(prior);
        delete prior;
    }
    return info;
}

void PluginHost::Unload() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActivePlugin* prior = impl_->active.exchange(nullptr, std::memory_order_acq_rel);
    if (prior == nullptr) return;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    TearDownActive(prior);
    delete prior;
    impl_->currentInfo = LoadInfo{};
}

bool PluginHost::IsLoaded() const noexcept {
    return impl_->active.load(std::memory_order_acquire) != nullptr;
}

LoadInfo PluginHost::CurrentInfo() const {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    return impl_->currentInfo;
}

bool PluginHost::Process(const float* in, float* out, std::int32_t frames) noexcept {
    ActivePlugin* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr) {
        return false;
    }
    if (frames <= 0 || frames > a->maxBlockSize) {
        // Overrun — caller's contract said frames <= maxBlockSize. Bail
        // safely; passthrough loop will memcpy and continue.
        return false;
    }

    // Cast away constness: ProcessData uses float**, but the plugin
    // contract for input buffers is read-only (well-behaved plugins do
    // not write through the input pointer). The test fixture re-uses
    // the input array each block, so this is safe for our use.
    float* mutableIn = const_cast<float*>(in);

    AudioBusBuffers inputs;
    inputs.numChannels      = 1;
    inputs.silenceFlags     = 0;
    inputs.channelBuffers32 = &mutableIn;

    AudioBusBuffers outputs;
    outputs.numChannels      = 1;
    outputs.silenceFlags     = 0;
    outputs.channelBuffers32 = &out;

    ProcessData data;
    data.processMode         = static_cast<Steinberg::int32>(ProcessModes::kRealtime);
    data.symbolicSampleSize  = static_cast<Steinberg::int32>(SymbolicSampleSizes::kSample32);
    data.numSamples          = frames;
    data.numInputs           = 1;
    data.numOutputs          = 1;
    data.inputs              = &inputs;
    data.outputs             = &outputs;
    data.inputParameterChanges  = nullptr;
    data.outputParameterChanges = nullptr;
    data.inputEvents            = nullptr;
    data.outputEvents           = nullptr;
    data.processContext         = nullptr;

    tresult tr = a->processor->process(data);
    if (tr != kResultOk && tr != kResultTrue) {
        // Plugin reported failure — keep the existing samples in `out`
        // intact (the caller pre-fills with the input via memcpy in the
        // bypass path) and signal "not processed" so the passthrough
        // loop knows. We do NOT unload — Phase 2 contract is "audio
        // came back, no crash"; one bad block is not grounds to evict
        // the plugin.
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parameter introspection (Phase 3a). Both ListParams and SetParam run on
// the control thread under controlMutex; they read the active-plugin slot
// directly because the slot is only mutated from the same thread we run on.
// ---------------------------------------------------------------------------

std::vector<ParamInfo> PluginHost::ListParams() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    std::vector<ParamInfo> out;
    ActivePlugin* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr || !a->controller) {
        return out;
    }
    const Steinberg::int32 count = a->controller->getParameterCount();
    if (count <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    for (Steinberg::int32 i = 0; i < count; ++i) {
        ParameterInfo pi;
        std::memset(&pi, 0, sizeof(pi));
        if (a->controller->getParameterInfo(i, pi) != kResultOk) {
            continue;
        }
        ParamInfo info;
        info.id           = static_cast<std::uint32_t>(pi.id);
        info.name         = Steinberg::Vst::StringConvert::convert(pi.title);
        info.units        = Steinberg::Vst::StringConvert::convert(pi.units);
        info.defaultValue = pi.defaultNormalizedValue;
        // currentValue: read from the controller. May fail silently for
        // some plugins; the default is a safe fallback.
        info.currentValue = a->controller->getParamNormalized(pi.id);
        info.stepCount    = static_cast<std::int32_t>(pi.stepCount);

        std::uint8_t flags = 0;
        if ((pi.flags & ParameterInfo::kIsReadOnly)  != 0) flags |= 0x01;
        if ((pi.flags & ParameterInfo::kCanAutomate) != 0) flags |= 0x02;
        if ((pi.flags & ParameterInfo::kIsHidden)    != 0) flags |= 0x04;
        if ((pi.flags & ParameterInfo::kIsList)      != 0) flags |= 0x08;
        info.flags = flags;

        out.push_back(std::move(info));
    }
    return out;
}

double PluginHost::SetParam(std::uint32_t paramId, double normalized) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActivePlugin* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr || !a->controller) {
        return std::nan("");
    }
    if (normalized < 0.0) normalized = 0.0;
    if (normalized > 1.0) normalized = 1.0;
    a->controller->setParamNormalized(static_cast<ParamID>(paramId), normalized);
    // Read back what the plugin actually accepted (post quantise / clamp).
    return a->controller->getParamNormalized(static_cast<ParamID>(paramId));
}

}  // namespace zeus::plughost::vst3
