// vst2_view.h — Steinberg::IPlugView wrapper around a VST 2.4 AEffect editor.
//
// VST2 plugins expose their editor via the dispatcher opcodes
// effEditGetRect / effEditOpen / effEditClose / effEditIdle, with the
// platform parent supplied as a raw window handle (X11 Window on Linux).
// Zeus's editor host (EditorWindow + GuiThread) was originally written
// against Steinberg::IPlugView from the VST3 SDK; this wrapper presents
// the VST2 editor through that same interface so the GUI thread doesn't
// have to grow a per-format code path.
//
// The wrapper is owned by Vst2Plugin (created lazily by AcquireEditorView,
// destroyed by ReleaseEditorView). FUnknown addRef/release are no-ops
// returning a constant — lifetime is tied to the plugin, not refcounts —
// matching the same convention HostPlugFrame uses.
//
// Idle pump: VST2 plugins that draw their own GUI typically expect the
// host to call effEditIdle ~10–30 Hz so they can repaint. The GuiThread
// downcasts the IPlugView to IEditorIdlePump and ticks Idle() each pass
// of its select() loop (capped at 10 Hz today by the 100 ms wake cap).

#pragma once

#include <cstdint>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "vst3/editor_idle_pump.h"

struct AEffect;

namespace zeus::plughost::vst2 {

class Vst2ViewWrapper : public Steinberg::IPlugView,
                        public vst3::IEditorIdlePump {
public:
    // `effect` MUST outlive this wrapper. The wrapper does not unload the
    // plugin; Vst2Plugin's Unload() destroys the wrapper first via
    // ReleaseEditorView before tearing down the AEffect.
    explicit Vst2ViewWrapper(AEffect* effect);
    ~Vst2ViewWrapper() override;

    Vst2ViewWrapper(const Vst2ViewWrapper&)            = delete;
    Vst2ViewWrapper& operator=(const Vst2ViewWrapper&) = delete;

    // -- FUnknown --------------------------------------------------------
    Steinberg::tresult PLUGIN_API queryInterface(
        const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override  { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

    // -- IPlugView -------------------------------------------------------
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(
        Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(
        void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onWheel(float distance) override;
    Steinberg::tresult PLUGIN_API onKeyDown(
        Steinberg::char16 key, Steinberg::int16 keyCode,
        Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API onKeyUp(
        Steinberg::char16 key, Steinberg::int16 keyCode,
        Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(
        Steinberg::ViewRect* rect) override;

    // -- IEditorIdlePump -------------------------------------------------
    void Idle() override;

private:
    // Read the plugin's reported edit-rect via effEditGetRect. Falls back
    // to a 400x300 default if the plugin returns null or a degenerate rect.
    void QueryEditRect(int& outWidth, int& outHeight) const;

    AEffect*               effect_;
    Steinberg::IPlugFrame* frame_{nullptr};
    bool                   attached_{false};
    int                    width_{0};
    int                    height_{0};
};

}  // namespace zeus::plughost::vst2
