// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// editor_idle_pump.h — optional idle hook for non-VST3 IPlugView wrappers.
//
// VST3 plugins integrate with the host via IRunLoop (timers + fds), so
// the GUI thread's select() loop drives them naturally. Linux VST2 and
// some CLAP plugins instead expect the host to tick them on the main
// thread (effEditIdle for VST2; clap_plugin->on_main_thread for CLAP)
// to repaint or process queued GUI events.
//
// View wrappers that need periodic main-thread time inherit this
// interface in addition to Steinberg::IPlugView. The GuiThread does a
// dynamic_cast on each editor's IPlugView and, if the cast succeeds,
// calls Idle() on its slow tick (~10 Hz, bounded by the existing 100 ms
// select cap). Wrappers that don't need it (the VST3 path) simply don't
// implement this interface.
//
// Idle() is called only from the GUI thread, after DispatchPendingXEvents.

#pragma once

namespace zeus::plughost::vst3 {

class IEditorIdlePump {
public:
    virtual ~IEditorIdlePump() = default;
    virtual void Idle() = 0;
};

}  // namespace zeus::plughost::vst3