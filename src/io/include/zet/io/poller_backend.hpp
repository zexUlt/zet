#pragma once

#include "zet/io/fd.hpp"

/// What a Poller holds that differs by platform, and the one place in
/// zet_io that says which platform it is built for (design.md §10). The
/// backend is chosen at build time: poller_epoll.cpp on Linux,
/// poller_kqueue.cpp on macOS, and every caller sees only poller.hpp.
namespace zet::io {

#if defined(__linux__)

struct PollerBackend {
    UniqueFd Epoll;
    /// The signalfd all watched signals arrive through, opened by the first
    /// WatchSignal.
    UniqueFd Signals;
    /// The eventfd behind the Waker, opened by MakeWaker.
    UniqueFd Waker;

    [[nodiscard]] bool HasWaker() const noexcept { return Waker.IsOpen(); }
};

#elif defined(__APPLE__)

struct PollerBackend {
    /// Descriptors, signals and the waker are all filters on this one queue.
    UniqueFd Queue;
    /// Whether the EVFILT_USER filter of the Waker has been added.
    bool WakerAdded{false};

    [[nodiscard]] bool HasWaker() const noexcept { return WakerAdded; }
};

#else
#error "zet_io has a Poller backend for Linux (epoll) and macOS (kqueue) only"
#endif

}  // namespace zet::io
