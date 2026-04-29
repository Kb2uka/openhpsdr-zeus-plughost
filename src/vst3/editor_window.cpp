// editor_window.cpp — one top-level X11 window hosting one IPlugView.

#include "vst3/editor_window.h"
#include "vst3/host_plug_frame.h"
#include "vst3/host_run_loop.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <cstdio>

namespace zeus::plughost::vst3 {

using Steinberg::IPlugView;
using Steinberg::IPtr;
using Steinberg::kPlatformTypeX11EmbedWindowID;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::tresult;
using Steinberg::ViewRect;

EditorWindow::EditorWindow(Display* display, int slotIdx,
                           const std::string& title, HostRunLoop* runLoop)
    : display_(display), slotIdx_(slotIdx), title_(title),
      runLoop_(runLoop) {}

EditorWindow::~EditorWindow() {
    Detach();
}

bool EditorWindow::Attach(IPlugView* view, std::uint8_t& outStatus) {
    if (view == nullptr) {
        outStatus = 5;
        return false;
    }
    if (attached_) {
        // Already attached — should be a no-op at the caller level.
        outStatus = 0;
        return true;
    }
    if (display_ == nullptr) {
        outStatus = 7;
        return false;
    }

    // 1. Confirm the plugin understands the X11 embed platform type.
    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        outStatus = 3;  // platform-not-supported
        return false;
    }

    // 2. Ask the plugin its initial size.
    ViewRect size{};
    tresult tr = view->getSize(&size);
    if (tr != kResultOk && tr != kResultTrue) {
        outStatus = 5;
        return false;
    }
    int width  = size.getWidth();
    int height = size.getHeight();
    if (width  <= 0) width  = 400;  // defensive default
    if (height <= 0) height = 300;

    // 3. Create the top-level X11 window.
    const int screen = DefaultScreen(display_);
    const ::Window root = RootWindow(display_, screen);

    XSetWindowAttributes attrs{};
    attrs.background_pixel = BlackPixel(display_, screen);
    attrs.border_pixel     = BlackPixel(display_, screen);
    attrs.event_mask       = StructureNotifyMask | ExposureMask;
    const unsigned long mask = CWBackPixel | CWBorderPixel | CWEventMask;

    ::Window w = XCreateWindow(display_, root,
                               0, 0,
                               static_cast<unsigned int>(width),
                               static_cast<unsigned int>(height),
                               0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               mask, &attrs);
    if (w == 0) {
        outStatus = 5;
        return false;
    }
    window_ = w;

    // 4. Title + WM_DELETE_WINDOW protocol so we can intercept close.
    XStoreName(display_, window_, title_.c_str());
    {
        XClassHint hint{};
        hint.res_name  = const_cast<char*>("zeus-plughost-editor");
        hint.res_class = const_cast<char*>("ZeusPlughostEditor");
        XSetClassHint(display_, window_, &hint);
    }
    wmDeleteWindow_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, window_, &wmDeleteWindow_, 1);

    // 5. Honour canResize: set size hints accordingly. If the plugin
    //    cannot resize, lock the WM-side size bounds.
    {
        const bool canResize = (view->canResize() == kResultTrue);
        XSizeHints* hints = XAllocSizeHints();
        if (hints != nullptr) {
            hints->flags = PMinSize;
            if (canResize) {
                hints->min_width  = 80;
                hints->min_height = 60;
            } else {
                hints->flags |= PMaxSize;
                hints->min_width  = hints->max_width  = width;
                hints->min_height = hints->max_height = height;
            }
            XSetWMNormalHints(display_, window_, hints);
            XFree(hints);
        }
    }

    // 6. Build a HostPlugFrame backed by this window + the GUI thread's
    //    HostRunLoop. The plugin will queryInterface for IRunLoop on it.
    //    We capture `this` so resize callbacks can update our cached
    //    width/height — but we DO NOT post the async event from inside
    //    the callback (the GuiThread sets a different callback that
    //    forwards to the host).
    plugFrame_ = std::make_unique<HostPlugFrame>(
        display_, window_, runLoop_,
        [this](int newW, int newH) {
            this->OnResized(newW, newH);
        });

    // 7. setFrame BEFORE attached, per SDK convention so plugins that
    //    call resizeView during attached have a frame to call.
    view->setFrame(plugFrame_.get());

    // 8. Map the window so the plugin sees a realized X11 Window when
    //    it runs attached(). Some plugins don't strictly require this,
    //    but it matches the SDK example flow and avoids subtle "I never
    //    saw a MapNotify" issues.
    XMapWindow(display_, window_);
    XFlush(display_);

    // 9. Attach. This is the load-bearing call — the plugin creates its
    //    GUI inside our window. On failure, tear everything down.
    tr = view->attached(reinterpret_cast<void*>(window_),
                        kPlatformTypeX11EmbedWindowID);
    if (tr != kResultOk && tr != kResultTrue) {
        view->setFrame(nullptr);
        plugFrame_.reset();
        XDestroyWindow(display_, window_);
        XFlush(display_);
        window_ = 0;
        outStatus = 4;  // attach-failed
        return false;
    }

    // 10. Some plugins call resizeView during attached() — re-read the
    //     view's reported size to catch that.
    {
        ViewRect after{};
        if (view->getSize(&after) == kResultOk) {
            const int w2 = after.getWidth();
            const int h2 = after.getHeight();
            if (w2 > 0 && h2 > 0 && (w2 != width || h2 != height)) {
                XResizeWindow(display_, window_,
                              static_cast<unsigned int>(w2),
                              static_cast<unsigned int>(h2));
                width  = w2;
                height = h2;
            }
        }
    }

    width_    = width;
    height_   = height;
    view_     = view;     // bumps the IPtr refcount
    attached_ = true;
    outStatus = 0;
    XFlush(display_);
    return true;
}

void EditorWindow::Detach() {
    if (attached_ && view_) {
        view_->setFrame(nullptr);
        view_->removed();
    }
    view_.reset();

    if (display_ != nullptr && window_ != 0) {
        XUnmapWindow(display_, window_);
        XDestroyWindow(display_, window_);
        XFlush(display_);
    }
    window_   = 0;
    plugFrame_.reset();
    attached_ = false;
}

bool EditorWindow::OnEvent(const XEvent& ev) {
    switch (ev.type) {
        case ClientMessage: {
            if (wmDeleteWindow_ != None &&
                ev.xclient.window == window_ &&
                static_cast<Atom>(ev.xclient.data.l[0]) == wmDeleteWindow_) {
                return true;  // GUI thread will tear us down
            }
            break;
        }
        case ConfigureNotify: {
            if (ev.xconfigure.window == window_) {
                int w = ev.xconfigure.width;
                int h = ev.xconfigure.height;
                if (w > 0 && h > 0 && (w != width_ || h != height_)) {
                    width_  = w;
                    height_ = h;
                    if (view_) {
                        ViewRect rect(0, 0, w, h);
                        view_->onSize(&rect);
                    }
                }
            }
            break;
        }
        case DestroyNotify: {
            if (ev.xdestroywindow.window == window_) {
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

void EditorWindow::OnResized(int newW, int newH) {
    if (newW > 0) width_  = newW;
    if (newH > 0) height_ = newH;
}

}  // namespace zeus::plughost::vst3
