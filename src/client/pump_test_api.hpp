#pragma once

#include <unistd.h>
#include <cstdint>
#include <optional>

#include "zet/core/time.hpp"
#include "zet/io/fd.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

/// Test seam: drives the client pump loop on a pre-connected socket,
/// bypassing SSH bootstrap and the connect phase. Only used from unit and
/// integration tests.
namespace zet::client {

struct SessionTestConfig {
    wire::SessionSecrets Secrets;
    session::SessionLimits Limits;
    /// Pre-connected non-blocking socket; the client owns it from here.
    io::UniqueFd Socket;
    int StdoutFd{STDOUT_FILENO};
    int StderrFd{STDERR_FILENO};
    bool NoticesInline{false};
    bool GridCapable{true};
    uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
    /// How long to keep running after the connection drops (backoff + give-up).
    std::optional<Duration> GiveUpAfter{std::chrono::milliseconds{500}};
};

[[nodiscard]] int RunSessionForTest(SessionTestConfig config) noexcept;

}  // namespace zet::client
