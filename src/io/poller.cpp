#include "zet/io/poller.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <utility>

namespace zet::io {

namespace {

/// What the poller registers its own descriptors with in epoll data.
constexpr std::uint64_t SIGNAL_SOURCE = FIRST_RESERVED_TOKEN;
constexpr std::uint64_t WAKER_SOURCE = FIRST_RESERVED_TOKEN + 1;

static_assert(WAKER_SOURCE == UINT64_MAX);

std::uint32_t EpollEvents(EInterest interest) noexcept {
    // EPOLLRDHUP: a peer that shut down its write side shows up as a hangup
    // straight away, rather than only once the read returns 0.
    switch (interest) {
        case EInterest::Read:
            return EPOLLIN | EPOLLRDHUP;
        case EInterest::Write:
            return EPOLLOUT;
        case EInterest::ReadWrite:
            return EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    }
    return EPOLLIN | EPOLLRDHUP;
}

Errno Refused(std::string_view call) noexcept {
    return Errno{.Code = EINVAL, .Call = call};
}

}  // namespace

IoResult<void> Waker::Wake() const noexcept {
    const std::uint64_t one = 1;
    for (;;) {
        if (::write(Fd_, &one, sizeof(one)) ==
            static_cast<ssize_t>(sizeof(one))) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        // The counter is full, so a wake is pending already: nothing lost.
        if (errno == EAGAIN) {
            return {};
        }
        return std::unexpected(LastErrno("write"));
    }
}

int TimeoutMillis(Deadline deadline, Timestamp now) noexcept {
    if (!deadline.IsSet()) {
        return -1;
    }
    const Duration left = deadline.At() - now;
    if (left <= Duration::zero()) {
        return 0;
    }
    const auto millis =
        std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(millis, INT_MAX));
}

Poller::Poller(UniqueFd epoll) noexcept : Epoll_(std::move(epoll)) {
    sigemptyset(&Blocked_);
}

Poller::Poller(Poller&& other) noexcept
    : Epoll_(std::move(other.Epoll_)),
      Signals_(std::move(other.Signals_)),
      Waker_(std::move(other.Waker_)),
      WakerToken_(other.WakerToken_),
      Blocked_(other.Blocked_),
      SignalTokens_(other.SignalTokens_) {
    // The moved-from poller owns no signals any more, and must not unblock
    // them when it goes.
    sigemptyset(&other.Blocked_);
}

Poller::~Poller() {
    static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &Blocked_, nullptr));
}

IoResult<Poller> Poller::Create() noexcept {
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(LastErrno("epoll_create1"));
    }
    return Poller{UniqueFd{fd}};
}

IoResult<void> Poller::Control(int op, int fd, std::uint32_t events,
                               std::uint64_t token) noexcept {
    epoll_event event{};
    event.events = events;
    event.data.u64 = token;
    if (::epoll_ctl(Epoll_.Get(), op, fd, &event) != 0) {
        return std::unexpected(LastErrno("epoll_ctl"));
    }
    return {};
}

IoResult<void> Poller::Watch(int fd, EInterest interest,
                             std::uint64_t token) noexcept {
    if (token >= FIRST_RESERVED_TOKEN) {
        return std::unexpected(Refused("epoll_ctl"));
    }
    return Control(EPOLL_CTL_ADD, fd, EpollEvents(interest), token);
}

IoResult<void> Poller::Modify(int fd, EInterest interest,
                              std::uint64_t token) noexcept {
    if (token >= FIRST_RESERVED_TOKEN) {
        return std::unexpected(Refused("epoll_ctl"));
    }
    return Control(EPOLL_CTL_MOD, fd, EpollEvents(interest), token);
}

IoResult<void> Poller::Unwatch(int fd) noexcept {
    return Control(EPOLL_CTL_DEL, fd, 0, 0);
}

IoResult<void> Poller::WatchSignal(int signo, std::uint64_t token) noexcept {
    if (signo <= 0 || signo > MAX_SIGNAL) {
        return std::unexpected(Refused("signalfd"));
    }
    // One token per signal, as one per descriptor: a second subscriber would
    // silently take the first one's events.
    if (SignalTokens_[static_cast<std::size_t>(signo)].has_value()) {
        return std::unexpected(Errno{.Code = EEXIST, .Call = "signalfd"});
    }
    sigset_t one{};
    sigemptyset(&one);
    sigaddset(&one, signo);
    sigset_t before{};
    if (const int error = ::pthread_sigmask(SIG_BLOCK, &one, &before);
        error != 0) {
        return std::unexpected(Errno{.Code = error, .Call = "pthread_sigmask"});
    }
    // Only what this call blocked is this poller's to unblock: a signal the
    // caller had blocked already stays blocked after the poller is gone.
    const bool blockedHere = sigismember(&before, signo) == 0;
    if (blockedHere) {
        sigaddset(&Blocked_, signo);
    }
    // A failure below leaves the signal neither delivered nor watched; put
    // the mask back as the caller had it.
    const auto undo = [&]() noexcept {
        if (blockedHere) {
            static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &one, nullptr));
            sigdelset(&Blocked_, signo);
        }
    };

    // The full set of watched signals, this one included.
    sigset_t watched{};
    sigemptyset(&watched);
    for (int i = 1; i <= MAX_SIGNAL; ++i) {
        if (SignalTokens_[static_cast<std::size_t>(i)].has_value() ||
            i == signo) {
            sigaddset(&watched, i);
        }
    }

    const bool first = !Signals_.IsOpen();
    const int fd = ::signalfd(first ? -1 : Signals_.Get(), &watched,
                              SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        const Errno error = LastErrno("signalfd");
        undo();
        return std::unexpected(error);
    }
    if (first) {
        UniqueFd signals{fd};
        if (auto added = Control(EPOLL_CTL_ADD, fd, EPOLLIN, SIGNAL_SOURCE);
            !added) {
            undo();
            return added;
        }
        Signals_ = std::move(signals);
    }
    SignalTokens_[static_cast<std::size_t>(signo)] = token;
    return {};
}

IoResult<Waker> Poller::MakeWaker(std::uint64_t token) noexcept {
    if (Waker_.IsOpen()) {
        return std::unexpected(Errno{.Code = EEXIST, .Call = "eventfd"});
    }
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) {
        return std::unexpected(LastErrno("eventfd"));
    }
    UniqueFd waker{fd};
    if (auto added = Control(EPOLL_CTL_ADD, fd, EPOLLIN, WAKER_SOURCE);
        !added) {
        return std::unexpected(added.error());
    }
    Waker_ = std::move(waker);
    WakerToken_ = token;
    return Waker{fd};
}

void Poller::DrainWaker() noexcept {
    // One read takes the whole counter, however many wakes it holds.
    std::uint64_t count = 0;
    while (::read(Waker_.Get(), &count, sizeof(count)) < 0 && errno == EINTR) {
    }
}

IoResult<std::size_t> Poller::TakeSignals(std::span<PollEvent> out) noexcept {
    std::size_t taken = 0;
    while (taken < out.size()) {
        signalfd_siginfo info{};
        const ssize_t n = ::read(Signals_.Get(), &info, sizeof(info));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            return std::unexpected(LastErrno("read"));
        }
        if (n != static_cast<ssize_t>(sizeof(info))) {
            break;
        }
        const auto signo = static_cast<int>(info.ssi_signo);
        if (signo <= 0 || signo > MAX_SIGNAL) {
            continue;
        }
        const auto& token = SignalTokens_[static_cast<std::size_t>(signo)];
        if (!token) {
            continue;
        }
        out[taken] = PollEvent{.Token = *token, .Signal = signo};
        ++taken;
    }
    return taken;
}

IoResult<std::size_t> Poller::Wait(std::span<PollEvent> out, Deadline deadline,
                                   Timestamp now) noexcept {
    std::array<epoll_event, MAX_EVENTS_PER_WAIT> ready{};
    const auto room = static_cast<int>(std::min(out.size(), ready.size()));
    const int n = ::epoll_wait(Epoll_.Get(), ready.data(), room,
                               TimeoutMillis(deadline, now));
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return std::unexpected(LastErrno("epoll_wait"));
    }

    std::size_t written = 0;
    for (const epoll_event& event :
         std::span{ready}.first(static_cast<std::size_t>(n))) {
        if (written == out.size()) {
            break;
        }
        switch (event.data.u64) {
            case SIGNAL_SOURCE: {
                // A signal arrives as one record each, so one ready signalfd
                // can stand for several events.
                auto taken = TakeSignals(out.subspan(written));
                if (!taken) {
                    return std::unexpected(taken.error());
                }
                written += *taken;
                break;
            }
            case WAKER_SOURCE:
                DrainWaker();
                out[written++] = PollEvent{.Token = WakerToken_};
                break;
            default:
                out[written++] = PollEvent{
                    .Token = event.data.u64,
                    .Readable = (event.events & EPOLLIN) != 0,
                    .Writable = (event.events & EPOLLOUT) != 0,
                    .Hangup = (event.events & (EPOLLHUP | EPOLLRDHUP)) != 0,
                    .Error = (event.events & EPOLLERR) != 0,
                };
                break;
        }
    }
    return written;
}

}  // namespace zet::io
