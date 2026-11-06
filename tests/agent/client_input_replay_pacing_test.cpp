/// The real client's replay of input typed while detached (design.md §9),
/// against the real agent, over TCP and a real pty.
///
/// A detached client takes stdin up to DisconnectMax. Its socket queue is
/// ResendMax plus a fixed slack, so the input replay after the handshake is
/// larger than the queue and has to go out as the socket drains. A queue that
/// refuses the whole replay closes the connection, and every reconnect does the
/// same again, forever.
///
/// Kills: the send gate in Client::Pump removed (the replay overflows
/// SocketOut_, the link is dropped on every reconnect and nothing arrives);
/// the gate never reopened (the replay stops after the first queue-full); a
/// live connection dropped once (the client's lost-connection notice reaches
/// stderr).

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
#include <string>
#include <thread>
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

constexpr std::size_t RESEND = 65536;
constexpr std::size_t DISCONNECT = 3U * 1024U * 1024U;
// More than the client's socket queue holds (RESEND + 1 MiB of slack), less
// than the disconnect buffer.
constexpr std::size_t INPUT = 2U * 1024U * 1024U + 512U * 1024U;

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: an input replay larger than the socket queue is "
                  "paced by the socket") {
    auto limits = TinyLimits();
    limits.ResendMax = RESEND;
    limits.DisconnectMax = DISCONNECT;

    auto pty = io::OpenPty();
    REQUIRE(pty.has_value());
    termios t{};
    REQUIRE(::tcgetattr(pty->Slave.Get(), &t) == 0);
    ::cfmakeraw(&t);
    REQUIRE(::tcsetattr(pty->Slave.Get(), TCSANOW, &t) == 0);
    REQUIRE(io::SetNonBlocking(pty->Slave.Get()).has_value());
    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->Port;
    auto addresses = io::Resolve("127.0.0.1", port);
    REQUIRE(addresses.has_value());

    auto in = io::MakePipe();
    REQUIRE(in.has_value());
    REQUIRE(io::SetNonBlocking(in->Read.Get()).has_value());
    io::UniqueFd devnull{::open("/dev/null", O_WRONLY)};
    REQUIRE(devnull.Get() >= 0);
    // The client prints its lost-connection notice here, and only when a live
    // connection drops.
    char errPath[] = "/tmp/zet_replay_pacing_stderr_XXXXXX";
    io::UniqueFd errFile{::mkstemp(errPath)};
    REQUIRE(errFile.Get() >= 0);
    ::unlink(errPath);

    // Nobody answers on the listener yet: the client is detached and takes
    // the whole input into its session.
    auto clientDone = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = wire::DeriveSessionSecrets(MakeKey(93)),
            .Limits = limits,
            .Addresses = *addresses,
            .StdoutFd = devnull.Get(),
            .StderrFd = errFile.Get(),
            .StdinFd = in->Read.Get(),
            .NoticesInline = false,
            .Ceiling = wire::PROTOCOL_VERSION_GRID,
            .BackoffMax = 20ms,
            .GiveUpAfter = 4s,
        });
    });

    std::vector<std::byte> input(INPUT);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i * 131 % 251);
    }
    std::thread feeder{[&] {
        ByteSpan rest{input};
        while (!rest.empty()) {
            auto w = io::WriteSome(in->Write.Get(), rest);
            if (w) {
                rest = rest.subspan(*w);
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
    }};
    std::this_thread::sleep_for(1500ms);  // the client has read it all

    std::atomic<bool> stop{false};
    std::thread agent{
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

    std::vector<std::byte> got;
    std::array<std::byte, 65536> buf{};
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (got.size() < input.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto r = io::ReadSome(pty->Slave.Get(), MutableByteSpan{buf});
        if (r && *r > 0) {
            got.insert(got.end(), buf.begin(), buf.begin() + *r);
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }

    // One connection carried the whole replay: nothing live was dropped.
    std::string errText(256, '\0');
    const ssize_t errLen =
        ::pread(errFile.Get(), errText.data(), errText.size(), 0);
    REQUIRE(errLen >= 0);
    errText.resize(static_cast<std::size_t>(errLen));
    CHECK(errText.empty());

    stop = true;
    // A connection wakes the agent so it sees the flag.
    auto waker = io::StartConnect(addresses->Items[0]);
    std::this_thread::sleep_for(100ms);
    agent.join();
    // The agent is gone: the client gives up after GiveUpAfter.
    CHECK(clientDone.wait_for(15s) == std::future_status::ready);
    feeder.join();

    CHECK(got.size() == input.size());
    CHECK(got == input);
}
