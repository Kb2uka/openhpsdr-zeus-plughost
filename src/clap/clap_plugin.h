// clap_plugin.h — Linux CLAP plugin host (Phase 6).
//
// Loads a `.clap` shared object (CLAP plugin), drives it through the CLAP
// lifecycle, and exposes the same surface as PluginHost (the VST3 host)
// so PluginChain can dispatch by format with shared boilerplate.
//
// Editor (clap_plugin_gui extension) is wired through a ClapViewWrapper
// that presents the CLAP GUI as a Steinberg::IPlugView, letting the
// GuiThread / EditorWindow infrastructure built for VST3 host it without
// a per-format code path.
//
// Threading shape mirrors PluginHost: Load/Unload/SetParam on the control
// thread, Process on the audio thread, atomic-active-pointer publication
// pattern.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "vst3/plugin_host.h"  // reuse LoadResult / LoadInfo / LoadError / ParamInfo

namespace Steinberg { class IPlugView; }

namespace zeus::plughost::clap {

class ClapViewWrapper;

// Cheap path-extension probe — does the file at `path` look like a CLAP
// plugin? CLAP install paths are `~/.clap`, `/usr/lib/clap`, plus any
// directory named in CLAP_PATH. Plugins end in `.clap`. We don't dlopen
// here; that happens in Load().
bool LooksLikeClap(const std::string& path);

class ClapPlugin {
public:
    ClapPlugin();
    ~ClapPlugin();

    ClapPlugin(const ClapPlugin&)            = delete;
    ClapPlugin& operator=(const ClapPlugin&) = delete;
    ClapPlugin(ClapPlugin&&)                 = delete;
    ClapPlugin& operator=(ClapPlugin&&)      = delete;

    // CLAP load lifecycle:
    //   1. dlopen(path)
    //   2. dlsym("clap_entry") -> clap_plugin_entry_t*
    //   3. clap_entry->init(path)
    //   4. clap_entry->get_factory(CLAP_PLUGIN_FACTORY_ID)
    //   5. factory->create_plugin(host, plugin_id)
    //   6. plugin->init()
    //   7. plugin->activate(sample_rate, 1, max_block_size)
    //   8. plugin->start_processing()
    // Failure unwinds the prior steps.
    vst3::LoadResult Load(const std::string& path,
                          double               sampleRate,
                          std::int32_t         maxBlockSize);

    void Unload();
    bool IsLoaded() const noexcept;

    // Mono-in / mono-out through plugin->process(). The CLAP audio buffer
    // contract takes channel-count x frames; we pass numChannels=1. When
    // the plugin's audio-port topology requires stereo or more, this
    // returns false (caller falls back to bypass memcpy).
    bool Process(const float* in, float* out, std::int32_t frames) noexcept;

    vst3::LoadInfo CurrentInfo() const;

    // Parameter introspection via the clap_plugin_params extension.
    // Returns empty if the plugin doesn't implement params (rare for
    // effects). Each ParamInfo carries the plugin's stable clap_id as
    // the `id` field — the host can pass that back through SetParam.
    std::vector<vst3::ParamInfo> ListParams();

    // SetParam queues a CLAP_EVENT_PARAM_VALUE for the next process()
    // call. CLAP plugins expect parameter updates to flow through the
    // event list, not direct setters; we maintain a small thread-safe
    // queue that the audio thread drains. Returns the value the plugin
    // will apply (post normalisation), or NaN on error.
    double SetParam(std::uint32_t paramId, double normalized);

    // Editor: lazily creates a ClapViewWrapper around the loaded plugin's
    // clap.gui extension and returns it as an IPlugView so the GuiThread
    // can host it through the same EditorWindow path it uses for VST3.
    // Returns nullptr if no plugin is loaded or the plugin doesn't expose
    // the clap.gui extension. The pointer is owned by this ClapPlugin;
    // pair every Acquire with a Release before Unload.
    Steinberg::IPlugView* AcquireEditorView() noexcept;
    void                  ReleaseEditorView() noexcept;

    // Plugin-driven parameter changes (from internal automation or, when
    // we eventually add an editor, from knob drags) flow back as
    // CLAP_EVENT_PARAM_VALUE entries in the OUTPUT event list during
    // process(). We read them post-process and dispatch to this callback,
    // mirroring the VST3 IComponentHandler::performEdit bridge.
    using ParamChangedCallback = vst3::PluginHost::ParamChangedCallback;
    void SetParamChangedCallback(ParamChangedCallback cb);

    // Called by ClapViewWrapper::Idle from the GUI thread on each editor
    // tick. Pumps clap_plugin->on_main_thread (so plugins waiting on
    // request_callback get serviced) and fires any registered timers
    // whose period has elapsed (DPF / pugl-based plugins drive their
    // editor redraw entirely from the timer-support extension).
    void OnEditorIdleTick();

    // Forward-declared public so the static C-callback in clap_plugin.cpp
    // (HostGetExtension) can recover the Impl* from clap_host_t::host_data
    // and reach the cached host-extension stubs (clap.gui in particular).
    // The definition lives in the cpp; nothing leaks beyond the type name.
    struct Impl;

private:
    Impl* impl_;
};

}  // namespace zeus::plughost::clap
