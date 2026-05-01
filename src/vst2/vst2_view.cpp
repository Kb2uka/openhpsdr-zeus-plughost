// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// vst2_view.cpp — Steinberg::IPlugView wrapper around a VST 2.4 AEffect editor.

#include "vst2/vst2_view.h"
#include "aeffectx.h"

#include <cstdint>
#include <cstring>

namespace zeus::plughost::vst2 {

using Steinberg::FIDString;
using Steinberg::FUnknown;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kNotImplemented;
using Steinberg::kPlatformTypeX11EmbedWindowID;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::tresult;
using Steinberg::ViewRect;

Vst2ViewWrapper::Vst2ViewWrapper(AEffect* effect) : effect_(effect) {}

Vst2ViewWrapper::~Vst2ViewWrapper() {
    // Defensive: if EditorWindow forgot to call removed(), close the
    // editor here so the plugin's GUI resources aren't leaked. effClose
    // is documented as idempotent on most plugins; best-effort otherwise.
    if (attached_ && effect_ != nullptr) {
        effect_->dispatcher(effect_, effEditClose, 0, 0, nullptr, 0.0f);
        attached_ = false;
    }
}

void Vst2ViewWrapper::QueryEditRect(int& outWidth, int& outHeight) const {
    outWidth  = 400;
    outHeight = 300;
    if (effect_ == nullptr) return;

    ERect* rect = nullptr;
    effect_->dispatcher(effect_, effEditGetRect, 0, 0, &rect, 0.0f);
    if (rect == nullptr) return;
    int w = rect->right  - rect->left;
    int h = rect->bottom - rect->top;
    if (w > 0) outWidth  = w;
    if (h > 0) outHeight = h;
}

// -- FUnknown ------------------------------------------------------------

tresult PLUGIN_API Vst2ViewWrapper::queryInterface(
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

tresult PLUGIN_API Vst2ViewWrapper::isPlatformTypeSupported(FIDString type) {
    if (type == nullptr) return kInvalidArgument;
    // EditorWindow only ever asks for X11; VST2 plugins on Linux only
    // ever speak X11. Refuse any other platform string so callers fail
    // fast with a clear "platform not supported" status.
    if (std::strcmp(type, kPlatformTypeX11EmbedWindowID) != 0) {
        return kResultFalse;
    }
    return kResultTrue;
}

tresult PLUGIN_API Vst2ViewWrapper::attached(void* parent, FIDString type) {
    if (effect_ == nullptr) return kResultFalse;
    if (attached_) return kResultOk;
    if (isPlatformTypeSupported(type) != kResultTrue) {
        return kResultFalse;
    }

    // Cache initial dimensions so getSize is correct before the plugin
    // potentially calls back through any host channel during effEditOpen.
    QueryEditRect(width_, height_);

    // VST2 effEditOpen takes a platform-native window handle in `ptr`.
    // On X11, hosts pass the X Window cast through intptr_t -> void*.
    // EditorWindow already created and mapped the parent X Window before
    // calling attached().
    intptr_t r = effect_->dispatcher(
        effect_, effEditOpen, 0, 0, parent, 0.0f);
    // Most plugins return 1 on success, 0 on failure; treat any non-zero
    // as success per the convention LinuxSampler / Carla / etc. follow.
    if (r == 0) {
        return kResultFalse;
    }
    attached_ = true;

    // Re-query the rect — some plugins compute their final size only
    // after the parent is realised.
    int w = 0, h = 0;
    QueryEditRect(w, h);
    if (w > 0) width_  = w;
    if (h > 0) height_ = h;
    return kResultOk;
}

tresult PLUGIN_API Vst2ViewWrapper::removed() {
    if (effect_ == nullptr) return kResultFalse;
    if (!attached_) return kResultOk;
    effect_->dispatcher(effect_, effEditClose, 0, 0, nullptr, 0.0f);
    attached_ = false;
    return kResultOk;
}

tresult PLUGIN_API Vst2ViewWrapper::onWheel(float /*distance*/) {
    return kResultFalse;
}

tresult PLUGIN_API Vst2ViewWrapper::onKeyDown(
    Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/,
    Steinberg::int16 /*modifiers*/) {
    // VST2 has effEditKeyDown but the host-side wiring would require us
    // to translate Steinberg virtual-key codes back into VST2 ones. We
    // let the plugin's own X11 event handlers see key events directly.
    return kResultFalse;
}

tresult PLUGIN_API Vst2ViewWrapper::onKeyUp(
    Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/,
    Steinberg::int16 /*modifiers*/) {
    return kResultFalse;
}

tresult PLUGIN_API Vst2ViewWrapper::getSize(ViewRect* size) {
    if (size == nullptr) return kInvalidArgument;
    int w = width_;
    int h = height_;
    if (w <= 0 || h <= 0) {
        QueryEditRect(w, h);
    }
    size->left   = 0;
    size->top    = 0;
    size->right  = w;
    size->bottom = h;
    return kResultOk;
}

tresult PLUGIN_API Vst2ViewWrapper::onSize(ViewRect* newSize) {
    if (newSize == nullptr) return kInvalidArgument;
    width_  = newSize->getWidth();
    height_ = newSize->getHeight();
    // VST2 has no onSize equivalent — the host XResizeWindow already
    // happened in HostPlugFrame::resizeView. The plugin observes the
    // new size via its own X11 ConfigureNotify on the parent.
    return kResultOk;
}

tresult PLUGIN_API Vst2ViewWrapper::onFocus(Steinberg::TBool /*state*/) {
    return kResultFalse;
}

tresult PLUGIN_API Vst2ViewWrapper::setFrame(IPlugFrame* frame) {
    frame_ = frame;
    return kResultOk;
}

tresult PLUGIN_API Vst2ViewWrapper::canResize() {
    // VST 2.4 didn't standardise resizable editors. A few plugins handle
    // it via audioMasterSizeWindow but most are fixed-size. Default to
    // false so EditorWindow locks the WM size hints to the reported rect.
    return kResultFalse;
}

tresult PLUGIN_API Vst2ViewWrapper::checkSizeConstraint(ViewRect* /*rect*/) {
    return kResultFalse;
}

// -- IEditorIdlePump -----------------------------------------------------

void Vst2ViewWrapper::Idle() {
    if (effect_ == nullptr || !attached_) return;
    effect_->dispatcher(effect_, effEditIdle, 0, 0, nullptr, 0.0f);
}

}  // namespace zeus::plughost::vst2