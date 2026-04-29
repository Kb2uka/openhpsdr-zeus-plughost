// control_pipe.cpp — Phase 2 AF_UNIX SOCK_STREAM client impl.
//
// We are the sidecar (client). The .NET host binds the path before
// launching us, so a connect() can fail with ENOENT until the bind
// completes — handled via a 50 ms retry loop totalling up to `timeoutMs`.

#include "ipc/control_pipe.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace zeus::plughost {

namespace {

constexpr std::size_t kMaxPayloadBytes = 1u * 1024u * 1024u;

}  // namespace

ControlPipe::ControlPipe() = default;

ControlPipe::~ControlPipe() {
    Close();
}

bool ControlPipe::Open(const std::string& path, std::uint32_t timeoutMs) {
    if (path.empty()) return false;
    path_ = path;

    if (path.size() >= sizeof(sockaddr_un().sun_path)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);

    while (true) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fd_ = fd;
            return true;
        }

        const int err = errno;
        ::close(fd);

        // Host might not have bound yet — retry until deadline.
        if ((err == ENOENT || err == ECONNREFUSED) &&
            std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        return false;
    }
}

bool ControlPipe::Send(ControlMessageTag tag,
                       const std::uint8_t* data,
                       std::size_t size) {
    if (fd_ < 0) return false;
    if (size > kMaxPayloadBytes) return false;

    const std::uint32_t length = static_cast<std::uint32_t>(size + 1);
    std::uint8_t header[5];
    header[0] = static_cast<std::uint8_t>(length & 0xFFu);
    header[1] = static_cast<std::uint8_t>((length >> 8) & 0xFFu);
    header[2] = static_cast<std::uint8_t>((length >> 16) & 0xFFu);
    header[3] = static_cast<std::uint8_t>((length >> 24) & 0xFFu);
    header[4] = static_cast<std::uint8_t>(tag);

    std::size_t sent = 0;
    while (sent < sizeof(header)) {
        ssize_t n = ::send(fd_, header + sent, sizeof(header) - sent,
                           MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }

    sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd_, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool ControlPipe::SendLog(const std::string& text) {
    return Send(ControlMessageTag::LogLine,
                reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size());
}

bool ControlPipe::RecvAll(std::uint8_t* dst, std::size_t n,
                          std::uint32_t timeoutMs, bool& outClosed) {
    outClosed = false;
    if (fd_ < 0) return false;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);

    std::size_t got = 0;
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;

        const auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, static_cast<int>(remainMs));
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) return false;  // timeout
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
            (pfd.revents & POLLIN) == 0) {
            outClosed = true;
            return false;
        }

        ssize_t r = ::recv(fd_, dst + got, n - got, 0);
        if (r == 0) {
            outClosed = true;
            return false;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool ControlPipe::Recv(ControlMessageTag& outTag,
                       std::vector<std::uint8_t>& outPayload,
                       std::uint32_t timeoutMs,
                       bool& outClosed) {
    outClosed = false;
    if (fd_ < 0) return false;

    std::uint8_t lenBuf[4];
    if (!RecvAll(lenBuf, 4, timeoutMs, outClosed)) return false;

    const std::uint32_t length =
        (static_cast<std::uint32_t>(lenBuf[0])      ) |
        (static_cast<std::uint32_t>(lenBuf[1]) <<  8) |
        (static_cast<std::uint32_t>(lenBuf[2]) << 16) |
        (static_cast<std::uint32_t>(lenBuf[3]) << 24);
    if (length == 0 || length > kMaxPayloadBytes) return false;

    std::uint8_t tagByte = 0;
    if (!RecvAll(&tagByte, 1, timeoutMs, outClosed)) return false;
    outTag = static_cast<ControlMessageTag>(tagByte);

    const std::size_t payloadBytes = length - 1u;
    outPayload.resize(payloadBytes);
    if (payloadBytes > 0) {
        if (!RecvAll(outPayload.data(), payloadBytes, timeoutMs, outClosed)) {
            return false;
        }
    }
    return true;
}

void ControlPipe::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace zeus::plughost
