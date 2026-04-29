// gui_thread.cpp — dedicated GUI thread implementation.

#include "vst3/gui_thread.h"
#include "vst3/editor_window.h"
#include "vst3/host_run_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

namespace zeus::plughost::vst3 {

GuiThread::GuiThread() = default;

GuiThread::~GuiThread() {
    Stop();
}

bool GuiThread::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    // Open the X11 display on the calling thread; if this fails, no GUI
    // thread is started and Show requests will fail with status 7.
    Display* d = XOpenDisplay(nullptr);
    if (d == nullptr) {
        std::fprintf(stderr,
            "zeus-plughost: GuiThread::Start: XOpenDisplay returned null "
            "(DISPLAY=%s)\n",
            std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)");
        return false;
    }

    // Self-pipe for select() wakeups. Non-blocking on both ends so we
    // never stall the control thread when the GUI thread is busy.
    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        XCloseDisplay(d);
        std::fprintf(stderr, "zeus-plughost: GuiThread::Start: pipe() failed\n");
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(pipeFds[i], F_GETFL, 0);
        if (flags >= 0) ::fcntl(pipeFds[i], F_SETFL, flags | O_NONBLOCK);
    }
    wakePipe_[0] = pipeFds[0];
    wakePipe_[1] = pipeFds[1];

    display_  = d;
    runLoop_  = std::make_unique<HostRunLoop>();
    stopRequested_.store(false, std::memory_order_release);

    thread_ = std::thread([this]() { this->ThreadMain(); });
    running_.store(true, std::memory_order_release);
    return true;
}

void GuiThread::Stop() {
    if (!running_.load(std::memory_order_acquire) && !thread_.joinable()) {
        // Even if not started, clean up any leftover descriptors.
        if (wakePipe_[0] != -1) { ::close(wakePipe_[0]); wakePipe_[0] = -1; }
        if (wakePipe_[1] != -1) { ::close(wakePipe_[1]); wakePipe_[1] = -1; }
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        Request q;
        q.kind = RequestKind::Quit;
        queue_.push_back(std::move(q));
    }
    Wake();
    if (thread_.joinable()) thread_.join();

    running_.store(false, std::memory_order_release);

    // Pipe + display are tidied up in the GUI thread before it exits;
    // null them out here so we can be Start()ed again later.
    if (wakePipe_[0] != -1) { ::close(wakePipe_[0]); wakePipe_[0] = -1; }
    if (wakePipe_[1] != -1) { ::close(wakePipe_[1]); wakePipe_[1] = -1; }
    if (display_ != nullptr) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
    runLoop_.reset();
}

void GuiThread::Wake() {
    if (wakePipe_[1] < 0) return;
    const char b = 1;
    ssize_t n = ::write(wakePipe_[1], &b, 1);
    (void)n;  // best-effort; non-blocking, EAGAIN means already pending
}

void GuiThread::SetAsyncCallback(AsyncCallback cb) {
    std::lock_guard<std::mutex> lk(asyncMutex_);
    asyncCallback_ = std::move(cb);
}

GuiThread::ShowResult GuiThread::RequestShow(
    int slotIdx, Steinberg::IPlugView* view, const std::string& title) {
    ShowResult result{false, 7, 0, 0};
    if (!running_.load(std::memory_order_acquire)) {
        return result;
    }
    if (slotIdx < 0 || slotIdx >= kMaxSlots) {
        result.status = 6;  // invalid-slot
        return result;
    }

    std::mutex             m;
    std::condition_variable cv;
    bool                   ready = false;

    Request req;
    req.kind        = RequestKind::Show;
    req.slotIdx     = slotIdx;
    req.view        = view;
    req.title       = title;
    req.replyMutex  = &m;
    req.replyCv     = &cv;
    req.replyReady  = &ready;
    req.showReply   = &result;

    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back(std::move(req));
    }
    Wake();

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return ready; });
        if (!ready) {
            // Timed out waiting for the GUI thread — likely deadlocked
            // inside a plugin attached() call. Surface as status 5.
            return ShowResult{false, 5, 0, 0};
        }
    }
    return result;
}

bool GuiThread::RequestHide(int slotIdx) {
    if (!running_.load(std::memory_order_acquire)) return false;
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return false;

    std::mutex             m;
    std::condition_variable cv;
    bool                   ready = false;
    bool                   reply = false;

    Request req;
    req.kind       = RequestKind::Hide;
    req.slotIdx    = slotIdx;
    req.replyMutex = &m;
    req.replyCv    = &cv;
    req.replyReady = &ready;
    req.hideReply  = &reply;

    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back(std::move(req));
    }
    Wake();

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return ready; });
    }
    return reply;
}

// ---------------------------------------------------------------------------
// GUI thread main
// ---------------------------------------------------------------------------

void GuiThread::ThreadMain() {
    // The Display* belongs to this thread for its lifetime. Plugins that
    // do their own Xlib calls inside attached()/onSize() will use this
    // same connection because they typically open it via the IRunLoop's
    // fd registration, but in any case we are the sole consumer of the
    // event queue.
    XSync(display_, False);

    while (!stopRequested_.load(std::memory_order_acquire)) {
        // 1. Service any pending requests posted by the control thread.
        DrainRequests();
        if (stopRequested_.load(std::memory_order_acquire)) break;

        // 2. Process X events that have already arrived.
        DispatchPendingXEvents();

        // 3. Tick timers + compute next-fire deadline.
        const std::uint64_t nextMs =
            runLoop_ ? runLoop_->HandleTimersAndGetNextFireMs()
                     : HostRunLoop::kNoTimers;

        // 4. Build the fd_set.
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxFd = -1;

        const int xfd = ConnectionNumber(display_);
        FD_SET(xfd, &rfds);
        if (xfd > maxFd) maxFd = xfd;

        const int wakeFd = wakePipe_[0];
        if (wakeFd >= 0) {
            FD_SET(wakeFd, &rfds);
            if (wakeFd > maxFd) maxFd = wakeFd;
        }

        std::vector<int> runLoopFds;
        if (runLoop_) {
            int rlMax = runLoop_->CollectFds(runLoopFds);
            if (rlMax > maxFd) maxFd = rlMax;
            for (int fd : runLoopFds) FD_SET(fd, &rfds);
        }

        struct timeval tv;
        struct timeval* tvp = nullptr;
        if (nextMs != HostRunLoop::kNoTimers) {
            // Cap waits at 100 ms so the request queue gets serviced
            // promptly even if a plugin never registers a timer.
            std::uint64_t cap = std::min<std::uint64_t>(nextMs, 100ull);
            tv.tv_sec  = static_cast<time_t>(cap / 1000ull);
            tv.tv_usec = static_cast<suseconds_t>((cap % 1000ull) * 1000ull);
            tvp = &tv;
        } else {
            // Always wake at least every 100 ms to drain stop / requests
            // that arrived between Wake() write and select() entry.
            tv.tv_sec  = 0;
            tv.tv_usec = 100 * 1000;
            tvp = &tv;
        }

        int sel = ::select(maxFd + 1, &rfds, nullptr, nullptr, tvp);
        if (sel < 0) {
            if (errno == EINTR) continue;
            // hard failure on select — bail out so we don't spin
            std::fprintf(stderr,
                "zeus-plughost: GuiThread select() failed errno=%d\n",
                errno);
            break;
        }

        if (sel > 0) {
            if (wakeFd >= 0 && FD_ISSET(wakeFd, &rfds)) {
                // Drain wakeups; non-blocking.
                char buf[64];
                while (::read(wakeFd, buf, sizeof(buf)) > 0) { /* drain */ }
            }
            // Dispatch any IRunLoop fds that are ready.
            std::vector<int> ready;
            for (int fd : runLoopFds) {
                if (FD_ISSET(fd, &rfds)) ready.push_back(fd);
            }
            if (!ready.empty() && runLoop_) {
                runLoop_->DispatchFds(ready);
            }
            // X events are handled at top of next loop iteration.
        }
    }

    // Tear down: close all editors, then the GUI thread exits.
    for (int i = 0; i < kMaxSlots; ++i) {
        if (windows_[i]) CloseEditorOnGuiThread(i, /*fireAsync=*/false);
    }
}

void GuiThread::DrainRequests() {
    std::deque<Request> local;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        local.swap(queue_);
    }
    for (auto& req : local) {
        HandleRequest(req);
        if (stopRequested_.load(std::memory_order_acquire)) break;
    }
}

void GuiThread::HandleRequest(const Request& req) {
    switch (req.kind) {
        case RequestKind::Show: HandleShow(req); break;
        case RequestKind::Hide: HandleHide(req); break;
        case RequestKind::Quit:
            stopRequested_.store(true, std::memory_order_release);
            break;
    }
}

void GuiThread::HandleShow(const Request& req) {
    auto signalReply = [&](const ShowResult& r) {
        if (req.showReply)  *req.showReply  = r;
        if (req.replyMutex && req.replyCv && req.replyReady) {
            std::lock_guard<std::mutex> lk(*req.replyMutex);
            *req.replyReady = true;
            req.replyCv->notify_one();
        }
    };

    if (req.slotIdx < 0 || req.slotIdx >= kMaxSlots) {
        signalReply(ShowResult{false, 6, 0, 0});
        return;
    }

    // Idempotent: if already open, return ok with current size.
    auto& slot = windows_[req.slotIdx];
    if (slot) {
        signalReply(ShowResult{true, 0, slot->Width(), slot->Height()});
        return;
    }
    if (req.view == nullptr) {
        // Caller (main.cpp) couldn't acquire a view — likely no editor.
        signalReply(ShowResult{false, 2, 0, 0});
        return;
    }

    auto win = std::make_unique<EditorWindow>(
        display_, req.slotIdx, req.title, runLoop_.get());
    std::uint8_t status = 5;
    bool ok = win->Attach(req.view, status);
    if (!ok) {
        signalReply(ShowResult{false, status, 0, 0});
        return;
    }
    int w = win->Width();
    int h = win->Height();
    slot = std::move(win);
    signalReply(ShowResult{true, 0, w, h});
}

void GuiThread::HandleHide(const Request& req) {
    auto signalReply = [&](bool wasOpen) {
        if (req.hideReply)  *req.hideReply = wasOpen;
        if (req.replyMutex && req.replyCv && req.replyReady) {
            std::lock_guard<std::mutex> lk(*req.replyMutex);
            *req.replyReady = true;
            req.replyCv->notify_one();
        }
    };
    if (req.slotIdx < 0 || req.slotIdx >= kMaxSlots) {
        signalReply(false);
        return;
    }
    if (!windows_[req.slotIdx]) {
        signalReply(false);
        return;
    }
    CloseEditorOnGuiThread(req.slotIdx, /*fireAsync=*/false);
    signalReply(true);
}

void GuiThread::CloseEditorOnGuiThread(int slotIdx, bool fireAsync) {
    if (slotIdx < 0 || slotIdx >= kMaxSlots) return;
    auto& slot = windows_[slotIdx];
    if (!slot) return;
    slot->Detach();
    slot.reset();
    if (fireAsync) {
        EnqueueAsyncEvent(AsyncEventTag::EditorClosed, slotIdx, 0, 0);
    }
}

void GuiThread::DispatchPendingXEvents() {
    if (display_ == nullptr) return;
    while (XPending(display_) > 0) {
        XEvent ev;
        XNextEvent(display_, &ev);
        // Match the event to a slot by window.
        int matchSlot = -1;
        for (int i = 0; i < kMaxSlots; ++i) {
            if (windows_[i] && windows_[i]->XWindow() == ev.xany.window) {
                matchSlot = i;
                break;
            }
        }
        if (matchSlot < 0) {
            // Event for a window we don't manage (plugin's child windows,
            // or stale events for a window we just destroyed). Ignore.
            continue;
        }
        auto& slot = windows_[matchSlot];
        const bool shouldClose = slot->OnEvent(ev);
        if (shouldClose) {
            CloseEditorOnGuiThread(matchSlot, /*fireAsync=*/true);
        } else if (ev.type == ConfigureNotify) {
            // Notify the host of resize even if it was external (e.g.
            // the user dragged the WM corner). The host doesn't strictly
            // need this for non-resizable plugins but it costs little.
            int w = slot->Width();
            int h = slot->Height();
            EnqueueAsyncEvent(AsyncEventTag::EditorResized,
                              matchSlot, w, h);
        }
    }
}

void GuiThread::EnqueueAsyncEvent(AsyncEventTag tag, int slot, int w, int h) {
    AsyncCallback cb;
    {
        std::lock_guard<std::mutex> lk(asyncMutex_);
        cb = asyncCallback_;
    }
    if (cb) cb(tag, slot, w, h);
}

}  // namespace zeus::plughost::vst3
