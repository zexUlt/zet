#pragma once

#include "zet/core/log.hpp"

namespace zet::app {

/// Sends zet::log records to `fd` as one line each: the agent's log once it
/// has detached and stderr is /dev/null (§15). The descriptor stays the
/// caller's to keep open.
void LogToFd(int fd) noexcept;

}  // namespace zet::app
