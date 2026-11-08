/// The real client's input path when the ring is full (design.md §9), against
/// the real agent, over TCP and a real pty.
///
/// The client is connected from the start and the user types several rings of
/// input while the shell prints nothing. The Ack that frees the client's ring
/// is taken in Client::Pump, after the loop's FeedStdin of the turn has run;
/// stdin is not watched while the client's input buffer holds bytes, so the
/// rest has to move into the freed ring in the same turn. Keepalives are far
/// apart, so nothing else wakes the loop.
///
/// Kills: the FeedStdin after the Pump in Client::Run removed (the input
/// stops after a ring or two and waits for the keepalive).

#include "../../src/agent/pump_test_api.hpp"
#include "../../src/client/pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/tcp.hpp"

namespace {

using namespace zet::agent;
using namespace std::chrono_literals;

// Not a divisor of the chunks the client reads stdin in, so the ring fills
// with input still left in its buffer.
constexpr std::size_t RESEND = 65536 + 1000;
constexpr std::size_t INPUT = 4 * RESEND;

/// Ends everything the test started, on every exit including a REQUIRE that
/// throws: a joinable std::thread would terminate the process and the
/// std::async future would block in its destructor until the client gives up.
/// Stops the agent (its listener closing is what makes the client give up),
/// stops the feeder, joins both, and waits for the client with a deadline.
struct Teardown {
    std::atomic<bool>& StopAgent;
    std::atomic<bool>& StopFeeder;
    const io::SocketAddress& Wake;
    std::thread& Agent;
    std::thread& Feeder;
    std::future<client::SessionTestResult>& Client;

    bool Done{false};

    ~Teardown() { Finish(); }

    /// Idempotent.
    void Finish() {
        if (std::exchange(Done, true)) {
            return;
        }
        StopFeeder = true;
        StopAgent = true;
        if (Agent.joinable()) {
            // A connection wakes the agent so it sees the flag.
            auto waker = io::StartConnect(Wake);
            std::this_thread::sleep_for(100ms);
            Agent.join();
        }
        if (Feeder.joinable()) {
            Feeder.join();
        }
        // The agent is gone: the client gives up after GiveUpAfter.
        if (Client.valid() &&
            Client.wait_for(15s) != std::future_status::ready) {
            FAIL_CHECK("the client session did not end after the agent left");
            std::quick_exit(1);  // the future's destructor would block forever
        }
    }
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: input left in its buffer moves when an Ack frees "
                  "the ring, with a quiet shell") {
    auto limits = TinyLimits();
    limits.ResendMax = RESEND;
    limits.DisconnectMax = RESEND;
    // Nothing but the Ack wakes the client within the test.
    limits.Liveness =
        LivenessLimits{.KeepaliveInterval = 30s, .PeerSilenceTimeout = 90s};

    auto pty = io::OpenPty();
    REQUIRE(pty.has_value());
    termios t{};
    REQUIRE(::tcgetattr(pty->Slave.Get(), &t) == 0);
    ::cfmakeraw(&t);
    REQUIRE(::tcsetattr(pty->Slave.Get(), TCSANOW, &t) == 0);
    REQUIRE(io::SetNonBlocking(pty->Slave.Get()).has_value());
    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    auto addresses = io::Resolve("127.0.0.1", listener->Port);
    REQUIRE(addresses.has_value());

    auto in = io::MakePipe();
    REQUIRE(in.has_value());
    REQUIRE(io::SetNonBlocking(in->Read.Get()).has_value());
    io::UniqueFd devnull{::open("/dev/null", O_WRONLY)};
    REQUIRE(devnull.Get() >= 0);

    std::atomic<bool> stop{false};
    std::atomic<bool> stopFeeder{false};
    std::thread agent;
    std::thread feeder;
    std::future<client::SessionTestResult> clientDone;
    Teardown teardown{stop,  stopFeeder, addresses->Items[0],
                      agent, feeder,     clientDone};

    agent = std::thread{
        [&, l = std::move(*listener), m = std::move(pty->Master)]() mutable {
            std::ignore = RunAgentForTest(AgentTestConfig{
                .Secrets = DeriveSessionSecrets(MakeKey(93)),
                .Limits = limits,
                .Listen = std::move(l),
                .PtyReadFd = std::move(m),
                .Ceiling = wire::PROTOCOL_VERSION_GRID,
                .GiveUpAfter = std::chrono::seconds{120},
                .Inspect = nullptr,
                .Stop = &stop,
                .InputToPty = true,
            });
        }};
    clientDone = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = wire::DeriveSessionSecrets(MakeKey(93)),
            .Limits = limits,
            .Addresses = *addresses,
            .StdoutFd = devnull.Get(),
            .StderrFd = devnull.Get(),
            .StdinFd = in->Read.Get(),
            .Ceiling = wire::PROTOCOL_VERSION_GRID,
            .BackoffMax = 20ms,
            .GiveUpAfter = 4s,
        });
    });

    std::vector<std::byte> input(INPUT);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i * 131 % 251);
    }
    // Connected and idle before the user types.
    std::this_thread::sleep_for(500ms);
    feeder = std::thread{[&] {
        ByteSpan rest{input};
        while (!rest.empty() && !stopFeeder) {
            auto w = io::WriteSome(in->Write.Get(), rest);
            if (w) {
                rest = rest.subspan(*w);
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
    }};

    // The shell reads what it is given; a stalled client leaves most of it
    // unread until the keepalive, far beyond the deadline.
    std::vector<std::byte> got;
    std::array<std::byte, 65536> buf{};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (got.size() < input.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto r = io::ReadSome(pty->Slave.Get(), MutableByteSpan{buf});
        if (r && *r > 0) {
            got.insert(got.end(), buf.begin(), buf.begin() + *r);
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
    teardown.Finish();

    CHECK(got.size() == input.size());
    CHECK(got == input);
}
