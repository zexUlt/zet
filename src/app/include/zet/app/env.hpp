#pragma once

#include <optional>
#include <string_view>

#include "zet/core/time.hpp"

/// What the binaries read from their environment: the timeouts of §9 that the
/// integration suite shortens (tests/integration/e2e.py), and nothing that
/// could change what the protocol does on the wire.
namespace zet::app {

/// The unit a variable is written in.
enum class EUnit : std::uint8_t {
    Milliseconds,
    Seconds,
};

/// `name` read as a whole number of `unit`, or nullopt when it is unset. A
/// value that is not a number is reported on stderr and ignored rather than
/// guessed at: a typo must not turn a 2-second test timeout into forever.
[[nodiscard]] std::optional<Duration> DurationFromEnv(const char* name,
                                                      EUnit unit) noexcept;

}  // namespace zet::app
