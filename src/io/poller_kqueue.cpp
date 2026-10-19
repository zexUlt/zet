#include <sys/event.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>

#include "zet/io/poller.hpp"

// The kqueue backend of the Poller contract in poller.hpp, for macOS.
//
// epoll has one registration per descriptor with an event mask; kqueue has
// one filter per descriptor and direction. A descriptor watched for
// ReadWrite is two filters here, and its two events are merged back into one
// within a Wait, as the contract promises.
namespace zet::io {

namespace {

/// The ident of the Waker's EVFILT_USER filter. One waker per poller, so any
/// constant will do.
constexpr std::uintptr_t WAKER_IDENT = 1;

constexpr std::size_t MAX_CHANGES = 2;

Errno Refused(std::string_view call) noexcept {
    return Errno{.Code = EINVAL, .Call = call};
}

void* AsUserData(std::uint64_t token) noexcept {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
}

std::uint64_t FromUserData(void* udata) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(udata));
}

struct kevent Change(std::uintptr_t ident, std::int16_t filter,
                     std::uint16_t flags, std::uint32_t fflags,
                     void* udata) noexcept {
    struct kevent change{};
    EV_SET(&change, ident, filter, flags, fflags, 0, udata);
    return change;
}

/// Applies `changes` and reports each one's own result: with EV_RECEIPT,
/// kevent answers every change with an EV_ERROR entry whose data is the
/// errno, 0 for success, rather than stopping at the first failure.
/// `missingIsFine` is for EV_DELETE of a filter that may never have been
/// added; the result is how many of those were missing.
IoResult<std::size_t> Apply(int queue, std::span<struct kevent> changes,
                            std::span<const bool> missingIsFine) noexcept {
    std::array<struct kevent, MAX_CHANGES> results{};
    for (auto& change : changes) {
        change.flags |= EV_RECEIPT;
    }
    const timespec zero{};
    const int n =
        ::kevent(queue, changes.data(), static_cast<int>(changes.size()),
                 results.data(), static_cast<int>(changes.size()), &zero);
    if (n < 0) {
        return std::unexpected(LastErrno("kevent"));
    }
    std::size_t missing = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
        const auto error = static_cast<int>(results[i].data);
        if ((results[i].flags & EV_ERROR) == 0 || error == 0) {
            continue;
        }
        if (error == ENOENT && missingIsFine[i]) {
            ++missing;
            continue;
        }
        return std::unexpected(Errno{.Code = error, .Call = "kevent"});
    }
    return missing;
}

/// Adds the filters `interest` wants and drops the ones it does not, which
/// is Watch and Modify alike: kqueue's EV_ADD replaces a filter in place.
IoResult<void> Register(int queue, int fd, EInterest interest,
                        std::uint64_t token) noexcept {
    const bool read = interest != EInterest::Write;
    const bool write = interest != EInterest::Read;
    const auto ident = static_cast<std::uintptr_t>(fd);
    std::array<struct kevent, MAX_CHANGES> changes{
        Change(ident, EVFILT_READ, read ? EV_ADD | EV_ENABLE : EV_DELETE, 0,
               AsUserData(token)),
        Change(ident, EVFILT_WRITE, write ? EV_ADD | EV_ENABLE : EV_DELETE, 0,
               AsUserData(token))};
    const std::array<bool, MAX_CHANGES> missingIsFine{!read, !write};
    if (auto applied = Apply(queue, changes, missingIsFine); !applied) {
        return std::unexpected(applied.error());
    }
    return {};
}

/// Takes the one pending instance of `signo` off the process, so that the
/// unblock in ~Poller does not deliver it a second time. kqueue reports a
/// blocked signal but leaves it pending, where signalfd would have read it.
void Consume(int signo) noexcept {
    sigset_t pending{};
    if (::sigpending(&pending) != 0 || sigismember(&pending, signo) != 1) {
        return;
    }
    sigset_t one{};
    sigemptyset(&one);
    sigaddset(&one, signo);
    int taken = 0;
    // Returns at once: the signal is pending and blocked.
    static_cast<void>(::sigwait(&one, &taken));
}

}  // namespace

IoResult<void> Waker::Wake() const noexcept {
    std::array<struct kevent, 1> trigger{
        Change(WAKER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, nullptr)};
    for (;;) {
        if (::kevent(Fd_, trigger.data(), 1, nullptr, 0, nullptr) == 0) {
            return {};
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("kevent"));
        }
    }
}

IoResult<Poller> Poller::Create() noexcept {
    UniqueFd queue{::kqueue()};
    if (!queue) {
        return std::unexpected(LastErrno("kqueue"));
    }
    // A kqueue does not survive fork anyway; close-on-exec keeps it out of
    // whatever the child execs as well.
    if (auto closing = SetCloseOnExec(queue.Get()); !closing) {
        return std::unexpected(closing.error());
    }
    return Poller{
        PollerBackend{.Queue = std::move(queue), .WakerAdded = false}};
}

IoResult<void> Poller::Watch(int fd, EInterest interest,
                             std::uint64_t token) noexcept {
    if (token >= FIRST_RESERVED_TOKEN) {
        return std::unexpected(Refused("kevent"));
    }
    return Register(Backend_.Queue.Get(), fd, interest, token);
}

IoResult<void> Poller::Modify(int fd, EInterest interest,
                              std::uint64_t token) noexcept {
    if (token >= FIRST_RESERVED_TOKEN) {
        return std::unexpected(Refused("kevent"));
    }
    return Register(Backend_.Queue.Get(), fd, interest, token);
}

IoResult<void> Poller::Unwatch(int fd) noexcept {
    const auto ident = static_cast<std::uintptr_t>(fd);
    std::array<struct kevent, MAX_CHANGES> changes{
        Change(ident, EVFILT_READ, EV_DELETE, 0, nullptr),
        Change(ident, EVFILT_WRITE, EV_DELETE, 0, nullptr)};
    // Either filter alone may be missing; both missing means fd was never
    // watched, which epoll answers with ENOENT and so does this.
    const std::array<bool, MAX_CHANGES> eitherMayBeMissing{true, true};
    auto missing = Apply(Backend_.Queue.Get(), changes, eitherMayBeMissing);
    if (!missing) {
        return std::unexpected(missing.error());
    }
    if (*missing == changes.size()) {
        return std::unexpected(Errno{.Code = ENOENT, .Call = "kevent"});
    }
    return {};
}

IoResult<void> Poller::WatchSignal(int signo, std::uint64_t token) noexcept {
    if (signo <= 0 || signo > MAX_SIGNAL || signo >= NSIG) {
        return std::unexpected(Refused("kevent"));
    }
    if (SignalTokens_[static_cast<std::size_t>(signo)].has_value()) {
        return std::unexpected(Errno{.Code = EEXIST, .Call = "kevent"});
    }
    sigset_t one{};
    sigemptyset(&one);
    sigaddset(&one, signo);
    sigset_t before{};
    if (const int error = ::pthread_sigmask(SIG_BLOCK, &one, &before);
        error != 0) {
        return std::unexpected(Errno{.Code = error, .Call = "pthread_sigmask"});
    }
    const bool blockedHere = sigismember(&before, signo) == 0;
    if (blockedHere) {
        sigaddset(&Blocked_, signo);
    }
    std::array<struct kevent, 1> add{Change(static_cast<std::uintptr_t>(signo),
                                            EVFILT_SIGNAL, EV_ADD, 0, nullptr)};
    const std::array<bool, 1> strict{false};
    if (auto added = Apply(Backend_.Queue.Get(), add, strict); !added) {
        // Put the mask back as the caller had it, as the epoll backend does.
        if (blockedHere) {
            static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &one, nullptr));
            sigdelset(&Blocked_, signo);
        }
        return std::unexpected(added.error());
    }
    SignalTokens_[static_cast<std::size_t>(signo)] = token;
    return {};
}

IoResult<Waker> Poller::MakeWaker(std::uint64_t token) noexcept {
    if (Backend_.HasWaker()) {
        return std::unexpected(Errno{.Code = EEXIST, .Call = "kevent"});
    }
    // EV_CLEAR: the trigger resets once reported, so wakes before a Wait
    // collapse into one event and the next Wait sees nothing.
    std::array<struct kevent, 1> add{
        Change(WAKER_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, nullptr)};
    const std::array<bool, 1> strict{false};
    if (auto added = Apply(Backend_.Queue.Get(), add, strict); !added) {
        return std::unexpected(added.error());
    }
    Backend_.WakerAdded = true;
    WakerToken_ = token;
    return Waker{Backend_.Queue.Get()};
}

IoResult<std::size_t> Poller::Wait(std::span<PollEvent> out, Deadline deadline,
                                   Timestamp now) noexcept {
    if (out.empty()) {
        return std::unexpected(Refused("kevent"));
    }
    std::array<struct kevent, MAX_EVENTS_PER_WAIT> ready{};
    const auto room = static_cast<int>(std::min(out.size(), ready.size()));
    const int millis = TimeoutMillis(deadline, now);
    constexpr long nanosPerMilli = 1'000'000;
    constexpr int millisPerSecond = 1000;
    const timespec timeout{
        .tv_sec = millis / millisPerSecond,
        .tv_nsec = static_cast<long>(millis % millisPerSecond) * nanosPerMilli};
    const int n = ::kevent(Backend_.Queue.Get(), nullptr, 0, ready.data(), room,
                           millis < 0 ? nullptr : &timeout);
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return std::unexpected(LastErrno("kevent"));
    }

    // The descriptor each written event came from, to merge a descriptor's
    // read and write filters into the one event the contract promises.
    std::array<std::uintptr_t, MAX_EVENTS_PER_WAIT> sources{};
    std::size_t written = 0;
    for (const struct kevent& event :
         std::span{ready}.first(static_cast<std::size_t>(n))) {
        switch (event.filter) {
            case EVFILT_USER:
                out[written++] = PollEvent{.Token = WakerToken_};
                break;
            case EVFILT_SIGNAL: {
                const auto signo = static_cast<int>(event.ident);
                Consume(signo);
                if (const auto token = SignalToken(signo)) {
                    out[written++] =
                        PollEvent{.Token = *token, .Signal = signo};
                }
                break;
            }
            case EVFILT_READ:
            case EVFILT_WRITE: {
                std::size_t at = written;
                for (std::size_t i = 0; i < written; ++i) {
                    if (out[i].Signal == 0 && sources[i] == event.ident &&
                        out[i].Token == FromUserData(event.udata)) {
                        at = i;
                        break;
                    }
                }
                if (at == written) {
                    out[written] =
                        PollEvent{.Token = FromUserData(event.udata)};
                    sources[written] = event.ident;
                    ++written;
                }
                PollEvent& merged = out[at];
                if (event.filter == EVFILT_READ) {
                    merged.Readable = true;
                } else {
                    merged.Writable = true;
                }
                if ((event.flags & EV_EOF) != 0) {
                    merged.Hangup = true;
                    // For a socket, fflags carries the error that ended it.
                    if (event.fflags != 0) {
                        merged.Error = true;
                    }
                }
                if ((event.flags & EV_ERROR) != 0) {
                    merged.Error = true;
                }
                break;
            }
            default:
                break;
        }
    }
    return written;
}

}  // namespace zet::io
