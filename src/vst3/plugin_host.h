// plugin_host.h — single-slot VST 3 plugin host.
//
// Phase 2 contract:
//
//   - One PluginHost owns at most one loaded plugin instance.
//   - Load() is called from the control thread (slow first-load is OK).
//   - Process() is called from the audio thread; it must be
//     allocation-free, lock-free, and syscall-free.
//   - The audio thread reads the loaded-plugin pointer through a
//     std::atomic with memory_order_acquire; control-thread writes use
//     memory_order_release. When IsLoaded() returns false, Process() is
//     a no-op (the audio loop falls back to its existing memcpy path).
//
// Phase 2 simplifications: mono in / mono out, sample size 32-bit float,
// fixed buffer geometry (caller passes sampleRate / maxBlockSize at Load
// time), no parameter automation, no presets, no GUI.
//
// Cross-platform shape: Module loading flows through the SDK's
// VST3::Hosting::Module facade; the platform-specific module_*.cpp file
// is compiled directly into the sidecar (selected via deps.cmake).

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <variant>

namespace zeus::plughost::vst3 {

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

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace zeus::plughost::vst3
