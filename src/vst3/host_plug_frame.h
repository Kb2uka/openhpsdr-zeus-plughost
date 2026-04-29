// host_plug_frame.h — IPlugFrame for one editor window.
//
// Each EditorWindow gets one HostPlugFrame, set on the plugin's
// IPlugView via view->setFrame(frame). Plugins that need an IRunLoop
// query for it through queryInterface on the IPlugFrame; we forward to
// the per-process HostRunLoop owned by GuiThread.
//
// resizeView is the plugin's "please resize me" callback. It runs on
// the GUI thread (the plugin learned about us during attach() on the
// GUI thread), so we can safely XResizeWindow + view->onSize directly
// here. We also notify the GuiThread so it can ship an async 0x35
// EditorResized to the host.

#pragma once

#include <cstdint>
#include <functional>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

namespace zeus::plughost::vst3 {

class HostRunLoop;

// Single EditorWindow's plug-frame. Lifetime is bounded by the
// EditorWindow that owns it; the plugin only sees a non-owning pointer.
// addRef/release are no-ops because the SDK contract is that the host
// retains ownership of IPlugFrame across attach()/removed().
class HostPlugFrame : public Steinberg::IPlugFrame {
public:
    using ResizeCallback =
        std::function<void(int newWidth, int newHeight)>;

    // `runLoop` MUST outlive this HostPlugFrame (lives in GuiThread).
    HostPlugFrame(Display* display, ::Window xWindow,
                  HostRunLoop* runLoop, ResizeCallback onResize);
    ~HostPlugFrame();

    HostPlugFrame(const HostPlugFrame&)            = delete;
    HostPlugFrame& operator=(const HostPlugFrame&) = delete;

    // -- FUnknown --------------------------------------------------------
    Steinberg::tresult PLUGIN_API queryInterface(
        const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override  { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

    // -- IPlugFrame ------------------------------------------------------
    Steinberg::tresult PLUGIN_API resizeView(
        Steinberg::IPlugView* view,
        Steinberg::ViewRect* newSize) override;

private:
    Display*       display_;
    ::Window       xWindow_;
    HostRunLoop*   runLoop_;
    ResizeCallback onResize_;
};

}  // namespace zeus::plughost::vst3
