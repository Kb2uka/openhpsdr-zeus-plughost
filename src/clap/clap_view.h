// clap_view.h — Steinberg::IPlugView wrapper around a CLAP plugin GUI.
//
// CLAP plugins expose their GUI through the clap.gui extension:
//   create -> set_scale -> set_size? -> set_parent -> show -> hide -> destroy
// with the host providing an X11 Window via clap_window_t. Zeus's editor
// host (EditorWindow + GuiThread) was originally written against
// Steinberg::IPlugView; this wrapper presents the CLAP editor through
// that same interface so the GUI thread doesn't have to grow a per-format
// code path.
//
// The wrapper is owned by ClapPlugin (created lazily by AcquireEditorView,
// destroyed by ReleaseEditorView). FUnknown addRef/release are no-ops
// returning a constant — lifetime is tied to the plugin, not refcounts —
// matching the same convention HostPlugFrame uses.

#pragma once

#include <cstdint>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "vst3/editor_idle_pump.h"

struct clap_plugin;
struct clap_plugin_gui;

namespace zeus::plughost::clap {

class ClapPlugin;

class ClapViewWrapper : public Steinberg::IPlugView,
                        public vst3::IEditorIdlePump {
public:
    // `plugin`, `gui`, and `owner` MUST outlive this wrapper. ClapPlugin's
    // Unload() destroys the wrapper first via ReleaseEditorView before
    // tearing down the plugin instance.
    ClapViewWrapper(const ::clap_plugin* plugin,
                    const ::clap_plugin_gui* gui,
                    ClapPlugin* owner);
    ~ClapViewWrapper() override;

    ClapViewWrapper(const ClapViewWrapper&)            = delete;
    ClapViewWrapper& operator=(const ClapViewWrapper&) = delete;

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
    const ::clap_plugin*      plugin_;
    const ::clap_plugin_gui*  gui_;
    ClapPlugin*               owner_;
    Steinberg::IPlugFrame*    frame_{nullptr};
    bool                      created_{false};
    bool                      shown_{false};
    bool                      embedded_{false};
    int                       width_{0};
    int                       height_{0};
};

}  // namespace zeus::plughost::clap
