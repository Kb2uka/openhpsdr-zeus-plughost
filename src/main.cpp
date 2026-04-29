// main.cpp — sidecar entry point.
//
// Phase 1 contract:
//   - Parse --shm-name <NAME> and --control-pipe <PATH>.
//   - Allocate the input + output SPSC rings via ShmRingMapping (heap-
//     backed for now; real shm_open / CreateFileMapping in Phase 2).
//   - Open the control pipe (stub).
//   - Run the data-plane pass-through loop until SIGINT / SIGTERM.
//
// SIGKILL gate: see docs/PHASE1.md. The host MUST tolerate being killed
// without warning — that means no resources Zeus would lose if we die
// (audio rings live in shared memory, not the heap; control pipe close is
// best-effort).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "audio/block_format.h"
#include "audio/passthrough.h"
#include "ipc/control_pipe.h"
#include "ipc/shm_ring.h"
#include "ipc/wakeup.h"

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
        "  %s --shm-name <NAME> --control-pipe <PATH>\n"
        "\n"
        "REQUIRED ARGUMENTS:\n"
        "  --shm-name      Shared-memory name prefix used for input + output\n"
        "                  rings. The host appends '.in' and '.out'.\n"
        "  --control-pipe  Path (Linux/macOS AF_UNIX) or pipe name (Windows)\n"
        "                  for the control channel.\n"
        "\n"
        "Phase 1: data-plane pass-through only. No plugin loading yet.\n"
        "See docs/PHASE1.md.\n",
        argv0);
}

struct Args {
    std::string shmName;
    std::string controlPipe;
};

bool ParseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--shm-name") == 0 && i + 1 < argc) {
            out.shmName = argv[++i];
        } else if (std::strcmp(a, "--control-pipe") == 0 && i + 1 < argc) {
            out.controlPipe = argv[++i];
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            return false;
        } else {
            std::fprintf(stderr, "zeus-plughost: unrecognised argument '%s'\n", a);
            return false;
        }
    }
    return !out.shmName.empty() && !out.controlPipe.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT,  HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);

    using namespace zeus::plughost;

    std::fprintf(stderr,
        "zeus-plughost: starting (shm-name='%s', control-pipe='%s')\n",
        args.shmName.c_str(), args.controlPipe.c_str());

    try {
        ShmRingMapping inputMapping(args.shmName + ".in",
                                    kPhase1Frames, kPhase1Channels,
                                    kPhase1SampleRate, kPhase1RingDepth);
        ShmRingMapping outputMapping(args.shmName + ".out",
                                     kPhase1Frames, kPhase1Channels,
                                     kPhase1SampleRate, kPhase1RingDepth);

        ControlPipe control;
        if (!control.Open(args.controlPipe)) {
            std::fprintf(stderr, "zeus-plughost: failed to open control pipe\n");
            return 2;
        }

        Wakeup inputWakeup;
        PassthroughStats stats;

        // Heartbeat thread: 1 Hz log of processed / dropped counters. Lives
        // off the audio thread per the realtime-discipline contract.
        std::thread heartbeat([&]() {
            std::uint64_t lastProcessed = 0;
            std::uint64_t lastDropped   = 0;
            while (!g_stopFlag) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const std::uint64_t p = stats.processed.load(std::memory_order_relaxed);
                const std::uint64_t d = stats.dropped.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "zeus-plughost.heartbeat: processed=%llu (+%llu) dropped=%llu (+%llu)\n",
                    static_cast<unsigned long long>(p),
                    static_cast<unsigned long long>(p - lastProcessed),
                    static_cast<unsigned long long>(d),
                    static_cast<unsigned long long>(d - lastDropped));
                lastProcessed = p;
                lastDropped   = d;
            }
        });

        RunPassthrough(inputMapping.Ring(),
                       outputMapping.Ring(),
                       inputWakeup,
                       stats,
                       g_stopFlag);

        if (heartbeat.joinable()) {
            heartbeat.join();
        }

        control.Close();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "zeus-plughost: fatal: %s\n", ex.what());
        return 3;
    }

    std::fprintf(stderr, "zeus-plughost: clean exit\n");
    return 0;
}
