// control_pipe.h — control-channel abstraction.
//
// Phase 1: stub. The class exposes Open / Send / Recv / Close with a
// length-prefixed byte-blob API. Real CBOR framing is deferred to Phase 2
// (TODO: tinycbor or similar MIT/BSD lib).
//
// Transport choice (per build target):
//
//   - Linux / macOS : AF_UNIX SOCK_SEQPACKET socket bound to the path
//                     passed via --control-pipe.
//   - Windows       : Named pipe (\\.\pipe\<name>) created with
//                     CreateNamedPipeA.
//
// Phase 1 leaves both impls as no-op stubs that succeed without doing any
// real I/O so the rest of the host can compile + run.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zeus::plughost {

// Phase 1 message tag enum. These names are stable across phases; only the
// payload framing changes (raw bytes -> CBOR map) when Phase 2 ships.
enum class ControlMessageTag : std::uint16_t {
    Hello     = 0x0001,  // host <- Zeus  : negotiate format, plugin path, ...
    Goodbye   = 0x0002,  // either way    : graceful shutdown
    Heartbeat = 0x0003,  // host -> Zeus  : 1 Hz liveness ping
    LogLine   = 0x0004,  // host -> Zeus  : structured log forwarding
};

class ControlPipe {
public:
    ControlPipe();
    ~ControlPipe();

    ControlPipe(const ControlPipe&)            = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;

    // Open the platform endpoint named by `path`. On Linux/macOS this is
    // an AF_UNIX path; on Windows it is the suffix of \\.\pipe\.
    // Returns false on error (errno / GetLastError() unchanged).
    //
    // Phase 1: this records the name and returns true without opening any
    // OS resource.
    bool Open(const std::string& path);

    // Send / Recv a single message. Length-prefixed framing is the caller's
    // problem until Phase 2 lands.
    bool Send(ControlMessageTag tag, const std::uint8_t* data, std::size_t size);
    bool Recv(ControlMessageTag& outTag, std::vector<std::uint8_t>& outPayload);

    void Close();

    bool IsOpen() const { return open_; }
    const std::string& Path() const { return path_; }

private:
    std::string path_;
    bool        open_ = false;
};

}  // namespace zeus::plughost
