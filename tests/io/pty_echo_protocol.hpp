#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

/// What zet_pty_echo says, shared by the child and the tests that read it.
namespace zet::io::test {

/// Printed once the child's terminal is in raw mode.
inline constexpr std::string_view READY_MARKER = "<zet-ready>";

/// The answer to SIGWINCH: rows, then columns.
inline constexpr const char* WINSIZE_REPORT_FORMAT = "<winsize %u %u>";

/// Room for the report with two five-digit numbers.
inline constexpr std::size_t WINSIZE_REPORT_MAX = 32;

/// How much the child reads or writes at once, and so holds in between.
inline constexpr std::size_t CHILD_BUFFER_SIZE = 64 * 1024;

/// The byte at `offset` of what `emit` prints. 251 is prime, so the pattern
/// does not line up with any buffer size and a shifted or dropped chunk shows.
[[nodiscard]] constexpr std::uint8_t PatternByte(
    std::uint64_t offset) noexcept {
    constexpr std::uint64_t period = 251;
    return static_cast<std::uint8_t>(offset % period);
}

}  // namespace zet::io::test
