#pragma once

#include <termios.h>

#include <cstdint>

#include "zet/io/errno.hpp"

/// The client's own terminal, per docs/design.md §3: the one it was started
/// from, put in raw mode for as long as the session runs on it.
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

/// Sets the terminal's size. The kernel sends SIGWINCH to the foreground
/// process group of the slave when it changes.
[[nodiscard]] IoResult<void> Resize(int fd, WindowSize size) noexcept;

/// The terminal's size, read from either end.
[[nodiscard]] IoResult<WindowSize> GetWindowSize(int fd) noexcept;

/// Whether `fd` is a terminal at all. A client whose stdin is a pipe has
/// nothing to put in raw mode.
[[nodiscard]] bool IsTerminal(int fd) noexcept;

/// Raw mode on a terminal for as long as the object lives; the mode it found
/// comes back on destruction or on Restore.
///
/// Raw as cfmakeraw has it, which is what a remote terminal needs: no line
/// editing and no local echo, because the remote shell does both; no ISIG,
/// so Ctrl-C and Ctrl-Z travel to the remote instead of stopping the client;
/// no IXON, so Ctrl-S and Ctrl-Q do too; no output processing, because the
/// bytes from the remote are already what the screen should get. A read
/// returns as soon as one byte is there.
class RawMode {
public:
    [[nodiscard]] static IoResult<RawMode> Enter(int fd) noexcept;

    ~RawMode() { static_cast<void>(Restore()); }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;
    RawMode(RawMode&& other) noexcept;
    RawMode& operator=(RawMode&&) = delete;

    /// Puts the terminal back, once. Waits for queued output to be written
    /// first, so the last screenful is not cut off by the mode change.
    [[nodiscard]] IoResult<void> Restore() noexcept;

private:
    RawMode(int fd, const termios& saved) noexcept : Fd_(fd), Saved_(saved) {}

    int Fd_{-1};
    termios Saved_{};
};

}  // namespace zet::io
