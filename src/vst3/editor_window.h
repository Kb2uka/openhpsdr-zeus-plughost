// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// editor_window.h — one X11 top-level window hosting one VST3 IPlugView.
//
// Every plugin slot that has its editor open gets one EditorWindow.
// The Window is a top-level (RootWindow child) decorated by the WM —
// titled with the plugin's display name, reaped via WM_DELETE_WINDOW.
// The plugin's IPlugView attaches directly to this window via
// attached(window, kPlatformTypeX11EmbedWindowID).
//
// All EditorWindow methods run on the GUI thread. The audio thread
// never touches this class.
//
// Lifecycle:
//   ctor:    nothing — just stores display + slot meta.
//   Attach:  XCreateWindow + XMapWindow + view->setFrame + view->attached.
//   Detach:  view->removed + XDestroyWindow + free our IPlugFrame.
//   dtor:    Detach if Attach was called.
//
// Width/Height accessors are GUI-thread only. The control-thread that
// builds the SlotShowEditorResult frame reads them via the GuiThread
// reply struct (the GUI thread snapshots them under its own data and
// hands them back via the reply queue).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

namespace zeus::plughost::vst3 {

class HostRunLoop;
class HostPlugFrame;
class IEditorIdlePump;

class EditorWindow {
public:
    EditorWindow(Display* display, int slotIdx,
                 const std::string& title, HostRunLoop* runLoop);
    ~EditorWindow();

    EditorWindow(const EditorWindow&)            = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Acquire the plugin's view, queryInterface for the X11 platform
    // type, create + map the X11 window, and attach. On any failure the
    // window is destroyed and Attach returns false (status is set in
    // outStatus per SlotShowEditorResult).
    //
    // The view's refcount is bumped by the caller before passing it in;
    // EditorWindow keeps a private IPtr until Detach().
    //
    // `idlePump` is optional. Non-null only for VST2 / CLAP wrappers
    // that need a periodic main-thread tick (effEditIdle / on_main_thread).
    // VST3 plugins drive themselves via IRunLoop and pass nullptr — we
    // never touch their view through any side-channel, so no chance of
    // a cross-DSO RTTI crash.
    bool Attach(Steinberg::IPlugView* view,
                IEditorIdlePump* idlePump,
                std::uint8_t& outStatus);

    // Idempotent. Calls view->removed and XDestroyWindow.
    void Detach();

    int SlotIdx()  const noexcept { return slotIdx_; }
    int Width()    const noexcept { return width_; }
    int Height()   const noexcept { return height_; }
    ::Window XWindow() const noexcept { return window_; }

    // Called by the GUI thread when an XEvent for our window arrives.
    // Returns true if the event was a close (window-manager DELETE or
    // explicit unmap from the plugin) — the GUI thread should shut the
    // window down and notify the host with EditorClosed.
    bool OnEvent(const XEvent& ev);

    // Called by the GUI thread when HostPlugFrame::resizeView ran
    // successfully (our HostPlugFrame is constructed with this as its
    // ResizeCallback target).
    void OnResized(int newW, int newH);

    // Periodic main-thread tick. Called only from the GUI thread.
    // No-op when `idlePump_` is null (VST3 path); for VST2 / CLAP this
    // forwards to the wrapper's Idle() so the plugin can repaint or
    // drain main-thread work.
    void OnIdleTick();

private:
    Display*                            display_;
    int                                 slotIdx_;
    std::string                         title_;
    HostRunLoop*                        runLoop_;

    Steinberg::IPtr<Steinberg::IPlugView> view_;
    IEditorIdlePump*                    idlePump_{nullptr};
    std::unique_ptr<HostPlugFrame>      plugFrame_;
    ::Window                            window_{0};
    Atom                                wmDeleteWindow_{None};

    int                                 width_{0};
    int                                 height_{0};
    bool                                attached_{false};
};

}  // namespace zeus::plughost::vst3