// control_pipe.cpp — Phase 1 stub impl.
//
// All transport calls return success without touching the OS. The shape of
// the API is locked so the caller (main.cpp) can be written against the
// real interface today and we only fill in the Linux AF_UNIX / Windows
// named-pipe code in Phase 2.
//
// TODO(phase2):
//   - Linux/macOS: socket(AF_UNIX, SOCK_SEQPACKET, 0) + bind/connect.
//   - Windows   : CreateNamedPipeA / ConnectNamedPipe.
//   - Replace the raw byte payload with CBOR-encoded structured messages
//     (tinycbor: https://github.com/intel/tinycbor, MIT-licensed).

#include "ipc/control_pipe.h"

namespace zeus::plughost {

ControlPipe::ControlPipe() = default;

ControlPipe::~ControlPipe() {
    Close();
}

bool ControlPipe::Open(const std::string& path) {
    path_ = path;
    open_ = true;  // Phase 1: pretend we connected.
    return true;
}

bool ControlPipe::Send(ControlMessageTag tag,
                       const std::uint8_t* data,
                       std::size_t size) {
    (void)tag;
    (void)data;
    (void)size;
    return open_;  // Phase 1: drop on the floor.
}

bool ControlPipe::Recv(ControlMessageTag& outTag,
                       std::vector<std::uint8_t>& outPayload) {
    (void)outTag;
    (void)outPayload;
    return false;  // Phase 1: never reports a message available.
}

void ControlPipe::Close() {
    open_ = false;
}

}  // namespace zeus::plughost
