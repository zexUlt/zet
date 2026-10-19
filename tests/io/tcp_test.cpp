#include "zet/io/tcp.hpp"

#include <doctest/doctest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>

#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::io;

constexpr std::uint64_t LISTENER_TOKEN = 1;
constexpr std::uint64_t CLIENT_TOKEN = 2;

/// A deadline far enough off that reaching it means the event never came.
constexpr Duration GIVE_UP = 10s;

/// Waits for `token` to be ready, or fails the test at GIVE_UP.
PollEvent WaitFor(Poller& poller, std::uint64_t token) {
    std::array<PollEvent, 4> events{};
    const Timestamp start = MonotonicNow();
    for (;;) {
        const Timestamp now = MonotonicNow();
        REQUIRE_MESSAGE(now - start < GIVE_UP,
                        "token " << token << " never ready");
        const auto ready = poller.Wait(events, Deadline{start + GIVE_UP}, now);
        REQUIRE(ready.has_value());
        for (std::size_t i = 0; i < *ready; ++i) {
            if (events[i].Token == token) {
                return events[i];
            }
        }
    }
}

SocketAddress Loopback(std::uint16_t port) {
    auto found = Resolve("localhost", port);
    REQUIRE(found.has_value());
    REQUIRE(found->Count > 0);
    return found->Items[0];
}

bool NoDelay(int fd) {
    int value = 0;
    socklen_t length = sizeof(value);
    REQUIRE(::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &length) == 0);
    return value != 0;
}

}  // namespace

TEST_CASE("a listener gets a port of its own and accepts nothing yet") {
    auto listener = ListenTcp();
    REQUIRE(listener.has_value());
    CHECK(listener->Port != 0);
    const auto none = Accept(listener->Socket.Get());
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().WouldBlock());
    CHECK(none.error().Call == "accept");
}

TEST_CASE("connect, accept and a round trip go through the poller") {
    auto listener = ListenTcp();
    REQUIRE(listener.has_value());
    auto poller = Poller::Create();
    REQUIRE(poller.has_value());
    REQUIRE(
        poller->Watch(listener->Socket.Get(), EInterest::Read, LISTENER_TOKEN));

    auto client = StartConnect(Loopback(listener->Port));
    REQUIRE(client.has_value());
    CHECK(NoDelay(client->Get()));
    REQUIRE(poller->Watch(client->Get(), EInterest::Write, CLIENT_TOKEN));
    CHECK(WaitFor(*poller, CLIENT_TOKEN).Writable);
    REQUIRE(FinishConnect(client->Get()));

    CHECK(WaitFor(*poller, LISTENER_TOKEN).Readable);
    auto served = Accept(listener->Socket.Get());
    REQUIRE(served.has_value());
    CHECK(NoDelay(served->Get()));

    const std::array<std::byte, 4> hello{std::byte{'z'}, std::byte{'e'},
                                         std::byte{'t'}, std::byte{'!'}};
    REQUIRE(WriteSome(client->Get(), ByteSpan{hello}) == hello.size());
    REQUIRE(poller->Watch(served->Get(), EInterest::Read, LISTENER_TOKEN + 10));
    CHECK(WaitFor(*poller, LISTENER_TOKEN + 10).Readable);
    std::array<std::byte, 16> got{};
    REQUIRE(ReadSome(served->Get(), MutableByteSpan{got}) == hello.size());
    CHECK(std::memcmp(got.data(), hello.data(), hello.size()) == 0);
}

TEST_CASE("a refused connection is reported by FinishConnect") {
    // A port that was listening a moment ago and is not now.
    std::uint16_t closedPort = 0;
    {
        auto listener = ListenTcp();
        REQUIRE(listener.has_value());
        closedPort = listener->Port;
    }
    auto poller = Poller::Create();
    REQUIRE(poller.has_value());
    auto client = StartConnect(Loopback(closedPort));
    REQUIRE(client.has_value());
    REQUIRE(poller->Watch(client->Get(), EInterest::Write, CLIENT_TOKEN));
    WaitFor(*poller, CLIENT_TOKEN);
    const auto finished = FinishConnect(client->Get());
    REQUIRE_FALSE(finished.has_value());
    CHECK(finished.error().Code == ECONNREFUSED);
    CHECK(finished.error().Call == "connect");
}

TEST_CASE("resolving fills in the port and names a failure") {
    constexpr std::uint16_t port = 4242;
    auto found = Resolve("127.0.0.1", port);
    REQUIRE(found.has_value());
    REQUIRE(found->Count == 1);
    CHECK(found->View().size() == 1);
    CHECK(found->Items[0].Port() == port);
    CHECK(found->Items[0].Storage.ss_family == AF_INET);

    const auto missing = Resolve("no-such-host.invalid", port);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().Call == "getaddrinfo");
    CHECK_FALSE(missing.error().Description().empty());
}

TEST_CASE("a listener can take a port it was asked for") {
    std::uint16_t port = 0;
    {
        auto first = ListenTcp();
        REQUIRE(first.has_value());
        port = first->Port;
    }
    auto again = ListenTcp(port);
    REQUIRE(again.has_value());
    CHECK(again->Port == port);
    const auto taken = ListenTcp(port);
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().Code == EADDRINUSE);
    CHECK(taken.error().Call == "bind");
}

TEST_CASE("a socket address of no known family has no port") {
    SocketAddress unknown{};
    unknown.Storage.ss_family = AF_UNIX;
    CHECK(unknown.Port() == 0);
}
