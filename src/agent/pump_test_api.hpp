#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

#include "grid_pump.hpp"
#include "zet/core/time.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

namespace zet::agent {

/// Test seam: drives the agent pump loop on a TCP listener with a pipe in
/// place of the pty, bypassing SSH bootstrap. Only used from tests.
struct AgentTestConfig {
    wire::SessionSecrets Secrets;
    session::SessionLimits Limits;
    /// Pre-bound-and-listening TCP socket; the test connects to Listen.Port.
    io::Listener Listen;
    /// Non-blocking pipe read end standing in for the pty master.
    io::UniqueFd PtyReadFd;
    /// Protocol ceiling used for every connection accepted from Listen.
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
    /// Maximum run time before the loop returns FAILURE_STATUS.
    std::optional<Duration> GiveUpAfter{std::chrono::milliseconds{2000}};
    /// Called on the agent thread with the agent's GridPump after the loop
    /// returns, so a test can read the grid the real pump built.
    std::function<void(const GridPump&)> Inspect;
    /// When it reads true at the top of a loop turn the loop returns
    /// FAILURE_STATUS, like GiveUpAfter.  The loop only looks when something
    /// wakes it, so set it and then connect to Listen.Port.
    const std::atomic<bool>* Stop{nullptr};
    /// The seam presets the child as exited, so by default the agent drops the
    /// client's input as for a shell that has gone. True makes it write that
    /// input to PtyReadFd (the pty master) instead.
    bool InputToPty{false};
};

[[nodiscard]] int RunAgentForTest(AgentTestConfig config) noexcept;

}  // namespace zet::agent
