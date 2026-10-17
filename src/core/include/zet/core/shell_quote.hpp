#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "zet/core/error.hpp"

/// The one place in zet that builds text for a shell to read (design.md §4).
///
/// ssh hands the remote command to the user's login shell as a single string,
/// so anything variable in it is a way in. The command is a constant
/// everywhere except the path to zet-agent, and that path goes through here.
namespace zet {

/// How long `text` becomes once quoted: two quotes around it, and every quote
/// inside turned into the four characters '\''.
[[nodiscard]] constexpr std::size_t ShellQuotedSize(
    std::string_view text) noexcept {
    std::size_t size = 2;
    for (const char c : text) {
        size += c == '\'' ? 4 : 1;
    }
    return size;
}

/// Writes `text` as one single-quoted word: inside single quotes a POSIX
/// shell expands nothing, so a newline, a `$` or a backtick stays itself. The
/// one character that cannot appear there, the quote, closes the word, is
/// written escaped, and opens a new one.
///
/// A NUL cannot travel in a command line at all and is MalformedField rather
/// than a silently shortened path.
[[nodiscard]] ProtoResult<std::size_t> ShellQuote(std::string_view text,
                                                  std::span<char> out) noexcept;

}  // namespace zet
