// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// gui_thread.h — dedicated GUI thread for native plugin editor windows.
//
// One instance per sidecar process. Lazy Start() on first SlotShowEditor.
// Owns:
//   - the X11 Display* connection (one per process)
//   - the HostRunLoop (so plugins can register fds + timers)
//   - the per-slot EditorWindow map
//   - a request queue (control thread posts; GUI thread consumes)
//
// The GUI thread's main loop is a select() over (X11 fd, request-queue
// pipe-fd, IRunLoop fds), with a timeout sized by the soonest pending
// IRunLoop timer. On wake it dispatches XEvents to per-window handlers,
// pumps timers, dispatches fd handlers, and processes any queued
// requests.
//
// Threading discipline:
//   - Audio thread: NEVER touches GuiThread. Continues processing
//     PluginChain::Process under its own atomics.
//   - Control thread: posts ShowEditorRequest / HideEditorRequest /
//     QuitRequest via the request queue, optionally blocks on a reply
//     condvar (1 s budget) before sending the result frame back to .NET.
//   - GUI thread: only touches X11, the HostRunLoop, the EditorWindow
//     map, and its private side of the queue.
//
// Async events (window closed by WM, plugin resize) are surfaced via
// SetAsyncCallback; the callback runs on the GUI thread, so the
// callback implementation MUST be lock-free / non-blocking (we forward
// to a small lock-protected "pending events" queue that main.cpp pumps
// from the control reader thread).

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "vst3/sdk_includes.h"
#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

namespace zeus::plughost::vst3 {

class EditorWindow;
class HostRunLoop;
class PluginChain;
class IEditorIdlePump;

class GuiThread {
public:
    static constexpr int kMaxSlots = 8;

    // Async event tags surfaced to the host (match wire-spec 0x34 / 0x35).
    enum class AsyncEventTag : std::uint8_t {
        EditorClosed  = 0x34,
        EditorResized = 0x35,
    };
    using AsyncCallback = std::function<void(
        AsyncEventTag tag, int slotIdx, int width, int height)>;

    struct ShowResult {
        bool          ok;
        std::uint8_t  status;  // matches SlotShowEditorResult enum
        int           width;
        int           height;
    };

    GuiThread();
    ~GuiThread();

    GuiThread(const GuiThread&)            = delete;
    GuiThread& operator=(const GuiThread&) = delete;

    // Lazy start. Idempotent. Returns false if the X11 display couldn't
    // be opened (DISPLAY unset, etc.). Subsequent ShowEditor calls then
    // return status 7 (gui-thread-init-failed).
    bool Start();

    // Stop the GUI thread, closing every editor first. Idempotent.
    void Stop();

    bool IsRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Show the editor for `slotIdx`. The caller (control thread) acquires
    // `view` (with refcount bumped) before calling. The GUI thread takes
    // ownership of the view via the EditorWindow's IPtr. If an editor is
    // already open for this slot, we return ok with the current size.
    //
    // `idlePump` is optional and non-null only for VST2 / CLAP wrappers
    // that need a periodic main-thread tick. VST3 callers pass nullptr.
    //
    // Synchronous up to `timeout`. Returns status 7 if the GUI thread
    // hasn't been started yet (caller's responsibility to Start first).
    ShowResult RequestShow(int slotIdx, Steinberg::IPlugView* view,
                           IEditorIdlePump* idlePump,
                           const std::string& title);

    // Hide the editor for `slotIdx`. Returns true if an editor was open
    // (status 0), false if not (status 1).
    bool RequestHide(int slotIdx);

    void SetAsyncCallback(AsyncCallback cb);

private:
    enum class RequestKind { Show, Hide, Quit };

    struct Request {
        RequestKind                          kind;
        int                                  slotIdx{-1};
        Steinberg::IPlugView*                view{nullptr};
        IEditorIdlePump*                     idlePump{nullptr};
        std::string                          title;

        // Reply machinery — caller waits on the same condvar.
        std::mutex*                          replyMutex{nullptr};
        std::condition_variable*             replyCv{nullptr};
        bool*                                replyReady{nullptr};
        ShowResult*                          showReply{nullptr};
        bool*                                hideReply{nullptr};
    };

    void ThreadMain();
    void DrainRequests();
    void HandleRequest(const Request& req);
    void HandleShow(const Request& req);
    void HandleHide(const Request& req);
    void CloseEditorOnGuiThread(int slotIdx, bool fireAsync);
    void DispatchPendingXEvents();
    int  WakePipeReadFd() const noexcept { return wakePipe_[0]; }
    void Wake();

    // Async event delivery: GUI thread captures into a small queue;
    // the control-thread side polls + drains.
    void EnqueueAsyncEvent(AsyncEventTag tag, int slot, int w, int h);

    std::atomic<bool>            running_{false};
    std::atomic<bool>            stopRequested_{false};
    std::thread                  thread_;

    Display*                     display_{nullptr};
    std::unique_ptr<HostRunLoop> runLoop_;

    std::array<std::unique_ptr<EditorWindow>, kMaxSlots> windows_;

    // Request queue (control thread -> GUI thread).
    std::mutex                   queueMutex_;
    std::deque<Request>          queue_;

    // Self-pipe used to wake the GUI thread's select() when a new
    // request arrives.
    int wakePipe_[2]{-1, -1};

    // Async callback shim. Called on the GUI thread; implementations
    // must hand off to another thread before doing socket I/O.
    std::mutex                   asyncMutex_;
    AsyncCallback                asyncCallback_;
};

}  // namespace zeus::plughost::vst3