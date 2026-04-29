// control_pipe.h — control-channel abstraction (Phase 2).
//
// AF_UNIX SOCK_STREAM client on the sidecar side. The host (.NET) is the
// server: it bind()s and listen()s before forking us; we connect with
// retry. Wire framing is fixed-length (4-byte little-endian payload size,
// then 1-byte type, then `length - 1` bytes of payload).
//
// Phase 2 message types (see docs/proposals/vst-host-phase2-wire.md):
//   0x01 Hello              sidecar -> host (16 byte payload)
//   0x02 HelloAck           host -> sidecar (no payload)
//   0x03 Heartbeat          bidirectional (optional Phase 2)
//   0x04 Goodbye            host -> sidecar (no payload)
//   0x05 LogLine            sidecar -> host (UTF-8 bytes)
//
// Plugin lifecycle (Phase 2 real):
//   0x10 LoadPlugin         host -> sidecar (u32 pathLen + UTF-8 path)
//   0x11 LoadPluginResult   sidecar -> host (status u8 + name/vendor/version | error)
//   0x12 UnloadPlugin       host -> sidecar (no payload)
//   0x13 UnloadPluginResult sidecar -> host (status u8)
//
// Parameter-change messages are Phase 3+.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zeus::plughost {

enum class ControlMessageTag : std::uint8_t {
    Hello              = 0x01,
    HelloAck           = 0x02,
    Heartbeat          = 0x03,
    Goodbye            = 0x04,
    LogLine            = 0x05,
    LoadPlugin         = 0x10,
    LoadPluginResult   = 0x11,
    UnloadPlugin       = 0x12,
    UnloadPluginResult = 0x13,
};

class ControlPipe {
public:
    ControlPipe();
    ~ControlPipe();

    ControlPipe(const ControlPipe&)            = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;

    // Open the AF_UNIX SOCK_STREAM client connection. Retries every 50 ms
    // up to ~`timeoutMs` total to handle the race where the host has not
    // yet bound the socket. Returns false on permanent error.
    bool Open(const std::string& path, std::uint32_t timeoutMs = 2000);

    // Send one length-prefixed message. Synchronous write.
    bool Send(ControlMessageTag tag,
              const std::uint8_t* data,
              std::size_t size);

    // Send a LogLine message containing `text`.
    bool SendLog(const std::string& text);

    // Receive one length-prefixed message, blocking until either a
    // complete frame arrives, the peer closes (returns false with
    // outClosed=true), or `timeoutMs` elapses.
    bool Recv(ControlMessageTag& outTag,
              std::vector<std::uint8_t>& outPayload,
              std::uint32_t timeoutMs,
              bool& outClosed);

    void Close();

    bool IsOpen() const { return fd_ >= 0; }
    const std::string& Path() const { return path_; }

private:
    bool RecvAll(std::uint8_t* dst, std::size_t n,
                 std::uint32_t timeoutMs, bool& outClosed);

    std::string path_;
    int         fd_ = -1;
};

}  // namespace zeus::plughost
