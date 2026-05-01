// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// vst2_plugin.h — Linux VST 2.4 plugin host (Phase 5a / Vestige).
//
// Loads a Linux-native VST2 plugin (.so with VSTPluginMain symbol), drives
// it through the standard suspend/resume/setSampleRate/setBlockSize/process
// lifecycle, and exposes the same shape as PluginHost (the VST3 host) so
// PluginChain can dispatch by format with minimal boilerplate.
//
// Editor (effEditOpen / effEditGetRect / effEditIdle) is wired through a
// Vst2ViewWrapper that presents the VST2 editor as a Steinberg::IPlugView,
// letting the GuiThread / EditorWindow infrastructure built for VST3 host
// it without a per-format code path.
//
// Threading shape mirrors the VST3 host: Load/Unload/SetParam on the
// control thread; Process on the audio thread; both serialised via the
// same atomic-active-pointer pattern that PluginHost uses.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "vst3/plugin_host.h"  // reuse LoadResult / LoadInfo / LoadError / ParamInfo

namespace Steinberg { class IPlugView; }

namespace zeus::plughost::vst2 {

class Vst2ViewWrapper;

// One-line probe: does the file at `path` look like a Linux VST2 plugin?
// Returns true if `path` ends in `.so` AND the binary exports the
// `VSTPluginMain` (or legacy `main`) symbol when dlopened. Side-effect-free
// for the host other than the dlopen+dlclose round-trip.
bool LooksLikeVst2(const std::string& path);

class Vst2Plugin {
public:
    Vst2Plugin();
    ~Vst2Plugin();

    Vst2Plugin(const Vst2Plugin&)            = delete;
    Vst2Plugin& operator=(const Vst2Plugin&) = delete;
    Vst2Plugin(Vst2Plugin&&)                 = delete;
    Vst2Plugin& operator=(Vst2Plugin&&)      = delete;

    // Same contract as PluginHost::Load. Maps the load lifecycle as:
    //   1. dlopen(path)
    //   2. dlsym("VSTPluginMain") OR "main_plugin" OR "main"
    //   3. VSTPluginMain(audioMasterCallback) -> AEffect*
    //   4. dispatcher(effOpen)
    //   5. dispatcher(effSetSampleRate, ..., sampleRate)
    //   6. dispatcher(effSetBlockSize, ..., maxBlockSize)
    //   7. dispatcher(effMainsChanged, 0, 1)  (resume)
    //   8. dispatcher(effStartProcess)
    // Failure at any step unwinds the prior steps. Reload of an already-
    // loaded slot unloads first.
    vst3::LoadResult Load(const std::string& path,
                          double               sampleRate,
                          std::int32_t         maxBlockSize);

    void Unload();
    bool IsLoaded() const noexcept;

    // Mono-in / mono-out through the plugin's processReplacing. Drops to
    // false (caller's bypass memcpy path) if the plugin doesn't expose
    // processReplacing (effFlagsCanReplacing clear), if it has more than
    // one input or output, or if frames > maxBlockSize.
    bool Process(const float* in, float* out, std::int32_t frames) noexcept;

    vst3::LoadInfo CurrentInfo() const;

    // Parameter introspection — best-effort. VST2 only gives us name +
    // label + current value via dispatcher opcodes; we synthesise the
    // rest of the ParamInfo struct (defaultValue := currentValue at probe
    // time, stepCount := 0, flags := automatable). All params are reported
    // as automatable; the operator's slider UI doesn't currently filter.
    std::vector<vst3::ParamInfo> ListParams();

    // Returns the value the plugin reports back via getParameter(idx)
    // after the set, in case the plugin quantises / clamps. NaN on error.
    double SetParam(std::uint32_t paramId, double normalized);

    // Editor: lazily creates a Vst2ViewWrapper around the loaded AEffect's
    // editor opcodes and returns it as an IPlugView so the GuiThread can
    // host it through the same EditorWindow path it uses for VST3.
    // Returns nullptr if no plugin is loaded or the plugin doesn't expose
    // an editor (effFlagsHasEditor clear). The pointer is owned by this
    // Vst2Plugin; pair every Acquire with a Release before Unload.
    Steinberg::IPlugView* AcquireEditorView() noexcept;
    void                  ReleaseEditorView() noexcept;

    // Same callback shape as PluginHost::SetParamChangedCallback. VST2
    // plugins notify the host of parameter changes via the
    // audioMasterAutomate opcode; we forward those fires through this
    // callback. Editor-driven param changes (when we eventually add an
    // editor) would route through this same path.
    using ParamChangedCallback = vst3::PluginHost::ParamChangedCallback;
    void SetParamChangedCallback(ParamChangedCallback cb);

    // Forward-declared public so the static C-callback in vst2_plugin.cpp
    // (audioMasterCallback) can recover the Impl* from AEffect::user. The
    // definition lives in the cpp; nothing leaks beyond the type name.
    struct Impl;

private:
    Impl* impl_;
};

}  // namespace zeus::plughost::vst2