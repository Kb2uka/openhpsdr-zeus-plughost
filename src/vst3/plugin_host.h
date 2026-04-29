// plugin_host.h — single-slot VST 3 plugin host.
//
// Phase 2/3a contract:
//
//   - One PluginHost owns at most one loaded plugin instance.
//   - Load() is called from the control thread (slow first-load is OK).
//   - Process() is called from the audio thread; it must be
//     allocation-free, lock-free, and syscall-free.
//   - The audio thread reads the loaded-plugin pointer through a
//     std::atomic with memory_order_acquire; control-thread writes use
//     memory_order_release. When IsLoaded() returns false, Process() is
//     a no-op (the audio loop falls back to its existing memcpy path).
//   - ListParams() / SetParam() walk the IEditController; both run on
//     the control thread and may take the controlMutex (which serialises
//     against Load/Unload).
//
// Phase 2 simplifications: mono in / mono out, sample size 32-bit float,
// fixed buffer geometry (caller passes sampleRate / maxBlockSize at Load
// time), no presets, no GUI.
//
// Phase 3a addition: parameter introspection via IEditController. Many
// plugins use a single-component class for both IComponent and
// IEditController; others expose a separate controller class via
// IComponent::getControllerClassId(). Both shapes are handled inside
// PluginHost::Load().
//
// Cross-platform shape: Module loading flows through the SDK's
// VST3::Hosting::Module facade; the platform-specific module_*.cpp file
// is compiled directly into the sidecar (selected via deps.cmake).

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Steinberg {
class IPlugView;
}

namespace zeus::plughost::vst3 {

// Snapshot of one VST3 parameter, copied from the plugin's
// IEditController via getParameterInfo + getParamNormalized. All fields
// are owned by value (no SDK refcounts leak through this struct).
struct ParamInfo {
    std::uint32_t id;            // VST3 ParamID, opaque to host
    std::string   name;          // UTF-8, from ParameterInfo::title
    std::string   units;         // UTF-8, from ParameterInfo::units
    double        defaultValue;  // normalized 0..1
    double        currentValue;  // normalized 0..1 (snapshot)
    std::int32_t  stepCount;     // 0 = continuous, >0 = stepped
    std::uint8_t  flags;         // bit0 readonly, bit1 automatable,
                                 // bit2 hidden, bit3 list
};

// Successful load — name / vendor / version come from the module's class
// info plus the factory info.
struct LoadInfo {
    std::string name;
    std::string vendor;
    std::string version;
};

// Failed load. Status codes mirror the wire-spec LoadPluginResult enum:
//   0 = ok (LoadInfo path); never used here.
//   1 = file-not-found
//   2 = not-a-vst3 (Module::create returned null)
//   3 = no-audio-effect-class (factory has no kVstAudioEffectClass entry)
//   4 = activate-failed (initialize / setActive / setProcessing failed)
//   5 = other (catch-all)
struct LoadError {
    std::uint8_t status;
    std::string  message;
};

using LoadResult = std::variant<LoadInfo, LoadError>;

// Owned by main.cpp; passed by reference into the passthrough loop and
// the control reader. PluginHost is move-/copy-disabled because it owns
// raw COM-style refcounts on the plugin component.
class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&)            = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    PluginHost(PluginHost&&)                 = delete;
    PluginHost& operator=(PluginHost&&)      = delete;

    // Load a VST3 bundle directory or single-file plugin from `path`.
    // If a plugin is already loaded, it is unloaded first. Idempotent on
    // re-load with the same path.
    //
    // Called from the control thread only.
    LoadResult Load(const std::string& path,
                    double sampleRate,
                    std::int32_t maxBlockSize);

    // Tear down the currently-loaded plugin (no-op if none). Called from
    // the control thread only.
    void Unload();

    // True between a successful Load() and the matching Unload(). Safe
    // to call from the audio thread.
    bool IsLoaded() const noexcept;

    // Process one block. `in` and `out` are mono planar pointers,
    // `frames` <= the maxBlockSize passed to Load(). When IsLoaded() is
    // false this is a no-op (caller's pre-existing memcpy path runs
    // instead). Called from the audio thread only.
    //
    // Returns true if the plugin actually processed the block, false if
    // bypassed (no plugin loaded or transient state during a swap).
    bool Process(const float* in, float* out, std::int32_t frames) noexcept;

    // Optional: snapshot of the most recent load info for diagnostics.
    // Returned by value to avoid races with concurrent Load/Unload.
    LoadInfo CurrentInfo() const;

    // Phase 3a: parameter introspection. Returns an empty list if no
    // plugin is loaded, no controller is available, or the controller
    // reports zero parameters. Called from the control thread only.
    std::vector<ParamInfo> ListParams();

    // Phase 3a: set one parameter, normalized 0..1. Returns the value
    // the plugin reports back via getParamNormalized after the set
    // (some plugins quantise / clamp). Returns NaN on error (no plugin
    // loaded, no controller, paramId not found). Called from the
    // control thread only.
    //
    // Threading note: VST3 strictly recommends parameter changes be
    // delivered through ProcessData::inputParameterChanges on the audio
    // thread. We bypass that and call setParamNormalized directly from
    // the control thread, which most simple hosts do for non-realtime
    // parameter tweaks. The plugin's controller must be reentrant with
    // respect to its audio-thread IComponent — that is the standard
    // VST3 contract for the controller object.
    double SetParam(std::uint32_t paramId, double normalized);

    // Phase 3 GUI: acquire the plugin's IPlugView for the editor. Returns
    // nullptr if no plugin loaded, no controller, or the controller's
    // createView returned null. The caller (typically PluginChain via
    // GuiThread) owns one extra refcount and MUST call ReleaseEditorView
    // before unloading the plugin.
    //
    // The returned pointer is the same instance across repeated calls
    // until ReleaseEditorView is called — Steinberg's contract is "one
    // editor view per plugin, lifetime managed by the host". We follow
    // that: AcquireEditorView creates on first call and reuses.
    Steinberg::IPlugView* AcquireEditorView();

    // Drop the editor view (idempotent).
    void ReleaseEditorView();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace zeus::plughost::vst3
