#include "zet/io/poller.hpp"

#include <doctest/doctest.h>
#include <signal.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <thread>

#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::io;

constexpr std::uint64_t FIRST_TOKEN = 7;
constexpr std::uint64_t SECOND_TOKEN = 8;
constexpr std::uint64_t SIGNAL_TOKEN = 40;
constexpr std::uint64_t WAKER_TOKEN = 50;

/// Long enough that a test which should not wait would visibly hang on it,
/// short enough that one which does wait finishes quickly.
constexpr Duration SHORT_WAIT = 20ms;

/// A deadline far enough off that reaching it means the event never came.
constexpr Duration GIVE_UP = 10s;

Poller MakePoller() {
    auto poller = Poller::Create();
    REQUIRE(poller.has_value());
    return std::move(*poller);
}

/// Waits once with a far deadline, for tests where something is ready.
std::size_t WaitReady(Poller& poller, std::span<PollEvent> out) {
    const Timestamp now = MonotonicNow();
    const auto n = poller.Wait(out, Deadline{now + GIVE_UP}, now);
    REQUIRE(n.has_value());
    return *n;
}

bool IsBlocked(int signo) {
    sigset_t current{};
    REQUIRE(::pthread_sigmask(SIG_BLOCK, nullptr, &current) == 0);
    return sigismember(&current, signo) == 1;
}

void Send(int fd) {
    const std::array<std::byte, 1> byte{std::byte{1}};
    REQUIRE(WriteSome(fd, ByteSpan{byte}) == 1u);
}

}  // namespace

TEST_CASE("the timeout is the deadline rounded up to a millisecond") {
    const Timestamp now = Timestamp::FromNanos(1'000'000'000);
    CHECK(TimeoutMillis(Deadline{}, now) == -1);
    CHECK(TimeoutMillis(Deadline{now}, now) == 0);
    CHECK(TimeoutMillis(Deadline{now - 5ms}, now) == 0);
    CHECK(TimeoutMillis(Deadline{now + 1ns}, now) == 1);
    CHECK(TimeoutMillis(Deadline{now + 1ms}, now) == 1);
    CHECK(TimeoutMillis(Deadline{now + 1ms + 1ns}, now) == 2);
    CHECK(TimeoutMillis(Deadline{now + std::chrono::hours{24 * 365}}, now) ==
          INT_MAX);
}

TEST_CASE("a descriptor with bytes waiting is reported readable") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(poller.Watch(pair->First.Get(), EInterest::Read, FIRST_TOKEN));

    Send(pair->Second.Get());
    std::array<PollEvent, 4> events{};
    REQUIRE(WaitReady(poller, events) == 1);
    CHECK(events[0].Token == FIRST_TOKEN);
    CHECK(events[0].Readable);
    CHECK_FALSE(events[0].Writable);
    CHECK_FALSE(events[0].Hangup);
    CHECK(events[0].Signal == 0);
}

TEST_CASE("a socket with room is reported writable") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(poller.Watch(pair->First.Get(), EInterest::Write, FIRST_TOKEN));

    std::array<PollEvent, 4> events{};
    REQUIRE(WaitReady(poller, events) == 1);
    CHECK(events[0].Writable);
    CHECK_FALSE(events[0].Readable);
}

TEST_CASE("a peer that closed is reported as a hangup") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(poller.Watch(pair->First.Get(), EInterest::Read, FIRST_TOKEN));

    pair->Second.Reset();
    std::array<PollEvent, 4> events{};
    REQUIRE(WaitReady(poller, events) == 1);
    CHECK(events[0].Hangup);
    CHECK(events[0].Readable);
}

TEST_CASE("Modify changes interest and token, Unwatch stops the reports") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    const int fd = pair->First.Get();
    REQUIRE(poller.Watch(fd, EInterest::Read, FIRST_TOKEN));
    REQUIRE(poller.Modify(fd, EInterest::ReadWrite, SECOND_TOKEN));

    std::array<PollEvent, 4> events{};
    REQUIRE(WaitReady(poller, events) == 1);
    CHECK(events[0].Token == SECOND_TOKEN);
    CHECK(events[0].Writable);

    REQUIRE(poller.Unwatch(fd));
    const Timestamp now = MonotonicNow();
    const auto n = poller.Wait(events, Deadline{now}, now);
    REQUIRE(n.has_value());
    CHECK(*n == 0);
}

TEST_CASE("with nothing ready Wait returns once the deadline has passed") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(poller.Watch(pair->First.Get(), EInterest::Read, FIRST_TOKEN));

    const Timestamp start = MonotonicNow();
    const Deadline deadline{start + SHORT_WAIT};
    std::array<PollEvent, 4> events{};
    const auto n = poller.Wait(events, deadline, start);
    REQUIRE(n.has_value());
    CHECK(*n == 0);
    CHECK(deadline.HasExpired(MonotonicNow()));
}

TEST_CASE("with no deadline a ready descriptor returns straight away") {
    Poller poller = MakePoller();
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(poller.Watch(pair->First.Get(), EInterest::Read, FIRST_TOKEN));
    Send(pair->Second.Get());

    std::array<PollEvent, 4> events{};
    const auto n = poller.Wait(events, Deadline{}, MonotonicNow());
    REQUIRE(n.has_value());
    CHECK(*n == 1);
}

TEST_CASE("more ready sources than room are reported on the next Wait") {
    Poller poller = MakePoller();
    auto first = MakeSocketPair();
    auto second = MakeSocketPair();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(poller.Watch(first->First.Get(), EInterest::Write, FIRST_TOKEN));
    REQUIRE(poller.Watch(second->First.Get(), EInterest::Write, SECOND_TOKEN));

    std::array<PollEvent, 1> one{};
    REQUIRE(WaitReady(poller, one) == 1);
    const std::uint64_t seen = one[0].Token;
    std::array<PollEvent, 4> rest{};
    REQUIRE(WaitReady(poller, rest) == 2);
    CHECK(seen != 0);
}

TEST_CASE("a watched signal arrives as an event and is unblocked after") {
    REQUIRE_FALSE(IsBlocked(SIGUSR1));
    {
        Poller poller = MakePoller();
        REQUIRE(poller.WatchSignal(SIGUSR1, SIGNAL_TOKEN));
        CHECK(IsBlocked(SIGUSR1));

        REQUIRE(::raise(SIGUSR1) == 0);
        std::array<PollEvent, 4> events{};
        REQUIRE(WaitReady(poller, events) == 1);
        CHECK(events[0].Token == SIGNAL_TOKEN);
        CHECK(events[0].Signal == SIGUSR1);
        CHECK_FALSE(events[0].Readable);
    }
    CHECK_FALSE(IsBlocked(SIGUSR1));
}

TEST_CASE("several watched signals share one source and keep their tokens") {
    Poller poller = MakePoller();
    REQUIRE(poller.WatchSignal(SIGUSR1, SIGNAL_TOKEN));
    REQUIRE(poller.WatchSignal(SIGUSR2, SIGNAL_TOKEN + 1));

    REQUIRE(::raise(SIGUSR2) == 0);
    REQUIRE(::raise(SIGUSR1) == 0);
    std::array<PollEvent, 4> events{};
    REQUIRE(WaitReady(poller, events) == 2);
    bool sawFirst = false;
    bool sawSecond = false;
    for (const PollEvent& event : std::span{events}.first(2)) {
        sawFirst |= event.Signal == SIGUSR1 && event.Token == SIGNAL_TOKEN;
        sawSecond |= event.Signal == SIGUSR2 && event.Token == SIGNAL_TOKEN + 1;
    }
    CHECK(sawFirst);
    CHECK(sawSecond);
}

TEST_CASE("a signal the caller had blocked stays blocked after the poller") {
    sigset_t one{};
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &one, nullptr) == 0);
    {
        Poller poller = MakePoller();
        REQUIRE(poller.WatchSignal(SIGUSR2, SIGNAL_TOKEN));
    }
    CHECK(IsBlocked(SIGUSR2));
    REQUIRE(::pthread_sigmask(SIG_UNBLOCK, &one, nullptr) == 0);
}

TEST_CASE("a moved-from poller leaves the signals to the one it moved to") {
    Poller target = MakePoller();
    {
        Poller source = MakePoller();
        REQUIRE(source.WatchSignal(SIGUSR1, SIGNAL_TOKEN));
        Poller moved{std::move(source)};
        REQUIRE(::raise(SIGUSR1) == 0);
        std::array<PollEvent, 4> events{};
        REQUIRE(WaitReady(moved, events) == 1);
        CHECK(events[0].Signal == SIGUSR1);
        // `source` is destroyed after `moved`; neither may leave it blocked,
        // and the moved-from one must not have unblocked it early.
        CHECK(IsBlocked(SIGUSR1));
    }
    CHECK_FALSE(IsBlocked(SIGUSR1));
}

TEST_CASE("a waker from another thread ends the wait") {
    Poller poller = MakePoller();
    auto waker = poller.MakeWaker(WAKER_TOKEN);
    REQUIRE(waker.has_value());

    std::thread other{[w = *waker] { static_cast<void>(w.Wake()); }};
    std::array<PollEvent, 4> events{};
    const std::size_t n = WaitReady(poller, events);
    other.join();
    REQUIRE(n == 1);
    CHECK(events[0].Token == WAKER_TOKEN);

    // Drained: the next wait sees nothing.
    const Timestamp now = MonotonicNow();
    const auto again = poller.Wait(events, Deadline{now}, now);
    REQUIRE(again.has_value());
    CHECK(*again == 0);
}

TEST_CASE("wakes before the wait collapse into one event") {
    Poller poller = MakePoller();
    auto waker = poller.MakeWaker(WAKER_TOKEN);
    REQUIRE(waker.has_value());
    REQUIRE(waker->Wake());
    REQUIRE(waker->Wake());
    std::array<PollEvent, 4> events{};
    CHECK(WaitReady(poller, events) == 1);
}

TEST_CASE("refusals name the call and the reason") {
    Poller poller = MakePoller();

    const auto bad = poller.Watch(-1, EInterest::Read, FIRST_TOKEN);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().Call == "epoll_ctl");
    CHECK(bad.error().Code == EBADF);

    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    const auto reserved =
        poller.Watch(pair->First.Get(), EInterest::Read, FIRST_RESERVED_TOKEN);
    REQUIRE_FALSE(reserved.has_value());
    CHECK(reserved.error().Code == EINVAL);
    const auto reservedModify = poller.Modify(
        pair->First.Get(), EInterest::Read, FIRST_RESERVED_TOKEN + 1);
    REQUIRE_FALSE(reservedModify.has_value());
    CHECK(reservedModify.error().Code == EINVAL);

    const auto unknown = poller.Unwatch(pair->First.Get());
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().Code == ENOENT);

    const auto noSignal = poller.WatchSignal(MAX_SIGNAL + 1, SIGNAL_TOKEN);
    REQUIRE_FALSE(noSignal.has_value());
    CHECK(noSignal.error().Code == EINVAL);
    CHECK_FALSE(poller.WatchSignal(0, SIGNAL_TOKEN).has_value());

    REQUIRE(poller.MakeWaker(WAKER_TOKEN));
    const auto second = poller.MakeWaker(WAKER_TOKEN);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().Code == EEXIST);

    std::array<PollEvent, 1> events{};
    const Timestamp now = MonotonicNow();
    const auto empty =
        poller.Wait(std::span<PollEvent>{events}.first(0), Deadline{now}, now);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().Call == "epoll_wait");
}
