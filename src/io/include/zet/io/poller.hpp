#pragma once

#include <signal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "zet/core/time.hpp"
#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"

/// The one wait in zet, per docs/design.md §10: a single thread blocks here
/// until a descriptor is ready, a signal arrives, another thread wakes it, or
/// the nearest deadline passes. There are no fixed ticks.
///
/// What is watched is a source — a descriptor, a signal by number, a waker —
/// rather than a bare descriptor. On Linux all three are descriptors (the
/// latter two through signalfd and eventfd); on kqueue they would be
/// EVFILT_READ, EVFILT_SIGNAL and EVFILT_USER. Keeping that difference behind
/// the interface is what lets a second backend be written without the event
/// loop noticing.
namespace zet::io {

enum class EInterest : std::uint8_t {
    Read,
    Write,
    ReadWrite,
};

/// One thing that happened, tagged with the token its source was watched
/// with. A descriptor reports readiness; a signal reports its number; a waker
/// reports nothing but its token.
struct PollEvent {
    std::uint64_t Token{0};
    bool Readable{false};
    bool Writable{false};
    /// The peer is gone, or this end was shut down. Reading may still return
    /// what was left in the buffer before it returns end of file.
    bool Hangup{false};
    bool Error{false};
    /// The signal number, for an event from WatchSignal; 0 otherwise.
    int Signal{0};
};

/// Tokens from here up are the poller's own, for its signal and waker
/// descriptors. Watch refuses them with EINVAL rather than confusing a
/// caller's descriptor with its own.
inline constexpr std::uint64_t FIRST_RESERVED_TOKEN = UINT64_MAX - 1;

/// The most events one Wait takes from the kernel. Anything ready beyond it
/// is still ready on the next Wait: everything is level-triggered.
inline constexpr std::size_t MAX_EVENTS_PER_WAIT = 64;

/// The highest signal number WatchSignal accepts: Linux has 64, counting the
/// real-time ones.
inline constexpr int MAX_SIGNAL = 64;

/// Wakes a Poller from another thread. Does not own the descriptor: the Poller
/// does, and a Waker must not outlive the Poller that made it.
class Waker {
public:
    /// Makes the next Wait return with the waker's token, or the current one
    /// if it is blocked. Wakes that arrive before that Wait collapse into one.
    /// Safe from any thread.
    [[nodiscard]] IoResult<void> Wake() const noexcept;

private:
    friend class Poller;
    explicit Waker(int fd) noexcept : Fd_(fd) {}

    int Fd_;
};

class Poller {
public:
    [[nodiscard]] static IoResult<Poller> Create() noexcept;

    /// Unblocks the signals WatchSignal blocked, and only those. A signal
    /// still pending then gets its default action, so drain them first.
    ~Poller();

    Poller(Poller&& other) noexcept;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    Poller& operator=(Poller&&) = delete;

    /// Reports `fd` with `token` whenever it is ready for `interest`, until
    /// Unwatch or until the descriptor is closed.
    [[nodiscard]] IoResult<void> Watch(int fd, EInterest interest,
                                       std::uint64_t token) noexcept;

    /// Changes what `fd` is watched for and the token it reports with. This is
    /// how a writer asks for Write only while it has something queued, and
    /// stops the loop from waking for a socket that is always writable.
    [[nodiscard]] IoResult<void> Modify(int fd, EInterest interest,
                                        std::uint64_t token) noexcept;

    [[nodiscard]] IoResult<void> Unwatch(int fd) noexcept;

    /// Reports `signo` with `token` instead of letting it interrupt the
    /// process. The signal is blocked from here on, and every thread created
    /// afterwards inherits that; a child must unblock it before execve.
    /// A signal already watched here is refused with EEXIST. On any failure
    /// the signal mask is left as it was before the call.
    [[nodiscard]] IoResult<void> WatchSignal(int signo,
                                             std::uint64_t token) noexcept;

    /// The waker of this poller, reporting with `token`. One per poller: a
    /// second call fails with EEXIST.
    [[nodiscard]] IoResult<Waker> MakeWaker(std::uint64_t token) noexcept;

    /// Blocks until something happens or `deadline` passes, and writes what
    /// happened into `out`. Returns how many events it wrote; 0 means the
    /// deadline passed, or a signal not watched here interrupted the wait.
    /// `now` is when the caller last read the clock: the timeout is the
    /// distance to the deadline from there, rounded up to a millisecond so
    /// that the deadline has passed when Wait returns for it.
    [[nodiscard]] IoResult<std::size_t> Wait(std::span<PollEvent> out,
                                             Deadline deadline,
                                             Timestamp now) noexcept;

private:
    explicit Poller(UniqueFd epoll) noexcept;

    [[nodiscard]] IoResult<void> Control(int op, int fd, std::uint32_t events,
                                         std::uint64_t token) noexcept;
    [[nodiscard]] IoResult<std::size_t> TakeSignals(
        std::span<PollEvent> out) noexcept;
    void DrainWaker() noexcept;

    UniqueFd Epoll_;
    UniqueFd Signals_;
    UniqueFd Waker_;
    std::uint64_t WakerToken_{0};
    /// The signals this poller blocked, which are the ones it unblocks.
    sigset_t Blocked_{};
    /// The token each watched signal reports with, by signal number.
    std::array<std::optional<std::uint64_t>, MAX_SIGNAL + 1> SignalTokens_{};
};

/// The epoll_wait timeout for `deadline` seen from `now`: -1 for no deadline,
/// otherwise the distance in milliseconds rounded up and clamped to what an
/// int holds. Exposed for its test.
[[nodiscard]] int TimeoutMillis(Deadline deadline, Timestamp now) noexcept;

}  // namespace zet::io
