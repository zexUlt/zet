#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "zet/core/time.hpp"

/// What the binaries read from their environment: the timeouts of §9 that the
/// integration suite shortens (tests/integration/e2e.py), and the test hook
/// that picks the resume path (§9 step 2).
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

/// `name` read as a whole number in [1, max], or nullopt when it is unset.
/// Zero, a value above `max`, a sign, a unit suffix or anything else is
/// reported on stderr and ignored.
[[nodiscard]] std::optional<std::size_t> SizeFromEnv(
    const char* name,
    std::size_t max = std::numeric_limits<std::size_t>::max()) noexcept;

/// The entry of `allowed` that `name` equals exactly, or nullopt when it is
/// unset. Any other value (a lower-case letter, trailing space) is reported on
/// stderr and ignored, so a typo cannot silently select a different mode.
[[nodiscard]] std::optional<std::string_view> ChoiceFromEnv(
    const char* name, std::span<const std::string_view> allowed) noexcept;

/// The raw value of `name`, or nullopt when it is unset.
[[nodiscard]] std::optional<std::string_view> StringFromEnv(
    const char* name) noexcept;

}  // namespace zet::app
