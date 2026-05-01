// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// main.cpp — sidecar entry point.
//
// Phase 2 contract:
//   - Parse --shm-name <SUFFIX> and --control-pipe <PATH>.
//   - Connect to the control socket (host bound it before launching us).
//   - Open the host-created shm regions /zeus-plughost-<SUFFIX>-h2s and
//     -s2h, plus the matching named semaphores.
//   - Send Hello, await HelloAck, then enter the audio pass-through loop.
//   - Exit cleanly on Goodbye, control socket EOF, or SIGINT/SIGTERM.
//
// SIGKILL gate: see docs/PHASE1.md. The host MUST tolerate being killed
// without warning — the kernel cleans our fds; the host owns the shm/sem
// names and unlinks them.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

#include "audio/block_format.h"
#include "audio/passthrough.h"
#include "ipc/control_pipe.h"
#include "ipc/shm_ring.h"
#include "ipc/wakeup.h"
#include "vst3/plugin_chain.h"
#include "vst3/plugin_host.h"

// Phase 3 GUI is Linux-only this wave; Win/Mac sidecar GUI lands in a
// later wave with platform-specific message-pump implementations.
#if defined(__linux__) && !defined(__APPLE__)
#  define ZEUS_PLUGHOST_HAS_GUI 1
#  include "vst3/gui_thread.h"
#  include "vst3/editor_idle_pump.h"
#  include "pluginterfaces/gui/iplugview.h"
#else
#  define ZEUS_PLUGHOST_HAS_GUI 0
#endif

#include <mutex>

namespace {

// Wired to SIGINT / SIGTERM so the audio loop can break out cleanly. Marked
// volatile so the (non-atomic) read inside the audio thread cannot be
// hoisted out of the loop. A signal handler writing through a sig_atomic_t
// is well-defined; we widen to bool here for ergonomic comparison.
volatile bool g_stopFlag = false;

extern "C" void HandleStopSignal(int /*sig*/) {
    g_stopFlag = true;
}

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "zeus-plughost — out-of-process VST/CLAP host sidecar for Zeus\n"
        "\n"
        "USAGE:\n"
        "  %s --shm-name <SUFFIX> --control-pipe <PATH>\n"
        "  %s --idle\n"
        "\n"
        "REQUIRED ARGUMENTS:\n"
        "  --shm-name      Suffix for the host-created shm + sem names. The\n"
        "                  sidecar appends '-h2s' and '-s2h' for the two ring\n"
        "                  directions, plus '-sem' for the wakeup semaphores.\n"
        "  --control-pipe  AF_UNIX socket path the host bound + listens on.\n"
        "\n"
        "OPTIONAL ARGUMENTS:\n"
        "  --idle          Test mode: no shm/control, heartbeat to stderr\n"
        "                  (used by Zeus.PluginHost.Tests Phase 1.5).\n"
        "\n"
        "Phase 2: cross-process pass-through round-trip. No plugin loading yet.\n"
        "See docs/proposals/vst-host-phase2-wire.md.\n",
        argv0, argv0);
}

struct Args {
    std::string shmName;
    std::string controlPipe;
    bool idle = false;
};

bool ParseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--shm-name") == 0 && i + 1 < argc) {
            out.shmName = argv[++i];
        } else if (std::strcmp(a, "--control-pipe") == 0 && i + 1 < argc) {
            out.controlPipe = argv[++i];
        } else if (std::strcmp(a, "--idle") == 0) {
            out.idle = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            return false;
        } else {
            std::fprintf(stderr, "zeus-plughost: unrecognised argument '%s'\n", a);
            return false;
        }
    }
    if (out.idle) {
        // --idle is exclusive: shm-name + control-pipe are not used.
        return true;
    }
    return !out.shmName.empty() && !out.controlPipe.empty();
}

// Idle/supervisor-test mode. Sleep in 1 s ticks emitting a heartbeat to
// stderr; SIGTERM/SIGINT flip g_stopFlag and we return 0. SIGKILL has no
// handler — the test harness uses that to verify Zeus's supervisor logic
// notices the death, restarts cleanly, and stays up itself.
int RunIdle() {
    std::signal(SIGINT,  HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);

#ifdef _WIN32
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(getpid());
#endif
    std::fprintf(stderr,
        "zeus-plughost: idle mode (pid=%ld) — heartbeat to stderr, SIGTERM exits 0\n",
        pid);

    std::uint64_t tick = 0;
    while (!g_stopFlag) {
        std::fprintf(stderr,
            "plughost idle pid=%ld tick=%llu\n",
            pid, static_cast<unsigned long long>(tick));
        std::fflush(stderr);
        ++tick;
        // Sleep in short chunks so SIGTERM is honoured promptly.
        for (int i = 0; i < 10 && !g_stopFlag; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::fprintf(stderr, "zeus-plughost: idle mode clean exit\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (args.idle) {
        return RunIdle();
    }

    std::signal(SIGINT,  HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
    // Don't crash if the control socket peer disappears mid-write; the
    // send() returns -1/EPIPE and we exit cleanly.
    std::signal(SIGPIPE, SIG_IGN);

    using namespace zeus::plughost;

    std::fprintf(stderr,
        "zeus-plughost: starting (shm-name='%s', control-pipe='%s')\n",
        args.shmName.c_str(), args.controlPipe.c_str());

    try {
        // 1. Connect to the control socket first. The host binds before
        //    launching us, so we should connect within ~50 ms; the retry
        //    loop tolerates up to 2 s of jitter.
        ControlPipe control;
        if (!control.Open(args.controlPipe, 2000)) {
            std::fprintf(stderr,
                "zeus-plughost: control socket connect failed (path=%s)\n",
                args.controlPipe.c_str());
            return 2;
        }

        // 2. Open shm regions (host already created and ftruncated them).
        const std::string h2sName = "/zeus-plughost-" + args.shmName + "-h2s";
        const std::string s2hName = "/zeus-plughost-" + args.shmName + "-s2h";
        ShmRingMapping inputMapping = ShmRingMapping::OpenExisting(
            h2sName,
            kPhase1Frames, kPhase1Channels, kPhase1SampleRate, kPhase1RingDepth);
        ShmRingMapping outputMapping = ShmRingMapping::OpenExisting(
            s2hName,
            kPhase1Frames, kPhase1Channels, kPhase1SampleRate, kPhase1RingDepth);

        // 3. Open the wakeup semaphores by name (host created them).
        const std::string h2sSem = "/zeus-plughost-" + args.shmName + "-h2s-sem";
        const std::string s2hSem = "/zeus-plughost-" + args.shmName + "-s2h-sem";
        Wakeup inputWakeup(h2sSem, /*create=*/false);
        Wakeup outputWakeup(s2hSem, /*create=*/false);

        // 4. Send Hello and await HelloAck before entering the audio loop.
        std::uint8_t helloPayload[16];
        const std::uint32_t protoVer = 1;
        const std::uint32_t sampleRate = kPhase1SampleRate;
        const std::uint32_t framesPerBlock = kPhase1Frames;
        const std::uint32_t channels = kPhase1Channels;
        std::memcpy(helloPayload + 0,  &protoVer,       4);
        std::memcpy(helloPayload + 4,  &sampleRate,     4);
        std::memcpy(helloPayload + 8,  &framesPerBlock, 4);
        std::memcpy(helloPayload + 12, &channels,       4);
        if (!control.Send(ControlMessageTag::Hello, helloPayload, sizeof(helloPayload))) {
            std::fprintf(stderr, "zeus-plughost: failed to send Hello\n");
            return 3;
        }

        ControlMessageTag tag{};
        std::vector<std::uint8_t> payload;
        bool closed = false;
        if (!control.Recv(tag, payload, /*timeoutMs=*/2000, closed) ||
            tag != ControlMessageTag::HelloAck) {
            std::fprintf(stderr,
                "zeus-plughost: HelloAck handshake failed (closed=%d)\n",
                closed ? 1 : 0);
            return 3;
        }
        std::fprintf(stderr,
            "zeus-plughost: handshake OK (proto=%u rate=%u frames=%u ch=%u)\n",
            protoVer, sampleRate, framesPerBlock, channels);

        // 8-slot serial plugin chain (Phase 3a). Master enable defaults
        // to OFF — the chain is bit-identical pass-through until the host
        // explicitly turns it on. Slot 0 alone preserves the Phase 2
        // single-slot wire (LoadPlugin / UnloadPlugin).
        vst3::PluginChain pluginChain(
            static_cast<double>(kPhase1SampleRate),
            static_cast<std::int32_t>(kPhase1Frames));

#if ZEUS_PLUGHOST_HAS_GUI
        // Phase 3 GUI: lazy GUI thread. Started on first SlotShowEditor.
        vst3::GuiThread guiThread;
#endif

        // Two writers will hit the control socket: the control-reader
        // thread (sync replies) and the GUI thread (async EditorClosed /
        // EditorResized). ControlPipe::Send is not internally
        // synchronised, so serialise here.
        std::mutex controlSendMutex;
        auto sendFrame = [&](ControlMessageTag tag,
                             const std::uint8_t* data, std::size_t size) {
            std::lock_guard<std::mutex> lk(controlSendMutex);
            return control.Send(tag, data, size);
        };
        // Non-blocking variant for lossy events. Drops if the socket's
        // send queue would block — used for ParamChanged at editor-knob
        // rates, where the .NET reader can't keep up and a blocking write
        // would freeze the plugin's GUI thread (rack-observed: VST2 ZAM
        // plugins fire automate per pixel of slider drag).
        auto trySendFrameLossy = [&](ControlMessageTag tag,
                                     const std::uint8_t* data, std::size_t size) {
            std::lock_guard<std::mutex> lk(controlSendMutex);
            return control.TrySendNonBlocking(tag, data, size);
        };

        // ---- helpers: little-endian byte appenders ----
        auto appendU8 = [](std::vector<std::uint8_t>& body, std::uint8_t v) {
            body.push_back(v);
        };
        auto appendU32Le = [](std::vector<std::uint8_t>& body, std::uint32_t v) {
            body.push_back(static_cast<std::uint8_t>(v & 0xFFu));
            body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
            body.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
            body.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
        };
        auto appendI32Le = [&](std::vector<std::uint8_t>& body, std::int32_t v) {
            appendU32Le(body, static_cast<std::uint32_t>(v));
        };
        auto appendF64Le = [](std::vector<std::uint8_t>& body, double v) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 0; i < 8; ++i) {
                body.push_back(static_cast<std::uint8_t>(
                    (bits >> (i * 8)) & 0xFFu));
            }
        };
        auto appendString = [&](std::vector<std::uint8_t>& body,
                                const std::string& s) {
            appendU32Le(body, static_cast<std::uint32_t>(s.size()));
            body.insert(body.end(), s.begin(), s.end());
        };

        // ---- helpers: little-endian payload decoders ----
        auto readU32Le = [](const std::vector<std::uint8_t>& p, std::size_t off) {
            return static_cast<std::uint32_t>(p[off])
                 | (static_cast<std::uint32_t>(p[off + 1]) << 8)
                 | (static_cast<std::uint32_t>(p[off + 2]) << 16)
                 | (static_cast<std::uint32_t>(p[off + 3]) << 24);
        };
        auto readF64Le = [](const std::vector<std::uint8_t>& p, std::size_t off) {
            std::uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) {
                bits |= static_cast<std::uint64_t>(p[off + i]) << (i * 8);
            }
            double v = 0;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        };

        // ---- send helpers (each composes a payload + ships it) ----
        auto buildLoadResultBody =
            [&](std::uint8_t status,
                const std::string& name,
                const std::string& vendor,
                const std::string& version,
                const std::string& errorMessage,
                std::vector<std::uint8_t>& body) {
            body.clear();
            body.push_back(status);
            if (status == 0) {
                appendString(body, name);
                appendString(body, vendor);
                appendString(body, version);
            } else {
                appendString(body, errorMessage);
            }
        };

        auto sendLoadResult =
            [&](std::uint8_t status,
                const std::string& name,
                const std::string& vendor,
                const std::string& version,
                const std::string& errorMessage) {
            std::vector<std::uint8_t> body;
            body.reserve(64 + name.size() + vendor.size()
                              + version.size() + errorMessage.size());
            buildLoadResultBody(status, name, vendor, version, errorMessage, body);
            sendFrame(ControlMessageTag::LoadPluginResult,
                      body.data(), body.size());
        };

        auto sendUnloadResult = [&](std::uint8_t status) {
            std::uint8_t one = status;
            sendFrame(ControlMessageTag::UnloadPluginResult, &one, 1);
        };

        auto sendSlotLoadResult =
            [&](std::uint8_t slot,
                std::uint8_t status,
                const std::string& name,
                const std::string& vendor,
                const std::string& version,
                const std::string& errorMessage) {
            std::vector<std::uint8_t> body;
            body.reserve(64 + name.size() + vendor.size()
                              + version.size() + errorMessage.size());
            body.push_back(slot);
            body.push_back(status);
            if (status == 0) {
                appendString(body, name);
                appendString(body, vendor);
                appendString(body, version);
            } else {
                appendString(body, errorMessage);
            }
            sendFrame(ControlMessageTag::SlotLoadPluginResult,
                      body.data(), body.size());
        };

        auto sendSlotUnloadResult = [&](std::uint8_t slot, std::uint8_t status) {
            std::uint8_t buf[2] = { slot, status };
            sendFrame(ControlMessageTag::SlotUnloadPluginResult, buf, 2);
        };

        auto sendSlotBypassResult = [&](std::uint8_t slot, std::uint8_t status) {
            std::uint8_t buf[2] = { slot, status };
            sendFrame(ControlMessageTag::SlotSetBypassResult, buf, 2);
        };

        auto sendChainEnabledResult = [&](std::uint8_t status) {
            std::uint8_t one = status;
            sendFrame(ControlMessageTag::SetChainEnabledResult, &one, 1);
        };

        auto sendSlotParamListResult =
            [&](std::uint8_t slot, std::uint8_t status,
                const std::vector<vst3::ParamInfo>& params) {
            std::vector<std::uint8_t> body;
            body.push_back(slot);
            body.push_back(status);
            if (status == 0) {
                appendU32Le(body, static_cast<std::uint32_t>(params.size()));
                for (const auto& p : params) {
                    appendU32Le(body, p.id);
                    appendString(body, p.name);
                    appendString(body, p.units);
                    appendF64Le(body, p.defaultValue);
                    appendF64Le(body, p.currentValue);
                    appendI32Le(body, p.stepCount);
                    appendU8(body, p.flags);
                }
            }
            sendFrame(ControlMessageTag::SlotParamListResult,
                      body.data(), body.size());
        };

        auto sendSlotSetParamResult =
            [&](std::uint8_t slot, std::uint32_t paramId,
                std::uint8_t status, double actual) {
            std::vector<std::uint8_t> body;
            body.reserve(14);
            body.push_back(slot);
            appendU32Le(body, paramId);
            body.push_back(status);
            appendF64Le(body, actual);
            sendFrame(ControlMessageTag::SlotSetParamResult,
                      body.data(), body.size());
        };

        auto sendSlotShowEditorResult =
            [&](std::uint8_t slot, std::uint8_t status,
                std::uint32_t width, std::uint32_t height) {
            std::vector<std::uint8_t> body;
            body.reserve(10);
            body.push_back(slot);
            body.push_back(status);
            if (status == 0) {
                appendU32Le(body, width);
                appendU32Le(body, height);
            }
            sendFrame(ControlMessageTag::SlotShowEditorResult,
                      body.data(), body.size());
        };

        auto sendSlotHideEditorResult = [&](std::uint8_t slot,
                                            std::uint8_t status) {
            std::uint8_t buf[2] = { slot, status };
            sendFrame(ControlMessageTag::SlotHideEditorResult, buf, 2);
        };

        // Async events (0x34 EditorClosed, 0x35 EditorResized) — fire
        // unsolicited from the GUI thread. .NET dispatches them via
        // event handlers, not awaiting tasks.
        auto sendEditorClosed = [&](std::uint8_t slot) {
            sendFrame(ControlMessageTag::EditorClosed, &slot, 1);
        };
        auto sendEditorResized = [&](std::uint8_t slot,
                                     std::uint32_t width,
                                     std::uint32_t height) {
            std::vector<std::uint8_t> body;
            body.reserve(9);
            body.push_back(slot);
            appendU32Le(body, width);
            appendU32Le(body, height);
            sendFrame(ControlMessageTag::EditorResized,
                      body.data(), body.size());
        };

        // Wave 7 — ParamChanged async sender. Wire format: u8 slot + u32 paramId
        // + f64 normalizedValue (13 bytes). Fires from any thread the plugin
        // calls performEdit on (typically the editor / GUI thread, but plugins
        // are permitted to fire from audio threads too — sendFrame mutex
        // serialises and the worst case is microsecond contention).
        auto sendParamChanged = [&](std::uint8_t slot, std::uint32_t paramId,
                                    double normalizedValue) {
            std::vector<std::uint8_t> body;
            body.reserve(13);
            body.push_back(slot);
            appendU32Le(body, paramId);
            appendF64Le(body, normalizedValue);
            // Lossy: drop on socket back-pressure. Latest-wins semantics
            // is correct for slider drags — the .NET host will get the
            // newer value on the next un-throttled tick. The previous
            // blocking-Send path stalled the plugin's GUI thread when
            // the .NET reader briefly fell behind, freezing the editor.
            trySendFrameLossy(ControlMessageTag::ParamChanged,
                              body.data(), body.size());
        };

        // Wave 7 — chain-level performEdit bridge. Every editor knob drag
        // (and every internal automation event) lands here as
        // (slot, paramId, normalizedValue). We just serialise into a
        // ParamChanged frame; the host (.NET) updates ChainSlot.Parameters
        // and debounce-saves to LiteDB. Bound here rather than inside the
        // chain so per-slot wrappers reuse the same sendParamChanged
        // closure without a heap-alloc per fire.
        pluginChain.SetParamChangedCallback(
            [&](int slotIdx, std::uint32_t paramId, double normalizedValue) {
                if (slotIdx < 0 || slotIdx > 0xFF) return;
                sendParamChanged(static_cast<std::uint8_t>(slotIdx),
                                 paramId, normalizedValue);
            });

#if ZEUS_PLUGHOST_HAS_GUI
        // The GUI thread will call this from its own thread when the
        // operator closes the WM frame or the plugin requests a resize.
        guiThread.SetAsyncCallback(
            [&](vst3::GuiThread::AsyncEventTag tag,
                int slotIdx, int w, int h) {
            if (slotIdx < 0 || slotIdx > 0xFF) return;
            const auto slotByte = static_cast<std::uint8_t>(slotIdx);
            if (tag == vst3::GuiThread::AsyncEventTag::EditorClosed) {
                // The GUI thread has already torn the editor down. Drop
                // our refcount on the IPlugView so re-Show can re-acquire.
                pluginChain.ReleaseEditorView(slotIdx);
                sendEditorClosed(slotByte);
            } else if (tag == vst3::GuiThread::AsyncEventTag::EditorResized) {
                if (w < 0) w = 0;
                if (h < 0) h = 0;
                sendEditorResized(slotByte,
                    static_cast<std::uint32_t>(w),
                    static_cast<std::uint32_t>(h));
            }
        });
#else
        (void)sendEditorClosed; (void)sendEditorResized;
#endif

        // 5. Background control-read thread. Reads Goodbye / Heartbeat /
        //    LoadPlugin / UnloadPlugin while the audio thread runs the
        //    pass-through loop. Plugin Load/Unload calls run on this
        //    thread (slow first-load is allowed); Process() runs on the
        //    audio thread.
        std::atomic<bool> stopFromControl{false};
        std::thread controlReader([&]() {
            while (!g_stopFlag && !stopFromControl.load(std::memory_order_relaxed)) {
                ControlMessageTag t{};
                std::vector<std::uint8_t> p;
                bool isClosed = false;
                bool ok = control.Recv(t, p, /*timeoutMs=*/200, isClosed);
                if (!ok) {
                    if (isClosed) {
                        stopFromControl.store(true, std::memory_order_relaxed);
                        return;
                    }
                    // timeout: loop and check stopFlag
                    continue;
                }
                if (t == ControlMessageTag::Goodbye) {
                    stopFromControl.store(true, std::memory_order_relaxed);
                    return;
                }
                if (t == ControlMessageTag::LoadPlugin) {
                    // Backwards-compat: 0x10 LoadPlugin == slot-0 SlotLoadPlugin.
                    if (p.size() < 4) {
                        sendLoadResult(5, {}, {}, {}, "LoadPlugin payload truncated");
                        continue;
                    }
                    std::uint32_t pathLen = readU32Le(p, 0);
                    if (p.size() != 4u + pathLen) {
                        sendLoadResult(5, {}, {}, {},
                            "LoadPlugin payload size mismatch");
                        continue;
                    }
                    std::string path(reinterpret_cast<const char*>(p.data() + 4),
                                     pathLen);
#if ZEUS_PLUGHOST_HAS_GUI
                    if (guiThread.IsRunning()) {
                        if (guiThread.RequestHide(0)) {
                            pluginChain.ReleaseEditorView(0);
                            std::uint8_t s0 = 0;
                            sendFrame(ControlMessageTag::EditorClosed, &s0, 1);
                        }
                    }
#endif
                    auto result = pluginChain.LoadSlot(0, path);
                    if (auto* info = std::get_if<vst3::LoadInfo>(&result)) {
                        std::fprintf(stderr,
                            "zeus-plughost: slot=0 loaded plugin name='%s' "
                            "vendor='%s' version='%s'\n",
                            info->name.c_str(), info->vendor.c_str(),
                            info->version.c_str());
                        sendLoadResult(0, info->name, info->vendor, info->version, {});
                    } else {
                        const auto& err = std::get<vst3::LoadError>(result);
                        std::fprintf(stderr,
                            "zeus-plughost: slot=0 load failed status=%u msg='%s'\n",
                            static_cast<unsigned>(err.status),
                            err.message.c_str());
                        sendLoadResult(err.status, {}, {}, {}, err.message);
                    }
                    continue;
                }
                if (t == ControlMessageTag::UnloadPlugin) {
                    // Backwards-compat: 0x12 == slot-0 SlotUnloadPlugin.
                    // Auto-close any open editor for this slot first.
                    if (guiThread.IsRunning()) {
                        if (guiThread.RequestHide(0)) {
                            pluginChain.ReleaseEditorView(0);
                            // Surface the auto-close as an async event so
                            // the host UI can refresh its "edit" state.
                            std::uint8_t s0 = 0;
                            sendFrame(ControlMessageTag::EditorClosed, &s0, 1);
                        }
                    }
                    std::uint8_t st = pluginChain.UnloadSlot(0);
                    sendUnloadResult(st);
                    continue;
                }
                if (t == ControlMessageTag::SlotLoadPlugin) {
                    // Payload: u8 slot + u32 pathLen + UTF-8 path.
                    if (p.size() < 5) {
                        sendSlotLoadResult(0xFFu, 5, {}, {}, {},
                            "SlotLoadPlugin payload truncated");
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    std::uint32_t pathLen = readU32Le(p, 1);
                    if (p.size() != 5u + pathLen) {
                        sendSlotLoadResult(slot, 5, {}, {}, {},
                            "SlotLoadPlugin payload size mismatch");
                        continue;
                    }
                    std::string path(reinterpret_cast<const char*>(p.data() + 5),
                                     pathLen);
#if ZEUS_PLUGHOST_HAS_GUI
                    if (guiThread.IsRunning() &&
                        slot < vst3::PluginChain::kMaxSlots) {
                        if (guiThread.RequestHide(static_cast<int>(slot))) {
                            pluginChain.ReleaseEditorView(
                                static_cast<int>(slot));
                            sendFrame(ControlMessageTag::EditorClosed, &slot, 1);
                        }
                    }
#endif
                    auto result = pluginChain.LoadSlot(static_cast<int>(slot), path);
                    if (auto* info = std::get_if<vst3::LoadInfo>(&result)) {
                        std::fprintf(stderr,
                            "zeus-plughost: slot=%u loaded plugin name='%s'\n",
                            slot, info->name.c_str());
                        sendSlotLoadResult(slot, 0,
                            info->name, info->vendor, info->version, {});
                    } else {
                        const auto& err = std::get<vst3::LoadError>(result);
                        std::fprintf(stderr,
                            "zeus-plughost: slot=%u load failed status=%u msg='%s'\n",
                            slot, static_cast<unsigned>(err.status),
                            err.message.c_str());
                        sendSlotLoadResult(slot, err.status, {}, {}, {}, err.message);
                    }
                    continue;
                }
                if (t == ControlMessageTag::SlotUnloadPlugin) {
                    if (p.size() != 1) {
                        sendSlotUnloadResult(0xFFu, 5);
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    // Auto-close any open editor first; emit async so
                    // the host UI knows to forget its "open" state.
#if ZEUS_PLUGHOST_HAS_GUI
                    if (guiThread.IsRunning() &&
                        slot < vst3::PluginChain::kMaxSlots) {
                        if (guiThread.RequestHide(static_cast<int>(slot))) {
                            pluginChain.ReleaseEditorView(
                                static_cast<int>(slot));
                            sendFrame(ControlMessageTag::EditorClosed, &slot, 1);
                        }
                    }
#endif
                    std::uint8_t st = pluginChain.UnloadSlot(static_cast<int>(slot));
                    sendSlotUnloadResult(slot, st);
                    continue;
                }
                if (t == ControlMessageTag::SlotSetBypass) {
                    if (p.size() != 2) {
                        sendSlotBypassResult(0xFFu, 5);
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    bool bypass = (p[1] != 0);
                    std::uint8_t st = pluginChain.SetSlotBypass(
                        static_cast<int>(slot), bypass);
                    sendSlotBypassResult(slot, st);
                    continue;
                }
                if (t == ControlMessageTag::SetChainEnabled) {
                    if (p.size() != 1) {
                        sendChainEnabledResult(5);
                        continue;
                    }
                    bool enabled = (p[0] != 0);
                    pluginChain.SetChainEnabled(enabled);
                    sendChainEnabledResult(0);
                    continue;
                }
                if (t == ControlMessageTag::SlotListParams) {
                    if (p.size() != 1) {
                        sendSlotParamListResult(0xFFu, 5, {});
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    std::vector<vst3::ParamInfo> params;
                    std::uint8_t st = pluginChain.ListParams(
                        static_cast<int>(slot), params);
                    sendSlotParamListResult(slot, st, params);
                    continue;
                }
                if (t == ControlMessageTag::SlotSetParam) {
                    // Payload: u8 slot + u32 paramId + f64 normalized = 13 bytes.
                    if (p.size() != 13) {
                        sendSlotSetParamResult(0xFFu, 0, 5, 0.0);
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    std::uint32_t paramId = readU32Le(p, 1);
                    double normalized = readF64Le(p, 5);
                    double actual = 0.0;
                    std::uint8_t st = pluginChain.SetParam(
                        static_cast<int>(slot), paramId, normalized, actual);
                    sendSlotSetParamResult(slot, paramId, st, actual);
                    continue;
                }
#if ZEUS_PLUGHOST_HAS_GUI
                if (t == ControlMessageTag::SlotShowEditor) {
                    if (p.size() != 1) {
                        sendSlotShowEditorResult(0xFFu, 5, 0, 0);
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    if (slot >= vst3::PluginChain::kMaxSlots) {
                        sendSlotShowEditorResult(slot, 6, 0, 0);
                        continue;
                    }
                    if (!pluginChain.IsSlotLoaded(static_cast<int>(slot))) {
                        sendSlotShowEditorResult(slot, 1, 0, 0);
                        continue;
                    }
                    if (!guiThread.Start()) {
                        sendSlotShowEditorResult(slot, 7, 0, 0);
                        continue;
                    }
                    Steinberg::IPlugView* view =
                        pluginChain.AcquireEditorView(static_cast<int>(slot));
                    if (view == nullptr) {
                        sendSlotShowEditorResult(slot, 2, 0, 0);
                        continue;
                    }
                    // Companion idle pump for VST2 / CLAP wrappers; null
                    // for VST3 (those drive themselves via IRunLoop).
                    vst3::IEditorIdlePump* idlePump =
                        pluginChain.AcquireEditorIdlePump(static_cast<int>(slot));
                    // Use the loaded plugin's display name as the title.
                    // (PluginChain doesn't expose an info getter; use a
                    // generic fallback. Future polish: surface name via
                    // CurrentInfo on slot.)
                    std::string title = "Plugin Editor — slot " + std::to_string(slot);
                    auto rsp = guiThread.RequestShow(
                        static_cast<int>(slot), view, idlePump, title);
                    if (!rsp.ok) {
                        // Drop our ref; on next show we'll createView again.
                        pluginChain.ReleaseEditorView(static_cast<int>(slot));
                        sendSlotShowEditorResult(slot, rsp.status, 0, 0);
                        continue;
                    }
                    sendSlotShowEditorResult(slot, 0,
                        static_cast<std::uint32_t>(rsp.width),
                        static_cast<std::uint32_t>(rsp.height));
                    continue;
                }
                if (t == ControlMessageTag::SlotHideEditor) {
                    if (p.size() != 1) {
                        sendSlotHideEditorResult(0xFFu, 5);
                        continue;
                    }
                    std::uint8_t slot = p[0];
                    if (slot >= vst3::PluginChain::kMaxSlots) {
                        sendSlotHideEditorResult(slot, 6);
                        continue;
                    }
                    if (!guiThread.IsRunning()) {
                        sendSlotHideEditorResult(slot, 1);
                        continue;
                    }
                    bool wasOpen = guiThread.RequestHide(
                        static_cast<int>(slot));
                    if (wasOpen) {
                        pluginChain.ReleaseEditorView(static_cast<int>(slot));
                        sendSlotHideEditorResult(slot, 0);
                    } else {
                        sendSlotHideEditorResult(slot, 1);
                    }
                    continue;
                }
#else
                if (t == ControlMessageTag::SlotShowEditor) {
                    if (p.size() != 1) {
                        sendSlotShowEditorResult(0xFFu, 5, 0, 0);
                        continue;
                    }
                    sendSlotShowEditorResult(p[0], 3, 0, 0); // platform-not-supported
                    continue;
                }
                if (t == ControlMessageTag::SlotHideEditor) {
                    if (p.size() != 1) {
                        sendSlotHideEditorResult(0xFFu, 5);
                        continue;
                    }
                    sendSlotHideEditorResult(p[0], 1);
                    continue;
                }
#endif
                // Heartbeat or anything else: ignore silently.
            }
        });

        PassthroughStats stats;

        // Heartbeat thread: 1 Hz log of processed / dropped counters. Lives
        // off the audio thread per the realtime-discipline contract.
        std::thread heartbeat([&]() {
            std::uint64_t lastProcessed = 0;
            std::uint64_t lastDropped   = 0;
            while (!g_stopFlag && !stopFromControl.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const std::uint64_t pp = stats.processed.load(std::memory_order_relaxed);
                const std::uint64_t dd = stats.dropped.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "zeus-plughost.heartbeat: processed=%llu (+%llu) dropped=%llu (+%llu)\n",
                    static_cast<unsigned long long>(pp),
                    static_cast<unsigned long long>(pp - lastProcessed),
                    static_cast<unsigned long long>(dd),
                    static_cast<unsigned long long>(dd - lastDropped));
                lastProcessed = pp;
                lastDropped   = dd;
            }
        });

        // The pass-through loop reads the input ring on the h2s wakeup,
        // runs the plugin if loaded (otherwise memcpy), and posts on the
        // s2h wakeup. The PassthroughStats counter increments per round-trip.
        RunPassthrough(inputMapping.Ring(),
                       outputMapping.Ring(),
                       inputWakeup,
                       outputWakeup,
                       stats,
                       g_stopFlag,
                       stopFromControl,
                       &pluginChain);

        if (heartbeat.joinable())     heartbeat.join();
        if (controlReader.joinable()) controlReader.join();

#if ZEUS_PLUGHOST_HAS_GUI
        // Detach the async callback BEFORE Stop() so any final teardown
        // events don't try to write to a control socket we're closing.
        guiThread.SetAsyncCallback({});
        guiThread.Stop();
#endif

        control.Close();
        // Mappings + wakeups close via their dtors; we don't shm_unlink
        // because the host owns the names.
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "zeus-plughost: fatal: %s\n", ex.what());
        return 4;
    }

    std::fprintf(stderr, "zeus-plughost: clean exit\n");
    return 0;
}