#pragma once

#include <expected>
#include <string_view>

/// The error type of the one layer that makes system calls, per
/// docs/design.md §10: which call failed and what errno it left.
namespace zet::io {

/// A system call that failed. `Call` names it and points at a string literal,
/// so an Errno can be copied, stored and logged without owning anything.
struct Errno {
    int Code{0};
    std::string_view Call;

    /// The call would have blocked: EAGAIN or EWOULDBLOCK. Not a failure on a
    /// non-blocking descriptor, only a sign to wait for the poller.
    [[nodiscard]] bool WouldBlock() const noexcept;

    /// What the C library says the code means. Good until the next call:
    /// musl answers from a static table, but glibc writes an unknown code's
    /// text into a buffer the next strerror reuses. A getaddrinfo failure is
    /// described from its own EAI_* table.
    [[nodiscard]] std::string_view Description() const noexcept;

    [[nodiscard]] friend bool operator==(const Errno& a,
                                         const Errno& b) noexcept {
        return a.Code == b.Code && a.Call == b.Call;
    }
};

template <typename TValue>
using IoResult = std::expected<TValue, Errno>;

/// The errno the last call left, attributed to `call`. Read straight after
/// the call that failed: anything in between may overwrite it.
[[nodiscard]] Errno LastErrno(std::string_view call) noexcept;

}  // namespace zet::io
