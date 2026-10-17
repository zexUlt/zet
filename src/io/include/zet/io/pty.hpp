#pragma once

#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"

/// Pseudo-terminals, per docs/design.md §10 and §16: the agent's end of the
/// shell it runs.
namespace zet::io {

/// What TIOCSWINSZ carries. Its own type rather than wire::ResizeMessage:
/// zet_io moves bytes and does not depend on the protocol.
struct WindowSize {
    std::uint16_t Rows{0};
    std::uint16_t Cols{0};
    std::uint16_t XPixels{0};
    std::uint16_t YPixels{0};

    [[nodiscard]] friend bool operator==(const WindowSize&,
                                         const WindowSize&) noexcept = default;
};

/// Both ends of one pseudo-terminal. The master is non-blocking and both are
/// close-on-exec; the slave is opened without becoming anyone's controlling
/// terminal, which is Spawn's to arrange in the child.
struct Pty {
    UniqueFd Master;
    UniqueFd Slave;
};

[[nodiscard]] IoResult<Pty> OpenPty() noexcept;

/// Sets the terminal's size. The kernel sends SIGWINCH to the foreground
/// process group of the slave when it changes.
[[nodiscard]] IoResult<void> Resize(int fd, WindowSize size) noexcept;

/// The terminal's size, read from either end.
[[nodiscard]] IoResult<WindowSize> GetWindowSize(int fd) noexcept;

/// One read from the master, retried on EINTR.
///
/// Returns 0 at the end of the stream. Linux does not say so with a 0 the way
/// a pipe does: once every descriptor of the slave is closed, a read on the
/// master fails with EIO, and that EIO comes only after whatever output was
/// still buffered has been read. It is reported here as 0, so the caller sees
/// one end of stream for a pty, a pipe and a socket alike.
[[nodiscard]] IoResult<std::size_t> ReadPty(int master,
                                            MutableByteSpan into) noexcept;

}  // namespace zet::io
