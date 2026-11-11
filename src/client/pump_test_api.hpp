#pragma once

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "zet/core/time.hpp"
#include "zet/io/tcp.hpp"
#include "zet/predict/engine.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

/// Test seam: runs the client's event loop against `Addresses`, skipping the
/// SSH bootstrap. Connecting, reconnecting and the loop are production code.
namespace zet::client {

struct SessionTestConfig {
    wire::SessionSecrets Secrets;
    session::SessionLimits Limits;
    io::AddressList Addresses;
    int StdoutFd{STDOUT_FILENO};
    int StderrFd{STDERR_FILENO};
    /// A non-blocking fd the client reads typed input from; -1 means the
    /// session has no input (it is ended from the start).
    int StdinFd{-1};
    bool NoticesInline{false};
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
    Duration BackoffMax{std::chrono::milliseconds{20}};
    /// How long to keep reconnecting after the link drops.
    std::optional<Duration> GiveUpAfter{std::chrono::milliseconds{500}};
    /// Typed before the loop starts, as if it came from the keyboard; at most
    /// a few bytes. Stdin ends after it.
    std::string InitialInput{};
    /// Keep the mirror grid and write its snapshot here on exit (the
    /// ZET_TEST_MIRROR_DUMP hook); empty means no mirror.
    std::string MirrorDump{};
    /// --predict. Never by default, so tests of the stream see it as it is.
    predict::EDisplayMode Predict{predict::EDisplayMode::Never};
    /// The ZET_TEST_OVERLAY_LOG hook: "offset length" lines for the overlay's
    /// insertions into stdout.
    std::string OverlayLog{};
    /// Ask the terminal where its cursor is (round C). Off by default: the
    /// tests' stdout is a pipe that does not answer.
    bool Calibrate{false};
    /// The terminal size the client starts with, as if SIGWINCH had never
    /// come; the tests have no terminal to ask.
    std::optional<wire::ResizeMessage> InitialSize{};
    /// Asked for the terminal's size on SIGWINCH instead of the tty, which
    /// the tests do not have; empty means the real one. Called on the
    /// client's thread.
    std::function<std::optional<wire::ResizeMessage>()> SizeSource{};
    /// The mirror cannot be rebuilt from this snapshot (counting from 1, as
    /// when its allocation fails): the client drops it. 0: never.
    unsigned LoseMirrorAtSnapshot{0};
};

struct SessionTestResult {
    int Status;
    /// What the client held for snapshot reassembly when it exited.
    std::size_t SnapshotBufferBytes;
    /// Whether the client built the stdin demultiplexer (never, an old agent
    /// and ver=1 must not).
    bool CalibrationCreated;
    /// Cursor position queries written to the terminal.
    std::uint32_t QueriesWritten;
};

[[nodiscard]] SessionTestResult RunSessionForTest(
    SessionTestConfig config) noexcept;

}  // namespace zet::client
