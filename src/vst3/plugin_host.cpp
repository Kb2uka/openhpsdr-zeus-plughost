// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
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

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
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
using Steinberg::Vst::IComponentHandler;
using Steinberg::Vst::IConnectionPoint;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::ParameterInfo;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::ProcessSetup;
using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::SpeakerArrangement;
using Steinberg::Vst::SymbolicSampleSizes;
using Steinberg::Vst::ProcessModes;

// Minimal IComponentHandler — captures performEdit and forwards to the
// host-supplied callback. beginEdit / endEdit are no-ops (they bracket the
// performEdit calls during a knob drag; we only care about the value
// transitions). restartComponent is also a no-op for now — it's the
// "params reloaded, host should re-read everything" notification, which
// our control-thread ListParams refresh handles on demand.
//
// Lifetime: lives inside ActivePlugin, owned by IPtr. The plugin's
// controller holds a refcount to it; we drop our reference on Unload after
// the controller is torn down.
class HostComponentHandler : public IComponentHandler {
public:
    // Callback for the audio-side pending-changes queue. performEdit
    // pushes (paramId, value) here so the next process() call delivers
    // them via inputParameterChanges to two-class plugins like ZamVerb
    // whose audio processor only learns about edits through that channel.
    using AudioQueuePush = std::function<void(
        std::uint32_t paramId, double normalizedValue)>;

    HostComponentHandler(PluginHost::ParamChangedCallback* cb,
                         AudioQueuePush                    audioQueuePush)
        : cb_(cb), audioQueuePush_(std::move(audioQueuePush)) {
        // Spin up the drain thread immediately. The plugin's editor calls
        // performEdit on the GUI thread; if we did the socket I/O there
        // any contention on the control-send mutex (or a full kernel send
        // buffer) would block the editor's repaint loop. Hard freeze, no
        // X-button response. The drain thread takes all that risk away
        // from the GUI thread.
        drainThread_ = std::thread([this]() { DrainLoop(); });
    }

    virtual ~HostComponentHandler() {
        // Stop the drain thread before any other teardown so it can't
        // deref cb_ during destruction. Notify wakes a possibly-sleeping
        // drain; the thread joins after it finishes any in-flight cb_ call.
        // (FUnknown's dtor is non-virtual by COM convention; we make ours
        // virtual so `delete this` from release() invokes the right one.)
        stopRequested_.store(true, std::memory_order_release);
        queueCv_.notify_all();
        if (drainThread_.joinable()) {
            drainThread_.join();
        }
    }

    // Coalesce window per paramId. Many plugins fire performEdit at the
    // editor's repaint rate (60–1000 Hz); each one in Wave 7's first cut
    // turned into a control-pipe frame, saturating the .NET dispatcher
    // and back-pressuring the editor's UI thread. With this filter, a
    // fast knob drag generates at most ~100 frames/sec/param while still
    // catching every distinct value (the time gate) and never losing the
    // final resting value (last-write wins on the trailing fire after
    // the gate window). Persistence ScheduleSave is debounced 250 ms so
    // there's no operator-perceivable cost.
    static constexpr std::int64_t kCoalesceMinNs = 10'000'000;  // 10 ms

    // FUnknown contract.
    Steinberg::tresult PLUGIN_API queryInterface(
            const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef()  override { return ++refCount_; }
    Steinberg::uint32 PLUGIN_API release() override {
        Steinberg::uint32 r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    // IComponentHandler contract.
    Steinberg::tresult PLUGIN_API beginEdit(ParamID /*id*/) override {
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(
            ParamID id, ParamValue valueNormalized) override {
        // Coalesce: if the same paramId fired within the gate window with
        // a value that didn't change meaningfully, swallow it inline. This
        // is just a hint to keep the queue small; the queue itself is
        // already bounded so the drain thread can keep up. Important: do
        // NOT call cb_ here — that touches the control socket and would
        // freeze the editor's GUI thread if the socket back-pressures.
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lk(coalesceMutex_);
            auto it = lastFires_.find(id);
            if (it != lastFires_.end()) {
                const auto deltaNs = nowNs - it->second.nsTime;
                const auto deltaVal = std::fabs(valueNormalized - it->second.value);
                if (deltaNs < kCoalesceMinNs && deltaVal < 0.001) {
                    it->second.value = valueNormalized;
                    return kResultOk;
                }
                it->second.nsTime = nowNs;
                it->second.value = valueNormalized;
            } else {
                lastFires_.emplace(id, FireRecord{nowNs, valueNormalized});
            }
        }

        // Push to the audio-side pending queue FIRST so the audio thread
        // sees the change on its next process() call. Two-class plugins
        // (ZamVerb etc.) only learn about edits through inputParameterChanges
        // — without this push, the audio stays at initial defaults no matter
        // how the operator moves the knob. Single-component plugins (ZamEQ2)
        // also see the change here, harmlessly. The lambda holds a raw
        // pointer to the ActivePlugin's mutex+deque; lifetime is bounded
        // by the handler's IPtr (destroyed before the ActivePlugin itself).
        if (audioQueuePush_) {
            audioQueuePush_(static_cast<std::uint32_t>(id),
                            static_cast<double>(valueNormalized));
        }

        // Enqueue and signal the IPC drain thread for host-side persistence.
        // The drain does the socket I/O. With a bounded queue we drop the
        // OLDEST entry on overflow (last-write-wins semantics; the operator's
        // final knob position is preserved). Caps memory in pathological
        // floods even if the drain can't keep up momentarily.
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            if (queue_.size() >= kQueueCap) {
                queue_.pop_front();
                ++droppedCount_;
            }
            queue_.push_back(QueuedFire{id, valueNormalized});
        }
        queueCv_.notify_one();
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(ParamID /*id*/) override {
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 /*flags*/) override {
        // A more thorough host would re-pull parameter info / state on
        // certain flags; for our persistence-bridge purposes the next
        // ListParams call refreshes the cache.
        return kResultOk;
    }

private:
    struct FireRecord {
        std::int64_t nsTime;
        double       value;
    };

    struct QueuedFire {
        std::uint32_t paramId;
        double        value;
    };

    // Drain thread: blocks on queueCv_, dispatches fires through cb_. All
    // socket I/O happens here; the GUI thread only enqueues. On stop we
    // dispatch remaining queued fires (best-effort persistence) before
    // joining so the operator's final knob positions land in the host's
    // ChainSlot.Parameters cache → LiteDB save.
    void DrainLoop() {
        for (;;) {
            QueuedFire fire;
            {
                std::unique_lock<std::mutex> lk(queueMutex_);
                queueCv_.wait(lk, [this]() {
                    return stopRequested_.load(std::memory_order_acquire)
                        || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stopRequested_.load(std::memory_order_acquire)) {
                        return;
                    }
                    continue;
                }
                fire = queue_.front();
                queue_.pop_front();
            }
            // Dispatch outside the lock so a slow socket write doesn't
            // block enqueuers (the GUI thread). cb_ may be null if the
            // PluginHost hasn't installed a callback yet — drop silently.
            if (cb_ && *cb_) {
                (*cb_)(fire.paramId, fire.value);
            }
        }
    }

    // Bounded queue cap. At ~100 fires/sec/param post-coalesce and a typical
    // ~10 simultaneous-knob limit, peak inflight is ~1000/sec. Cap at 2048
    // gives ~2s of buffer at the worst sustained rate before dropping —
    // plenty for the C# dispatcher to catch up. Drops are last-write-wins.
    static constexpr std::size_t kQueueCap = 2048;

    std::atomic<Steinberg::uint32>            refCount_{1};
    // Pointer to the PluginHost's installed callback. We don't own the
    // function; the PluginHost holds it. Null-checked at every fire.
    PluginHost::ParamChangedCallback*         cb_;
    // Audio-side queue push. Captures a pointer to the ActivePlugin's
    // pending-changes deque + mutex; called inline from performEdit (after
    // coalesce) on the GUI thread. The lock is brief — contention is at
    // most one process() block.
    AudioQueuePush                            audioQueuePush_;

    // Coalesce filter — first line of defence; reduces queue traffic.
    std::mutex                                coalesceMutex_;
    std::unordered_map<std::uint32_t, FireRecord> lastFires_;

    // Drain queue — second line of defence; isolates GUI thread from I/O.
    std::mutex                                queueMutex_;
    std::condition_variable                   queueCv_;
    std::deque<QueuedFire>                    queue_;
    std::atomic<bool>                         stopRequested_{false};
    std::thread                               drainThread_;
    // Diagnostic — number of queue overflow drops. Reset never; useful
    // for `wdsp.psSeed`-style health logging if we wire it later.
    std::uint64_t                             droppedCount_{0};
};

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
    IPtr<Steinberg::IPlugView> editorView;     // null until AcquireEditorView
    IPtr<HostComponentHandler> componentHandler;  // null until set on controller
    // Wave 7 — re-used across Process() calls so we don't allocate on the
    // audio thread. clearQueue() resets them at the start of each block.
    Steinberg::Vst::ParameterChanges inputParameterChanges;
    Steinberg::Vst::ParameterChanges outputParameterChanges;
    // Monotonic project-time sample counter. Plugins that animate on
    // transport time use this; we just increment by frames each block so
    // it advances in step with audio. Wraps after ~6 million years at 48k.
    Steinberg::int64           projectTimeSamples = 0;

    // Wave 7 — pending parameter changes from the editor (performEdit) and
    // host control-thread SetParam. The audio thread try_locks at the start
    // of each Process() and drains into inputParameterChanges so the audio
    // processor sees them on the next block. Two-class plugins (separate
    // IComponent + IEditController, e.g. ZamVerb) need this — without it
    // the controller cache updates but the audio processor never sees the
    // change, so the audio stays at initial defaults regardless of UI
    // movement. Single-component plugins like ZamEQ2 work either way
    // (component IS the controller) but routing all changes through here
    // is the canonical VST3 path and harmless for them too.
    struct PendingChange {
        Steinberg::Vst::ParamID    id;
        Steinberg::Vst::ParamValue value;
    };
    std::mutex                pendingMutex;
    std::deque<PendingChange> pendingChanges;
    static constexpr std::size_t kPendingCap = 256;

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
    // Drop the editor view first if any — by the time we reach Unload,
    // the GUI thread should have closed any window, but if the unload
    // is racing with a still-attached editor we just drop our refcount
    // and the plugin handles the dangling embed. PluginChain forces
    // editor close before calling Unload, so this is the safety net.
    if (a->editorView) {
        a->editorView = nullptr;
    }
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
    // Detach our IComponentHandler before tearing the controller down so
    // any final performEdit the plugin tries to fire during terminate
    // doesn't reach into our dangling state.
    if (a->controller && a->componentHandler) {
        a->controller->setComponentHandler(nullptr);
    }
    a->componentHandler = nullptr;
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

    // Wave 7 — host-supplied callback for IComponentHandler::performEdit.
    // Lives on the PluginHost (not the per-plugin ActivePlugin) so it
    // survives Load/Unload cycles. The HostComponentHandler installed on
    // each loaded controller holds a pointer to this slot and reads it on
    // every performEdit. Updated only via SetParamChangedCallback under
    // controlMutex, so there's no concurrent-write race; reads from the
    // handler happen on whatever thread the plugin called performEdit on
    // (no lock — std::function copy is not threadsafe, but updates only
    // happen at deterministic points outside the audio thread).
    PluginHost::ParamChangedCallback paramChangedCallback;

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

    // 4a. Wave 7 — install our IComponentHandler on the controller so the
    //     plugin can publish edit-driven parameter changes back to us. The
    //     handler holds a pointer to impl_->paramChangedCallback (a
    //     std::function slot owned by Impl); it dereferences on every fire.
    //     Best-effort — controllers without setComponentHandler support
    //     are rare but legal; meters from those plugins won't round-trip
    //     and edit-driven persistence won't see their knob changes, but
    //     the audio path is unaffected.
    //
    //     The audio-queue lambda captures fresh.get() so the audio thread
    //     can drain pending changes into inputParameterChanges before
    //     each process() call. Lifetime: the handler is owned by an IPtr
    //     in fresh->componentHandler, destroyed before fresh itself, so
    //     the captured pointer is always valid while the handler can fire.
    if (fresh->controller) {
        ActivePlugin* activePtr = fresh.get();
        auto audioPush = [activePtr](std::uint32_t paramId, double value) {
            std::lock_guard<std::mutex> lk(activePtr->pendingMutex);
            if (activePtr->pendingChanges.size() >= ActivePlugin::kPendingCap) {
                activePtr->pendingChanges.pop_front();
            }
            activePtr->pendingChanges.push_back({paramId, value});
        };
        fresh->componentHandler = Steinberg::owned(
            new HostComponentHandler(&impl_->paramChangedCallback, audioPush));
        fresh->controller->setComponentHandler(fresh->componentHandler.get());
    }

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

void PluginHost::SetParamChangedCallback(ParamChangedCallback cb) {
    // Update under the control mutex so the assignment is serialized vs
    // Load (which calls setComponentHandler with a pointer to this slot).
    // The handler reads via a std::function copy on the calling thread —
    // we accept the small race window where a performEdit fires during
    // assignment; std::function assignment from non-empty to non-empty is
    // not atomic but a torn read just means one missed event, which is
    // acceptable for a debounced-save persistence pathway.
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    impl_->paramChangedCallback = std::move(cb);
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

    // Wave 7 — populate a minimal ProcessContext so plugins that gate
    // metering / animation on transport state see "playing" with a valid
    // sample rate. Per VST3 spec the host must supply at least sampleRate
    // and (when kPlaying is set) projectTimeSamples. systemTime is marked
    // valid because some plugins use it for envelope reseeds. Stack-
    // allocated each call — ProcessContext is ~140 bytes, trivial.
    Steinberg::Vst::ProcessContext ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.state = static_cast<Steinberg::uint32>(
        Steinberg::Vst::ProcessContext::kPlaying |
        Steinberg::Vst::ProcessContext::kSystemTimeValid);
    ctx.sampleRate         = a->sampleRate;
    ctx.projectTimeSamples = a->projectTimeSamples;
    a->projectTimeSamples += frames;

    ProcessData data;
    data.processMode         = static_cast<Steinberg::int32>(ProcessModes::kRealtime);
    data.symbolicSampleSize  = static_cast<Steinberg::int32>(SymbolicSampleSizes::kSample32);
    data.numSamples          = frames;
    data.numInputs           = 1;
    data.numOutputs          = 1;
    data.inputs              = &inputs;
    data.outputs             = &outputs;
    // Pass real ParameterChanges objects so plugins that emit meter values
    // through outputParameterChanges (and plugins that expect non-null
    // input change lists) don't deref nullptr. Both objects live on the
    // ActivePlugin so they're allocated once per plugin, not per block.
    a->inputParameterChanges.clearQueue();
    a->outputParameterChanges.clearQueue();

    // Drain the pending parameter-change queue (populated by the editor's
    // performEdit and host-side SetParam). For two-class plugins like
    // ZamVerb, the audio processor only learns about parameter updates
    // through this channel — without the drain, knob movement updates the
    // controller's cache but the audio stays at initial defaults. We
    // try_lock to keep this real-time-safe; if a writer is mid-push, the
    // changes wait one block (21 ms at 48 kHz, imperceptible). Each change
    // becomes a 1-point queue at sampleOffset=0 so the plugin sees it
    // applied at the start of the block.
    if (a->pendingMutex.try_lock()) {
        if (!a->pendingChanges.empty()) {
            for (const auto& pc : a->pendingChanges) {
                Steinberg::int32 paramIndex = 0;
                auto* queue = a->inputParameterChanges.addParameterData(
                    pc.id, paramIndex);
                if (queue != nullptr) {
                    Steinberg::int32 pointIndex = 0;
                    queue->addPoint(0, pc.value, pointIndex);
                }
            }
            a->pendingChanges.clear();
        }
        a->pendingMutex.unlock();
    }
    data.inputParameterChanges  = &a->inputParameterChanges;
    data.outputParameterChanges = &a->outputParameterChanges;
    data.inputEvents            = nullptr;
    data.outputEvents           = nullptr;
    data.processContext         = &ctx;

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
    // Push to the audio-side pending queue so two-class plugins (ZamVerb
    // etc.) see the change at the next process() block. setParamNormalized
    // alone updates the controller's cache; for plugins where the audio
    // processor is a separate object, the canonical VST3 path is via
    // inputParameterChanges. Single-component plugins (ZamEQ2) get the
    // change either way — push here too for consistency.
    {
        std::lock_guard<std::mutex> lk(a->pendingMutex);
        if (a->pendingChanges.size() >= ActivePlugin::kPendingCap) {
            a->pendingChanges.pop_front();
        }
        a->pendingChanges.push_back({static_cast<ParamID>(paramId), normalized});
    }
    // Read back what the plugin actually accepted (post quantise / clamp).
    return a->controller->getParamNormalized(static_cast<ParamID>(paramId));
}

// ---------------------------------------------------------------------------
// Phase 3 GUI: editor view acquisition.
// ---------------------------------------------------------------------------

Steinberg::IPlugView* PluginHost::AcquireEditorView() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActivePlugin* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr || !a->controller) {
        return nullptr;
    }
    if (!a->editorView) {
        // controller->createView returns an already-AddRef'd raw pointer.
        Steinberg::IPlugView* raw = a->controller->createView(
            Steinberg::Vst::ViewType::kEditor);
        if (raw == nullptr) {
            return nullptr;
        }
        a->editorView = Steinberg::owned(raw);
    }
    return a->editorView.get();
}

void PluginHost::ReleaseEditorView() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActivePlugin* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr) return;
    a->editorView = nullptr;
}

}  // namespace zeus::plughost::vst3