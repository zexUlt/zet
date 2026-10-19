#pragma once

#include <cstddef>

#include "zet/core/bytes.hpp"

/// The client's escape, as ssh has it: `~` at the start of a line, then a
/// command character. The one command is `.`, which ends zet at once — the
/// way out of a session whose connection is dead and will not come back.
///
///     ~.    quit; zet restores the terminal and exits with 255, as ssh does
///     ~~    one literal `~`
///     ~x    both, `~` and `x`, for any other x
///
/// A line starts at the beginning of the session and after CR or LF. A `~`
/// anywhere else is just a `~`. Pure: bytes in, bytes out, no terminal.
namespace zet::client {

/// What zet exits with after `~.`: ssh's status for "the connection was
/// closed by the escape".
inline constexpr int ESCAPE_QUIT_STATUS = 255;

class EscapeScanner {
public:
    struct Result {
        /// Bytes written to `out`, for the session.
        std::size_t Written{0};
        /// `~.` was seen; the bytes after it in this chunk were not read.
        bool Quit{false};
    };

    /// Scans `in` and writes what the session gets to `out`, which must hold
    /// 2 * in.size() + 1 bytes: every byte may come out with a held `~`.
    [[nodiscard]] Result Feed(ByteSpan in, MutableByteSpan out) noexcept;

private:
    bool AtLineStart_{true};
    /// A `~` at the start of a line, held until the next byte says what it
    /// was.
    bool Held_{false};
};

}  // namespace zet::client
