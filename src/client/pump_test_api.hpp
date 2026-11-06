#pragma once

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "zet/core/time.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

/// Test seam: runs the client's event loop against `Addresses`, skipping the
/// SSH bootstrap. Connecting, reconnecting and the loop are production code.
namespace zet::client {

struct SessionTestConfig {
    wire::SessionSecrets Secrets;
    session::SessionLimits Limits;
    io::AddressList Addresses;
    int StdoutFd{STDOUT_FILENO};
    int StderrFd{STDERR_FILENO};
    /// A non-blocking fd the client reads typed input from; -1 means the
    /// session has no input (it is ended from the start).
    int StdinFd{-1};
    bool NoticesInline{false};
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
    Duration BackoffMax{std::chrono::milliseconds{20}};
    /// How long to keep reconnecting after the link drops.
    std::optional<Duration> GiveUpAfter{std::chrono::milliseconds{500}};
};

struct SessionTestResult {
    int Status;
    /// What the client held for snapshot reassembly when it exited.
    std::size_t SnapshotBufferBytes;
};

[[nodiscard]] SessionTestResult RunSessionForTest(
    SessionTestConfig config) noexcept;

}  // namespace zet::client
