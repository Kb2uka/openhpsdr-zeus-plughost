// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// clap_view.cpp — Steinberg::IPlugView wrapper around a CLAP plugin GUI.

#include "clap/clap_view.h"
#include "clap/clap_plugin.h"

#include "clap/plugin.h"
#include "clap/ext/gui.h"

#include <cstdint>
#include <cstring>

namespace zeus::plughost::clap {

using Steinberg::FIDString;
using Steinberg::FUnknown;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kPlatformTypeX11EmbedWindowID;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::tresult;
using Steinberg::ViewRect;

ClapViewWrapper::ClapViewWrapper(const ::clap_plugin* plugin,
                                 const ::clap_plugin_gui* gui,
                                 ClapPlugin* owner)
    : plugin_(plugin), gui_(gui), owner_(owner) {
    // Allocate the plugin's GUI immediately so getSize / canResize probes
    // from EditorWindow::Attach (which happen BEFORE view->attached) find
    // a valid internal gui object. DPF's CLAP wrapper, for example,
    // asserts gui != nullptr inside its get_size / get_resize_hints
    // implementations and bails out before hosting anything if create
    // hasn't been called yet. Pairing create with the wrapper ctor and
    // destroy with the wrapper dtor keeps the lifecycle balanced.
    if (gui_ != nullptr && plugin_ != nullptr && gui_->create != nullptr) {
        if (gui_->create(plugin_, CLAP_WINDOW_API_X11, /*is_floating=*/false)) {
            created_ = true;
            // Cache the plugin's preferred size up front so getSize never
            // has to call back into the plugin pre-attach.
            if (gui_->get_size != nullptr) {
                uint32_t w = 0, h = 0;
                if (gui_->get_size(plugin_, &w, &h) && w > 0 && h > 0) {
                    width_  = static_cast<int>(w);
                    height_ = static_cast<int>(h);
                }
            }
        }
    }
}

ClapViewWrapper::~ClapViewWrapper() {
    // Hide first if still shown, then destroy. Mirrors the constructor's
    // create() so every wrapper instance is one create/destroy pair.
    if (gui_ != nullptr && plugin_ != nullptr && created_) {
        if (shown_ && gui_->hide) {
            gui_->hide(plugin_);
        }
        if (gui_->destroy) {
            gui_->destroy(plugin_);
        }
    }
    created_  = false;
    shown_    = false;
    embedded_ = false;
}

// -- FUnknown ------------------------------------------------------------

tresult PLUGIN_API ClapViewWrapper::queryInterface(
    const Steinberg::TUID iid, void** obj) {
    if (obj == nullptr) return kInvalidArgument;
    if (Steinberg::FUnknownPrivate::iidEqual(iid, IPlugView::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugView*>(this);
        addRef();
        return kResultTrue;
    }
    *obj = nullptr;
    return kNoInterface;
}

// -- IPlugView -----------------------------------------------------------

tresult PLUGIN_API ClapViewWrapper::isPlatformTypeSupported(FIDString type) {
    if (type == nullptr) return kInvalidArgument;
    if (std::strcmp(type, kPlatformTypeX11EmbedWindowID) != 0) {
        return kResultFalse;
    }
    if (gui_ == nullptr || gui_->is_api_supported == nullptr) {
        return kResultFalse;
    }
    return gui_->is_api_supported(plugin_, CLAP_WINDOW_API_X11, /*is_floating=*/false)
        ? kResultTrue
        : kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::attached(void* parent, FIDString type) {
    if (gui_ == nullptr || plugin_ == nullptr) return kResultFalse;
    if (isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
    if (!created_) return kResultFalse;  // ctor's gui_->create() failed

    // Embed into the host's X11 Window. The CLAP X11 union member is
    // `clap_xwnd` (unsigned long), matching the X Window type.
    // EditorWindow passes the X Window cast through intptr_t -> void*.
    ::clap_window_t win;
    std::memset(&win, 0, sizeof(win));
    win.api = CLAP_WINDOW_API_X11;
    win.x11 = static_cast<::clap_xwnd>(reinterpret_cast<std::uintptr_t>(parent));

    if (gui_->set_parent == nullptr || !gui_->set_parent(plugin_, &win)) {
        return kResultFalse;
    }
    embedded_ = true;

    // Show the GUI. Some plugins draw nothing until show() is called.
    if (gui_->show != nullptr) {
        if (!gui_->show(plugin_)) {
            return kResultFalse;
        }
        shown_ = true;
    }
    return kResultOk;
}

tresult PLUGIN_API ClapViewWrapper::removed() {
    if (gui_ == nullptr || plugin_ == nullptr) return kResultFalse;
    // Tear down the plugin's GUI here, BEFORE EditorWindow::Detach
    // destroys our parent X Window. Doing it the other way around left
    // pugl/GLX trying to clean up children of an already-dead parent,
    // which surfaces as BadWindow / GLXBadDrawable spam in the X log
    // and (for some plugins) a crashed sidecar.
    if (shown_ && gui_->hide != nullptr) {
        gui_->hide(plugin_);
    }
    if (created_ && gui_->destroy != nullptr) {
        gui_->destroy(plugin_);
    }
    shown_    = false;
    embedded_ = false;
    created_  = false;
    return kResultOk;
}

tresult PLUGIN_API ClapViewWrapper::onWheel(float /*distance*/) {
    return kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::onKeyDown(
    Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/,
    Steinberg::int16 /*modifiers*/) {
    return kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::onKeyUp(
    Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/,
    Steinberg::int16 /*modifiers*/) {
    return kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::getSize(ViewRect* size) {
    if (size == nullptr) return kInvalidArgument;
    int w = width_;
    int h = height_;
    if ((w <= 0 || h <= 0) && gui_ != nullptr && plugin_ != nullptr &&
        gui_->get_size != nullptr) {
        uint32_t qw = 0, qh = 0;
        if (gui_->get_size(plugin_, &qw, &qh)) {
            if (qw > 0) w = static_cast<int>(qw);
            if (qh > 0) h = static_cast<int>(qh);
        }
    }
    if (w <= 0) w = 400;
    if (h <= 0) h = 300;
    size->left   = 0;
    size->top    = 0;
    size->right  = w;
    size->bottom = h;
    return kResultOk;
}

tresult PLUGIN_API ClapViewWrapper::onSize(ViewRect* newSize) {
    if (newSize == nullptr) return kInvalidArgument;
    width_  = newSize->getWidth();
    height_ = newSize->getHeight();
    if (gui_ != nullptr && plugin_ != nullptr && gui_->set_size != nullptr &&
        width_ > 0 && height_ > 0) {
        gui_->set_size(plugin_,
                       static_cast<uint32_t>(width_),
                       static_cast<uint32_t>(height_));
    }
    return kResultOk;
}

tresult PLUGIN_API ClapViewWrapper::onFocus(Steinberg::TBool /*state*/) {
    return kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::setFrame(IPlugFrame* frame) {
    frame_ = frame;
    return kResultOk;
}

tresult PLUGIN_API ClapViewWrapper::canResize() {
    if (gui_ == nullptr || plugin_ == nullptr || gui_->can_resize == nullptr) {
        return kResultFalse;
    }
    return gui_->can_resize(plugin_) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ClapViewWrapper::checkSizeConstraint(ViewRect* rect) {
    if (rect == nullptr) return kInvalidArgument;
    if (gui_ == nullptr || plugin_ == nullptr || gui_->adjust_size == nullptr) {
        return kResultFalse;
    }
    int w = rect->getWidth();
    int h = rect->getHeight();
    if (w <= 0 || h <= 0) return kInvalidArgument;
    uint32_t aw = static_cast<uint32_t>(w);
    uint32_t ah = static_cast<uint32_t>(h);
    if (!gui_->adjust_size(plugin_, &aw, &ah)) return kResultFalse;
    rect->right  = rect->left + static_cast<Steinberg::int32>(aw);
    rect->bottom = rect->top  + static_cast<Steinberg::int32>(ah);
    return kResultTrue;
}

// -- IEditorIdlePump -----------------------------------------------------

void ClapViewWrapper::Idle() {
    // Forward to ClapPlugin so it can pump on_main_thread AND fire any
    // due timers the plugin registered via the timer-support extension.
    // pugl/DPF-based UIs drive their entire redraw cycle off those
    // timers, so without this the editor stays blank.
    if (owner_ != nullptr) {
        owner_->OnEditorIdleTick();
    }
}

}  // namespace zeus::plughost::clap