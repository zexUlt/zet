#pragma once

#include <poll.h>

#include <chrono>
#include <string>
#include <string_view>

#include "zet/io/pty.hpp"

/// Waiting on a real pty in tests: poll(2) with a deadline, never a sleep.
namespace zet::io::test {

/// How long any one wait in these tests may take before it counts as a hang.
/// Generous for an ASan build on a shared CI runner; a passing run waits
/// milliseconds.
inline constexpr std::chrono::milliseconds PTY_WAIT_TIMEOUT{10'000};

/// What reading a master until a needle or the end of the stream produced.
struct ReadOutcome {
    std::string Output;
    bool Found{false};
    bool Ended{false};
    bool TimedOut{false};
};

/// Reads `master` until `needle` shows up, the stream ends, or a single wait
/// exceeds PTY_WAIT_TIMEOUT. An empty needle reads to the end.
inline ReadOutcome ReadUntil(int master, std::string_view needle) {
    ReadOutcome outcome;
    constexpr std::size_t chunkSize = 4096;
    std::string chunk(chunkSize, '\0');
    for (;;) {
        pollfd waiting{.fd = master, .events = POLLIN, .revents = 0};
        const int ready =
            ::poll(&waiting, 1, static_cast<int>(PTY_WAIT_TIMEOUT.count()));
        if (ready == 0) {
            outcome.TimedOut = true;
            return outcome;
        }
        if (ready < 0) {
            continue;
        }
        const auto n = ReadPty(
            master, MutableByteSpan{reinterpret_cast<std::byte*>(chunk.data()),
                                    chunk.size()});
        if (!n) {
            if (n.error().WouldBlock()) {
                continue;
            }
            outcome.Ended = true;
            return outcome;
        }
        if (*n == 0) {
            outcome.Ended = true;
            return outcome;
        }
        outcome.Output.append(chunk.data(), *n);
        if (!needle.empty() &&
            outcome.Output.find(needle) != std::string::npos) {
            outcome.Found = true;
            return outcome;
        }
    }
}

}  // namespace zet::io::test
