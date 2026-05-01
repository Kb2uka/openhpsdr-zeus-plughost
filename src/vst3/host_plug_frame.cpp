// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// host_plug_frame.cpp — IPlugFrame for one editor window.

#include "vst3/host_plug_frame.h"
#include "vst3/host_run_loop.h"

namespace zeus::plughost::vst3 {

using Steinberg::FUnknown;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::ViewRect;
using Steinberg::Linux::IRunLoop;

HostPlugFrame::HostPlugFrame(Display* display, ::Window xWindow,
                             HostRunLoop* runLoop,
                             ResizeCallback onResize)
    : display_(display), xWindow_(xWindow),
      runLoop_(runLoop), onResize_(std::move(onResize)) {}

HostPlugFrame::~HostPlugFrame() = default;

Steinberg::tresult PLUGIN_API HostPlugFrame::queryInterface(
    const Steinberg::TUID iid, void** obj) {
    if (obj == nullptr) return kInvalidArgument;
    if (Steinberg::FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame*>(this);
        addRef();
        return kResultTrue;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(iid, IRunLoop::iid)) {
        if (runLoop_ == nullptr) {
            *obj = nullptr;
            return kNoInterface;
        }
        // HostRunLoop's queryInterface handles IRunLoop / FUnknown and
        // returns its own self-pointer (addRef no-op). We just dispatch.
        return runLoop_->queryInterface(iid, obj);
    }
    *obj = nullptr;
    return kNoInterface;
}

Steinberg::tresult PLUGIN_API HostPlugFrame::resizeView(
    IPlugView* view, ViewRect* newSize) {
    if (view == nullptr || newSize == nullptr) return kInvalidArgument;
    const int w = newSize->getWidth();
    const int h = newSize->getHeight();
    if (w <= 0 || h <= 0) return kInvalidArgument;

    if (display_ != nullptr && xWindow_ != 0) {
        ::XResizeWindow(display_, xWindow_,
                        static_cast<unsigned int>(w),
                        static_cast<unsigned int>(h));
        ::XFlush(display_);
    }
    // Per the SDK contract we must call view->onSize after we've
    // resized our platform representation.
    view->onSize(newSize);
    if (onResize_) onResize_(w, h);
    return kResultOk;
}

}  // namespace zeus::plughost::vst3