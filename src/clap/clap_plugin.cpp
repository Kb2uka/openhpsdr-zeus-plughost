// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// clap_plugin.cpp — Linux CLAP plugin host implementation.
//
// Drives a CLAP plugin through the standard host lifecycle:
//   dlopen -> clap_entry.init -> get_factory -> create_plugin
//   -> plugin->init -> activate -> start_processing
//   <process loop>
//   -> stop_processing -> deactivate -> destroy -> clap_entry.deinit
//
// Lifetime / threading mirrors the VST3 PluginHost: Load/Unload/SetParam
// run on the control thread; Process on the audio thread; the plugin
// pointer is published into impl_->active via release-store and read with
// acquire-load.
//
// Parameters in CLAP flow through events. To set a parameter, we queue
// a CLAP_EVENT_PARAM_VALUE in the input-events list and the plugin reads
// it during process(). The plugin can also publish parameter changes back
// (from internal automation, when we eventually add an editor) by calling
// out_events->try_push() during process(); we read those post-process
// and dispatch through the registered ParamChangedCallback.

#include "clap/clap_plugin.h"
#include "clap/clap_view.h"

#include "clap/clap.h"
#include "clap/entry.h"
#include "clap/factory/plugin-factory.h"
#include "clap/host.h"
#include "clap/plugin.h"
#include "clap/plugin-features.h"
#include "clap/process.h"
#include "clap/audio-buffer.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zeus::plughost::clap {

namespace {

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

bool EndsWith(const std::string& s, const char* suf) {
    std::size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

}  // namespace

bool LooksLikeClap(const std::string& path) {
    return EndsWith(path, ".clap");
}

// ------------------------------------------------------------------
// ActiveClap — opaque heap object swapped through impl_->active.
// ------------------------------------------------------------------

struct ActiveClap {
    void*                                dlHandle      = nullptr;
    const ::clap_plugin_entry_t*         entry         = nullptr;
    bool                                 entryInit     = false;
    const ::clap_plugin_t*               plugin        = nullptr;
    bool                                 active        = false;
    bool                                 processing    = false;
    std::int32_t                         maxBlockSize  = 0;
    double                               sampleRate    = 0.0;
    std::int64_t                         steadyTime    = 0;

    // Cached extensions, looked up at activate time.
    const ::clap_plugin_params_t*        paramsExt     = nullptr;
    const ::clap_plugin_gui_t*           guiExt        = nullptr;
    const ::clap_plugin_timer_support_t* timerExt      = nullptr;

    // Channel topology of the plugin's first input/output port, queried
    // via clap.audio-ports at activate time. Process() picks the mono
    // fast path or a stereo upmix/downmix based on these. 0 means
    // "extension not implemented or no port" — Process() treats it like
    // a mismatch (skip slot, audio passes through).
    std::uint32_t                        inputChannels  = 0;
    std::uint32_t                        outputChannels = 0;
    // Stereo scratch buffers used when inputChannels==2 or
    // outputChannels==2. Allocated at Load time, sized to maxBlockSize so
    // Process() never touches the heap on the audio thread.
    std::vector<float>                   scratchInL;
    std::vector<float>                   scratchInR;
    std::vector<float>                   scratchOutL;
    std::vector<float>                   scratchOutR;

    vst3::LoadInfo                       info;
};

// ------------------------------------------------------------------
// PendingChange queue — host-side SetParam buffers events until the
// audio thread drains them into the plugin's input events list.
// ------------------------------------------------------------------

struct PendingClapChange {
    ::clap_id paramId;
    double    value;
};

struct ClapPlugin::Impl {
    std::mutex                       controlMutex;
    std::atomic<ActiveClap*>         active{nullptr};
    vst3::LoadInfo                   currentInfo;

    std::mutex                       pendingMutex;
    std::deque<PendingClapChange>    pendingChanges;
    static constexpr std::size_t     kPendingCap = 256;

    std::mutex                       callbackMutex;
    ParamChangedCallback             paramChangedCallback;

    // Lazily-created editor wrapper. Lifetime is bounded by Acquire / Release
    // pairs from the chain; recreated on demand. Owned by this Impl, never
    // touched on the audio thread.
    std::unique_ptr<ClapViewWrapper> editorView;

    // Host stubs presented to plugins. The clap_host_t routes extensions
    // through HostGetExtension; plugins that need clap.gui,
    // clap.timer-support, or clap.params get a minimal stub.
    ::clap_host_t                    host;
    ::clap_host_gui_t                hostGuiExt;
    ::clap_host_timer_support_t      hostTimerExt;
    ::clap_host_params_t             hostParamsExt;

    // Set by HostParamsRequestFlush when the plugin signals "I have queued
    // parameter changes — please flush so I can deliver them." The next
    // OnEditorIdleTick clears it and calls clap_plugin_params->flush(...)
    // outside the audio thread, so editor knob drags propagate even when
    // process() isn't running. Atomic so the request_flush callback can
    // fire from any thread.
    std::atomic<bool>                flushRequested{false};

    // Active timer registry. Plugins (typically pugl-based DPF UIs)
    // register periodic timers via the host_timer_support extension;
    // OnEditorIdleTick fires due timers from the GUI thread by calling
    // the plugin's clap_plugin_timer_support_t::on_timer. The plugin
    // call is `[main-thread]`-only per CLAP spec, but the GUI thread is
    // our de-facto main thread for editor work, matching how all other
    // hosts (Bitwig, Reaper) drive timer dispatch.
    struct TimerRecord {
        ::clap_id                                  id;
        std::uint32_t                              periodMs;
        std::chrono::steady_clock::time_point      lastFire;
    };
    std::mutex                       timersMutex;
    std::vector<TimerRecord>         timers;
    std::atomic<std::uint32_t>       nextTimerId{1};

    Impl();
};

namespace {

// Static host-side callback functions. They receive the clap_host_t* and
// route through host_data which we set to the ClapPlugin::Impl*.
const void* HostGetExtension(const ::clap_host_t* host, const char* ext) {
    if (host == nullptr || ext == nullptr) return nullptr;
    auto* impl = static_cast<ClapPlugin::Impl*>(host->host_data);
    if (impl == nullptr) return nullptr;
    if (std::strcmp(ext, CLAP_EXT_GUI) == 0) {
        return &impl->hostGuiExt;
    }
    if (std::strcmp(ext, CLAP_EXT_TIMER_SUPPORT) == 0) {
        return &impl->hostTimerExt;
    }
    if (std::strcmp(ext, CLAP_EXT_PARAMS) == 0) {
        return &impl->hostParamsExt;
    }
    return nullptr;
}
void HostRequestRestart(const ::clap_host_t* /*host*/) {}
void HostRequestProcess(const ::clap_host_t* /*host*/) {}
void HostRequestCallback(const ::clap_host_t* /*host*/) {}

// clap_host_gui callbacks. The plugin can ask the host to resize, show,
// hide its editor, or fire a "closed by user" notification. Our editor
// host (EditorWindow) drives all of those itself, so the request_*
// callbacks just answer "OK" and the closed/resize_hints notifications
// no-op. Returning false from request_resize would force the plugin to
// constrain itself; returning true lets it draw at the requested size.
void HostGuiResizeHintsChanged(const ::clap_host_t*) {}
bool HostGuiRequestResize(const ::clap_host_t*, uint32_t, uint32_t) { return true; }
bool HostGuiRequestShow(const ::clap_host_t*) { return true; }
bool HostGuiRequestHide(const ::clap_host_t*) { return true; }
void HostGuiClosed(const ::clap_host_t*, bool) {}

// clap_host_timer_support callbacks. DPF's CLAP wrapper asserts the
// extension is non-null but doesn't actually require timers to fire for
// the editor to attach. We hand out monotonically-increasing ids and
// remember nothing. If a plugin's editor is unresponsive without timer
// dispatch, we'll add a real periodic dispatcher integrated with the
// GUI thread's select() loop.
bool HostTimerRegister(const ::clap_host_t* host, uint32_t period_ms,
                       ::clap_id* timer_id) {
    if (host == nullptr || timer_id == nullptr) return false;
    auto* impl = static_cast<ClapPlugin::Impl*>(host->host_data);
    if (impl == nullptr) return false;
    // Clamp the period to keep CPU sane for plugins that ask for a sub-ms
    // period; CLAP spec only guarantees 30 Hz support anyway.
    if (period_ms < 16) period_ms = 16;
    ::clap_id newId = impl->nextTimerId.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(impl->timersMutex);
        impl->timers.push_back({newId, period_ms, std::chrono::steady_clock::now()});
    }
    *timer_id = newId;
    return true;
}
bool HostTimerUnregister(const ::clap_host_t* host, ::clap_id timer_id) {
    if (host == nullptr) return false;
    auto* impl = static_cast<ClapPlugin::Impl*>(host->host_data);
    if (impl == nullptr) return false;
    std::lock_guard<std::mutex> lk(impl->timersMutex);
    for (auto it = impl->timers.begin(); it != impl->timers.end(); ++it) {
        if (it->id == timer_id) {
            impl->timers.erase(it);
            return true;
        }
    }
    return false;
}

// clap_host_params callbacks. We only implement request_flush — the
// other entry points (rescan, clear) would matter for hosts that surface
// the plugin's parameter list dynamically; Zeus snapshots the list once
// per load and re-runs ListParams when needed, so the no-ops are safe.
//
// request_flush is the load-bearing one: when the plugin wants to push
// parameter changes outside of process() (typically because the operator
// dragged a knob in the editor while audio is paused), it calls this. We
// flip a flag and the next GUI-thread idle tick calls plugin_params->flush()
// to drain the changes through the same output-event path process() uses.
void HostParamsRescan(const ::clap_host_t*, ::clap_param_rescan_flags) {}
void HostParamsClear(const ::clap_host_t*, ::clap_id, ::clap_param_clear_flags) {}
void HostParamsRequestFlush(const ::clap_host_t* host) {
    if (host == nullptr) return;
    auto* impl = static_cast<ClapPlugin::Impl*>(host->host_data);
    if (impl == nullptr) return;
    impl->flushRequested.store(true, std::memory_order_release);
}

// Input-events context — drained queue passed to plugin->process().
struct InputEventsCtx {
    std::vector<::clap_event_param_value_t> events;
};

uint32_t InputEventsSize(const ::clap_input_events_t* list) {
    auto* ctx = static_cast<InputEventsCtx*>(list->ctx);
    return static_cast<uint32_t>(ctx->events.size());
}

const ::clap_event_header_t* InputEventsGet(const ::clap_input_events_t* list, uint32_t index) {
    auto* ctx = static_cast<InputEventsCtx*>(list->ctx);
    if (index >= ctx->events.size()) return nullptr;
    return &ctx->events[index].header;
}

// Output-events context — accumulator for plugin-driven param changes.
struct OutputEventsCtx {
    std::vector<::clap_event_param_value_t> paramValues;
};

bool OutputEventsTryPush(const ::clap_output_events_t* list,
                         const ::clap_event_header_t* event) {
    auto* ctx = static_cast<OutputEventsCtx*>(list->ctx);
    // We only care about CLAP_EVENT_PARAM_VALUE in the core space.
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        event->type     == CLAP_EVENT_PARAM_VALUE  &&
        event->size     >= sizeof(::clap_event_param_value_t)) {
        ctx->paramValues.push_back(
            *reinterpret_cast<const ::clap_event_param_value_t*>(event));
    }
    return true;
}

void TearDownActive(ActiveClap* a) noexcept {
    if (!a) return;
    if (a->plugin) {
        if (a->processing) {
            a->plugin->stop_processing(a->plugin);
            a->processing = false;
        }
        if (a->active) {
            a->plugin->deactivate(a->plugin);
            a->active = false;
        }
        a->plugin->destroy(a->plugin);
        a->plugin = nullptr;
    }
    if (a->entry && a->entryInit) {
        a->entry->deinit();
        a->entryInit = false;
    }
    a->entry = nullptr;
    if (a->dlHandle) {
        dlclose(a->dlHandle);
        a->dlHandle = nullptr;
    }
}

// Pick the first plugin in the factory whose features include
// "audio-effect". CLAP plugins can be instruments, effects, MIDI-only,
// etc.; the chain only handles effects.
std::string PickEffectPluginId(const ::clap_plugin_factory_t* factory,
                               vst3::LoadInfo& outInfo) {
    uint32_t n = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < n; ++i) {
        const ::clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, i);
        if (!desc) continue;
        bool isEffect = false;
        if (desc->features) {
            for (const char* const* f = desc->features; *f; ++f) {
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0) {
                    isEffect = true;
                    break;
                }
            }
        }
        if (!isEffect) continue;
        outInfo.name    = desc->name    ? desc->name    : "(unknown)";
        outInfo.vendor  = desc->vendor  ? desc->vendor  : "(unknown)";
        outInfo.version = desc->version ? desc->version : "";
        return desc->id ? std::string(desc->id) : std::string();
    }
    return std::string();
}

}  // namespace

ClapPlugin::Impl::Impl() {
    std::memset(&host, 0, sizeof(host));
    host.clap_version     = CLAP_VERSION;
    host.host_data        = this;
    host.name             = "Zeus";
    host.vendor           = "openhpsdr-zeus";
    host.url              = "https://github.com/brianbruff/openhpsdr-zeus";
    host.version          = "0.4.x";
    host.get_extension    = HostGetExtension;
    host.request_restart  = HostRequestRestart;
    host.request_process  = HostRequestProcess;
    host.request_callback = HostRequestCallback;

    std::memset(&hostGuiExt, 0, sizeof(hostGuiExt));
    hostGuiExt.resize_hints_changed = HostGuiResizeHintsChanged;
    hostGuiExt.request_resize       = HostGuiRequestResize;
    hostGuiExt.request_show         = HostGuiRequestShow;
    hostGuiExt.request_hide         = HostGuiRequestHide;
    hostGuiExt.closed               = HostGuiClosed;

    std::memset(&hostTimerExt, 0, sizeof(hostTimerExt));
    hostTimerExt.register_timer   = HostTimerRegister;
    hostTimerExt.unregister_timer = HostTimerUnregister;

    std::memset(&hostParamsExt, 0, sizeof(hostParamsExt));
    hostParamsExt.rescan        = HostParamsRescan;
    hostParamsExt.clear         = HostParamsClear;
    hostParamsExt.request_flush = HostParamsRequestFlush;
}

ClapPlugin::ClapPlugin() : impl_(new Impl()) {}

ClapPlugin::~ClapPlugin() {
    Unload();
    delete impl_;
}

vst3::LoadResult ClapPlugin::Load(const std::string& path,
                                  double sampleRate,
                                  std::int32_t maxBlockSize) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);

    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        return vst3::LoadError{1, std::string("dlopen failed: ")
                              + (dlerror() ? dlerror() : "(no error)")};
    }

    auto* entry = static_cast<const ::clap_plugin_entry_t*>(dlsym(h, "clap_entry"));
    if (!entry) {
        dlclose(h);
        return vst3::LoadError{2, "no clap_entry symbol — not a CLAP plugin"};
    }
    if (!entry->init || !entry->deinit || !entry->get_factory) {
        dlclose(h);
        return vst3::LoadError{2, "clap_entry has null required methods"};
    }
    if (!entry->init(path.c_str())) {
        dlclose(h);
        return vst3::LoadError{4, "clap_entry->init returned false"};
    }

    auto* factory = static_cast<const ::clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{3, "no clap.plugin-factory in module"};
    }

    vst3::LoadInfo info;
    std::string pluginId = PickEffectPluginId(factory, info);
    if (pluginId.empty()) {
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{3, "no audio-effect plugin in factory"};
    }

    const ::clap_plugin_t* plugin = factory->create_plugin(
        factory, &impl_->host, pluginId.c_str());
    if (!plugin) {
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{4, "factory->create_plugin returned null"};
    }
    if (!plugin->init(plugin)) {
        plugin->destroy(plugin);
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{4, "plugin->init returned false"};
    }
    if (!plugin->activate(plugin, sampleRate, 1, static_cast<uint32_t>(maxBlockSize))) {
        plugin->destroy(plugin);
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{4, "plugin->activate returned false"};
    }
    if (!plugin->start_processing(plugin)) {
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        entry->deinit();
        dlclose(h);
        return vst3::LoadError{4, "plugin->start_processing returned false"};
    }

    auto active = std::make_unique<ActiveClap>();
    active->dlHandle     = h;
    active->entry        = entry;
    active->entryInit    = true;
    active->plugin       = plugin;
    active->active       = true;
    active->processing   = true;
    active->maxBlockSize = maxBlockSize;
    active->sampleRate   = sampleRate;
    active->info         = info;
    active->paramsExt    = static_cast<const ::clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    active->guiExt       = static_cast<const ::clap_plugin_gui_t*>(
        plugin->get_extension(plugin, CLAP_EXT_GUI));
    active->timerExt     = static_cast<const ::clap_plugin_timer_support_t*>(
        plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));

    // Channel topology of the first input/output port. Used by Process()
    // to choose between the mono fast path and a stereo upmix/downmix.
    if (auto* portsExt = static_cast<const ::clap_plugin_audio_ports_t*>(
            plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS))) {
        ::clap_audio_port_info_t portInfo;
        std::memset(&portInfo, 0, sizeof(portInfo));
        if (portsExt->count != nullptr && portsExt->get != nullptr) {
            if (portsExt->count(plugin, /*is_input=*/true) > 0 &&
                portsExt->get(plugin, 0, /*is_input=*/true, &portInfo)) {
                active->inputChannels = portInfo.channel_count;
            }
            std::memset(&portInfo, 0, sizeof(portInfo));
            if (portsExt->count(plugin, /*is_input=*/false) > 0 &&
                portsExt->get(plugin, 0, /*is_input=*/false, &portInfo)) {
                active->outputChannels = portInfo.channel_count;
            }
        }
    }
    // Allocate stereo scratch up front for stereo plugins so Process()
    // doesn't touch the heap on the audio thread. CLAP wants planar
    // float32 with one pointer per channel, so we keep L and R in
    // separate buffers (no interleave/deinterleave needed).
    if (active->inputChannels == 2 || active->outputChannels == 2) {
        const std::size_t cap = static_cast<std::size_t>(maxBlockSize);
        active->scratchInL .assign(cap, 0.0f);
        active->scratchInR .assign(cap, 0.0f);
        active->scratchOutL.assign(cap, 0.0f);
        active->scratchOutR.assign(cap, 0.0f);
    }

    // Drop any editor wrapper bound to the prior plugin before swapping;
    // its dtor calls hide/destroy on the live plugin.
    impl_->editorView.reset();

    ActiveClap* prior = impl_->active.exchange(
        active.release(), std::memory_order_acq_rel);
    impl_->currentInfo = info;
    if (prior != nullptr) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        TearDownActive(prior);
        delete prior;
    }
    return info;
}

void ClapPlugin::Unload() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    // Drop the editor wrapper before tearing down the plugin — its dtor
    // calls hide/destroy on the live plugin instance.
    impl_->editorView.reset();
    // Drop any timers the prior plugin registered. Safe even if the
    // plugin already called unregister_timer for each on its way out;
    // either way the new plugin instance starts with a clean registry.
    {
        std::lock_guard<std::mutex> tlk(impl_->timersMutex);
        impl_->timers.clear();
    }
    ActiveClap* prior = impl_->active.exchange(nullptr, std::memory_order_acq_rel);
    if (!prior) return;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    TearDownActive(prior);
    delete prior;
    impl_->currentInfo = vst3::LoadInfo{};
}

bool ClapPlugin::IsLoaded() const noexcept {
    return impl_->active.load(std::memory_order_acquire) != nullptr;
}

bool ClapPlugin::Process(const float* in, float* out, std::int32_t frames) noexcept {
    ActiveClap* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->plugin || !a->processing) return false;
    if (frames <= 0 || frames > a->maxBlockSize) return false;
    // Anything other than mono-only or stereo-only is unusual for an
    // effect plugin; bail early so audio passes through unmodified
    // instead of triggering plugin asserts.
    const bool isMonoMono     = (a->inputChannels == 1 && a->outputChannels == 1);
    const bool isStereoStereo = (a->inputChannels == 2 && a->outputChannels == 2);
    if (!isMonoMono && !isStereoStereo) return false;
    if (isStereoStereo &&
        (a->scratchInL.size()  < static_cast<std::size_t>(frames) ||
         a->scratchOutL.size() < static_cast<std::size_t>(frames))) {
        return false;
    }

    // Drain pending param changes into a fresh input-events list.
    InputEventsCtx inCtx;
    {
        std::lock_guard<std::mutex> lk(impl_->pendingMutex);
        if (!impl_->pendingChanges.empty()) {
            inCtx.events.reserve(impl_->pendingChanges.size());
            for (const auto& pc : impl_->pendingChanges) {
                ::clap_event_param_value_t ev;
                std::memset(&ev, 0, sizeof(ev));
                ev.header.size     = sizeof(ev);
                ev.header.time     = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type     = CLAP_EVENT_PARAM_VALUE;
                ev.header.flags    = 0;
                ev.param_id        = pc.paramId;
                ev.cookie          = nullptr;
                ev.note_id         = -1;
                ev.port_index      = -1;
                ev.channel         = -1;
                ev.key             = -1;
                ev.value           = pc.value;
                inCtx.events.push_back(ev);
            }
            impl_->pendingChanges.clear();
        }
    }

    ::clap_input_events_t inEvents;
    inEvents.ctx  = &inCtx;
    inEvents.size = InputEventsSize;
    inEvents.get  = InputEventsGet;

    OutputEventsCtx outCtx;
    ::clap_output_events_t outEvents;
    outEvents.ctx      = &outCtx;
    outEvents.try_push = OutputEventsTryPush;

    // Build the audio buffers. Mono → use the caller's pointers directly.
    // Stereo → duplicate mono → L+R going in, average L+R → mono going out.
    float* monoIn  = const_cast<float*>(in);
    float* monoOut = out;
    float* inChans[2];
    float* outChans[2];
    ::clap_audio_buffer_t inBuf;
    ::clap_audio_buffer_t outBuf;
    inBuf.data64  = nullptr;
    inBuf.latency = 0;
    inBuf.constant_mask = 0;
    outBuf.data64  = nullptr;
    outBuf.latency = 0;
    outBuf.constant_mask = 0;

    if (isMonoMono) {
        inChans[0]  = monoIn;
        outChans[0] = monoOut;
        inBuf.data32         = inChans;
        inBuf.channel_count  = 1;
        outBuf.data32        = outChans;
        outBuf.channel_count = 1;
    } else {  // isStereoStereo
        float* inL  = a->scratchInL.data();
        float* inR  = a->scratchInR.data();
        float* outL = a->scratchOutL.data();
        float* outR = a->scratchOutR.data();
        for (std::int32_t i = 0; i < frames; ++i) {
            inL[i] = inR[i] = monoIn[i];
        }
        inChans[0]  = inL;  inChans[1]  = inR;
        outChans[0] = outL; outChans[1] = outR;
        inBuf.data32         = inChans;
        inBuf.channel_count  = 2;
        outBuf.data32        = outChans;
        outBuf.channel_count = 2;
    }

    ::clap_process_t proc;
    std::memset(&proc, 0, sizeof(proc));
    proc.steady_time         = a->steadyTime;
    proc.frames_count        = static_cast<uint32_t>(frames);
    proc.transport           = nullptr;
    proc.audio_inputs        = &inBuf;
    proc.audio_outputs       = &outBuf;
    proc.audio_inputs_count  = 1;
    proc.audio_outputs_count = 1;
    proc.in_events           = &inEvents;
    proc.out_events          = &outEvents;

    a->plugin->process(a->plugin, &proc);
    a->steadyTime += frames;

    if (isStereoStereo) {
        const float* outL = a->scratchOutL.data();
        const float* outR = a->scratchOutR.data();
        for (std::int32_t i = 0; i < frames; ++i) {
            monoOut[i] = 0.5f * (outL[i] + outR[i]);
        }
    }

    // Forward any plugin-driven parameter changes back through the
    // ParamChanged callback. Only fires if the plugin pushed values
    // during this block; common path is empty.
    if (!outCtx.paramValues.empty()) {
        ParamChangedCallback cb;
        {
            std::lock_guard<std::mutex> lk(impl_->callbackMutex);
            cb = impl_->paramChangedCallback;
        }
        if (cb) {
            for (const auto& ev : outCtx.paramValues) {
                cb(static_cast<std::uint32_t>(ev.param_id), ev.value);
            }
        }
    }

    return true;
}

vst3::LoadInfo ClapPlugin::CurrentInfo() const {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    return impl_->currentInfo;
}

std::vector<vst3::ParamInfo> ClapPlugin::ListParams() {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    std::vector<vst3::ParamInfo> out;
    ActiveClap* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->paramsExt || !a->plugin) return out;
    uint32_t n = a->paramsExt->count(a->plugin);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ::clap_param_info_t pi;
        std::memset(&pi, 0, sizeof(pi));
        if (!a->paramsExt->get_info(a->plugin, i, &pi)) continue;

        // CLAP exposes raw plain values (not normalised). To keep the
        // chain UI normalised-only, we map [min, max] -> [0, 1] when
        // building the ParamInfo. SetParam reverses the mapping.
        // We stash the min/max in an out-of-band sidecar table keyed on
        // paramId so SetParam can recover them — but to keep this first
        // cut simple, we send plain values straight through. The slot
        // UI sliders will need a refinement pass to handle plain ranges.

        double current = 0.0;
        a->paramsExt->get_value(a->plugin, pi.id, &current);

        vst3::ParamInfo info;
        info.id           = static_cast<std::uint32_t>(pi.id);
        info.name         = pi.name;
        info.units        = "";
        info.defaultValue = pi.default_value;
        info.currentValue = current;
        info.stepCount    = 0;
        info.flags        = (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) ? 0x02 : 0x00;
        out.push_back(std::move(info));
    }
    return out;
}

double ClapPlugin::SetParam(std::uint32_t paramId, double normalized) {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActiveClap* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->plugin) return std::nan("");

    // CLAP uses plain (un-normalised) values. Pass through verbatim for
    // now — the slot UI is feeding normalised, but for first iteration
    // we treat them as already-correct plain values. A range-aware
    // refinement pass is captured as a follow-up.
    {
        std::lock_guard<std::mutex> lk(impl_->pendingMutex);
        if (impl_->pendingChanges.size() >= Impl::kPendingCap) {
            impl_->pendingChanges.pop_front();
        }
        impl_->pendingChanges.push_back({static_cast<::clap_id>(paramId), normalized});
    }
    return normalized;
}

void ClapPlugin::SetParamChangedCallback(ParamChangedCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->callbackMutex);
    impl_->paramChangedCallback = std::move(cb);
}

void ClapPlugin::OnEditorIdleTick() {
    ActiveClap* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->plugin) return;

    // Drain any work the plugin queued via clap_host->request_callback.
    // Safe to call without a prior request — plugins handle that.
    if (a->plugin->on_main_thread != nullptr) {
        a->plugin->on_main_thread(a->plugin);
    }

    // Drain a parameter flush if the plugin requested one. CLAP plugins
    // (DPF / pugl-based or otherwise) deliver editor knob changes through
    // params.flush() when audio isn't running through process(). Without
    // this, an operator could turn knobs all day while RX-only and the
    // host would never see the changes — state save would lose them.
    if (a->paramsExt != nullptr && a->paramsExt->flush != nullptr &&
        impl_->flushRequested.exchange(false, std::memory_order_acq_rel)) {
        // Empty input events; we're flushing OUT only.
        InputEventsCtx inCtx;
        ::clap_input_events_t inEvents;
        inEvents.ctx  = &inCtx;
        inEvents.size = InputEventsSize;
        inEvents.get  = InputEventsGet;

        OutputEventsCtx outCtx;
        ::clap_output_events_t outEvents;
        outEvents.ctx      = &outCtx;
        outEvents.try_push = OutputEventsTryPush;

        a->paramsExt->flush(a->plugin, &inEvents, &outEvents);

        if (!outCtx.paramValues.empty()) {
            ParamChangedCallback cb;
            {
                std::lock_guard<std::mutex> lk(impl_->callbackMutex);
                cb = impl_->paramChangedCallback;
            }
            if (cb) {
                for (const auto& ev : outCtx.paramValues) {
                    cb(static_cast<std::uint32_t>(ev.param_id), ev.value);
                }
            }
        }
    }

    // Fire any timers whose period has elapsed. Snapshot the registry
    // under the mutex so a plugin re-entering register_timer from inside
    // on_timer doesn't deadlock or invalidate iterators.
    if (a->timerExt == nullptr || a->timerExt->on_timer == nullptr) return;
    std::vector<::clap_id> due;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(impl_->timersMutex);
        for (auto& t : impl_->timers) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t.lastFire).count();
            if (static_cast<std::uint32_t>(elapsed) >= t.periodMs) {
                due.push_back(t.id);
                t.lastFire = now;
            }
        }
    }
    for (::clap_id id : due) {
        a->timerExt->on_timer(a->plugin, id);
    }
}

Steinberg::IPlugView* ClapPlugin::AcquireEditorView() noexcept {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    ActiveClap* a = impl_->active.load(std::memory_order_acquire);
    if (!a || !a->plugin || !a->guiExt) return nullptr;
    if (!impl_->editorView) {
        impl_->editorView = std::make_unique<ClapViewWrapper>(
            a->plugin, a->guiExt, this);
    }
    return impl_->editorView.get();
}

void ClapPlugin::ReleaseEditorView() noexcept {
    std::lock_guard<std::mutex> guard(impl_->controlMutex);
    impl_->editorView.reset();
}

}  // namespace zeus::plughost::clap