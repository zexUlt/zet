#include "zet/session/connection.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "zet/core/byte_writer.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// Room for every byte a test sends, so that no test meets backpressure by
/// accident; the ones about backpressure live with StreamState.
constexpr std::size_t STREAM_STORAGE = 64 * 1024;

/// A shuttle that has not gone quiet after this many steps is spinning.
constexpr int MAX_STEPS = 10'000;

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t value = seed;
    for (auto& byte : bytes) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 5);
    }
    return crypto::Key{bytes};
}

/// Sixteen bytes no other call returns, the way a generator would.
HandshakeNonce NextNonce() {
    static std::uint32_t counter = 0;
    ++counter;
    HandshakeNonce nonce{};
    std::uint32_t value = counter;
    for (auto& byte : nonce) {
        byte = static_cast<std::byte>(value & 0xFFU);
        value = value * 2654435761U + 1U;
    }
    return nonce;
}

std::vector<std::byte> Pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes.at(i) = static_cast<std::byte>((seed + i * 7U) & 0xFFU);
    }
    return bytes;
}

SessionStreams Streams(std::vector<std::byte>& storage) {
    return SessionStreams{.Storage = MutableByteSpan{storage},
                          .Now = START,
                          .Limits = SessionLimits{}};
}

/// Both ends of one session, as they live across connections.
struct Ends {
    std::vector<std::byte> AgentStorage =
        std::vector<std::byte>(STREAM_STORAGE);
    std::vector<std::byte> ClientStorage =
        std::vector<std::byte>(STREAM_STORAGE);
    AgentSession Agent;
    ClientSession Client;
    crypto::Key LocalSecret = MakeKey(99);
    /// What each local side has been handed so far, in order.
    std::vector<std::byte> AtPty;
    std::vector<std::byte> AtTerminal;
    std::optional<wire::ResizeMessage> LastResize;

    explicit Ends(std::uint8_t agentMaster = 1, std::uint8_t clientMaster = 1)
        : Agent(DeriveSessionSecrets(MakeKey(agentMaster)),
                Streams(AgentStorage)),
          Client(DeriveSessionSecrets(MakeKey(clientMaster)),
                 Streams(ClientStorage)) {}
};

/// What a direction of the link does to the frames it carries.
enum class ECut : std::uint8_t {
    None,
    /// Everything the agent sends after its AuthOk is lost: Resume arrives,
    /// the answer to it never does.
    AfterAgentAuthOk,
};

/// One TCP connection between the two ends, frames carried whole.
struct Link {
    Ends& Session;
    std::vector<std::byte> AgentFrames =
        std::vector<std::byte>(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> ClientFrames =
        std::vector<std::byte>(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> Out = std::vector<std::byte>(CONNECTION_OUT_SIZE);
    std::vector<std::byte> Scratch =
        std::vector<std::byte>(CONNECTION_SCRATCH_SIZE);
    AgentConnection Agent;
    ClientConnection Client;
    bool RouteToUnknown{false};
    ECut Cut{ECut::None};
    int AgentFramesSent{0};
    std::optional<SessionError> AgentError;
    std::optional<SessionError> ClientError;

    Link(Ends& ends, Timestamp now)
        : Session(ends),
          Agent(MutableByteSpan{AgentFrames}, now),
          Client(ends.Client, NextNonce(), MutableByteSpan{ClientFrames}, now) {
    }

    /// Pumps both ends until neither has anything left to do.
    void Shuttle(Timestamp now) {
        for (int step = 0; step < MAX_STEPS; ++step) {
            const bool agentBusy = !AgentError && PumpAgent(now);
            const bool clientBusy = !ClientError && PumpClient(now);
            if (!agentBusy && !clientBusy) {
                return;
            }
        }
        FAIL("the link never went quiet");
    }

    bool PumpAgent(Timestamp now) {
        auto event =
            Agent.Pump(now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
        if (!event) {
            AgentError = event.error();
            return false;
        }
        if (const auto* need = std::get_if<NeedsSession>(&*event)) {
            if (RouteToUnknown || need->Id != Session.Agent.Ref().Id()) {
                Agent.RouteUnknown(Session.LocalSecret, NextNonce());
            } else {
                Agent.Route(Session.Agent.Ref(), NextNonce());
            }
            return true;
        }
        if (const auto* send = std::get_if<SendFrame>(&*event)) {
            ++AgentFramesSent;
            // Challenge and AuthOk are the agent's first two frames.
            const bool lost =
                Cut == ECut::AfterAgentAuthOk && AgentFramesSent > 2;
            if (!lost) {
                REQUIRE(Client.OnBytes(ByteSpan{Out}.first(send->Written)));
            }
            return true;
        }
        if (const auto* deliver = std::get_if<Deliver>(&*event)) {
            Session.AtPty.insert(Session.AtPty.end(), deliver->Bytes.begin(),
                                 deliver->Bytes.end());
            return true;
        }
        if (const auto* resized = std::get_if<Resized>(&*event)) {
            Session.LastResize = resized->Size;
            return true;
        }
        return false;
    }

    bool PumpClient(Timestamp now) {
        auto event =
            Client.Pump(now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
        if (!event) {
            ClientError = event.error();
            return false;
        }
        if (const auto* send = std::get_if<SendFrame>(&*event)) {
            const auto accepted =
                Agent.OnBytes(ByteSpan{Out}.first(send->Written));
            if (!accepted) {
                AgentError = accepted.error();
            }
            return true;
        }
        if (const auto* deliver = std::get_if<Deliver>(&*event)) {
            Session.AtTerminal.insert(Session.AtTerminal.end(),
                                      deliver->Bytes.begin(),
                                      deliver->Bytes.end());
            return true;
        }
        return false;
    }

    void Close(Timestamp now) {
        Agent.Close(now);
        Client.Close(now);
    }
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a resume carries both streams byte for byte") {
    Ends ends;
    // The shell printed while nobody was connected; the user typed ahead.
    const auto output = Pattern(5000, 1);
    const auto typed = Pattern(300, 2);
    REQUIRE(ends.Agent.Write(ByteSpan{output}) == output.size());
    REQUIRE(ends.Client.Write(ByteSpan{typed}) == typed.size());

    Link link{ends, START};
    link.Shuttle(START);

    CHECK_FALSE(link.AgentError.has_value());
    CHECK_FALSE(link.ClientError.has_value());
    CHECK(link.Agent.IsLive());
    CHECK(link.Client.IsLive());
    CHECK(ends.Agent.State() == ESessionState::Live);
    CHECK(ends.AtTerminal == output);
    CHECK(ends.AtPty == typed);

    // And it keeps flowing once live.
    const auto more = Pattern(700, 3);
    REQUIRE(ends.Agent.Write(ByteSpan{more}) == more.size());
    link.Shuttle(START + 1ms);
    CHECK(std::vector<std::byte>(ends.AtTerminal.begin() + 5000,
                                 ends.AtTerminal.end()) == more);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a resize queued before Resume reaches the agent") {
    Ends ends;
    Link link{ends, START};
    link.Client.QueueResize(
        wire::ResizeMessage{.Rows = 50, .Cols = 132, .XPixel = 0, .YPixel = 0});
    link.Shuttle(START);
    REQUIRE(ends.LastResize.has_value());
    CHECK(ends.LastResize->Rows == 50);
    CHECK(ends.LastResize->Cols == 132);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a reconnect picks up exactly where the last one stopped") {
    Ends ends;
    const auto before = Pattern(3000, 4);
    REQUIRE(ends.Agent.Write(ByteSpan{before}) == before.size());
    {
        Link first{ends, START};
        first.Shuttle(START);
        REQUIRE(first.Client.IsLive());
        first.Close(START + 1s);
    }
    CHECK(ends.Agent.State() == ESessionState::Detached);

    // Written while the network was down.
    const auto during = Pattern(2000, 5);
    REQUIRE(ends.Agent.Write(ByteSpan{during}) == during.size());

    Link second{ends, START + 2s};
    second.Shuttle(START + 2s);
    CHECK_FALSE(second.AgentError.has_value());
    CHECK_FALSE(second.ClientError.has_value());
    REQUIRE(second.Client.IsLive());

    auto expected = before;
    expected.insert(expected.end(), during.begin(), during.end());
    CHECK(ends.AtTerminal == expected);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a client whose answer to Resume was cut is met on the old seed") {
    // §8 end to end: Resume opened and the agent moved on; its first sealed
    // frame was lost, so the client did not. The next connection must still
    // open, through the agent's fallback keys.
    Ends ends;
    const auto output = Pattern(1000, 6);
    REQUIRE(ends.Agent.Write(ByteSpan{output}) == output.size());
    {
        Link first{ends, START};
        first.Cut = ECut::AfterAgentAuthOk;
        first.Shuttle(START);
        REQUIRE(first.Agent.IsLive());
        CHECK(ends.AtTerminal.empty());
        first.Close(START + 1s);
    }

    Link second{ends, START + 2s};
    second.Shuttle(START + 2s);
    CHECK_FALSE(second.AgentError.has_value());
    CHECK_FALSE(second.ClientError.has_value());
    REQUIRE(second.Client.IsLive());
    CHECK(ends.AtTerminal == output);

    // Both are on the same seed again: a third connection needs no fallback.
    second.Close(START + 3s);
    Link third{ends, START + 4s};
    third.Shuttle(START + 4s);
    CHECK(third.Client.IsLive());
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "an unknown sid is answered like a real one and never let in") {
    Ends ends;
    Link link{ends, START};
    link.RouteToUnknown = true;
    link.Shuttle(START);

    CHECK_FALSE(link.Agent.IsLive());
    CHECK_FALSE(link.Client.IsLive());
    REQUIRE(link.AgentError.has_value());
    CHECK(link.AgentError->Proto() == EProtoError::AuthenticationFailed);
    CHECK(link.AgentError->Disposition() == EDisposition::CloseConnection);
    CHECK(ends.Agent.State() == ESessionState::Detached);
    CHECK(ends.Agent.Ref().RememberedNonces() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client without K_auth leaves the session as it found it") {
    Ends ends{1, 2};
    Link link{ends, START};
    link.Shuttle(START);

    REQUIRE(link.AgentError.has_value());
    CHECK(link.AgentError->Disposition() == EDisposition::CloseConnection);
    CHECK(ends.Agent.State() == ESessionState::Detached);
    CHECK(ends.Agent.Ref().RememberedNonces() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a newer connection takes over and the old one stands down") {
    Ends ends;
    Link first{ends, START};
    first.Shuttle(START);
    REQUIRE(first.Agent.IsLive());

    // The client moved networks; the old socket has not noticed yet.
    Link second{ends, START + 1s};
    second.Shuttle(START + 1s);
    REQUIRE(second.Agent.IsLive());

    const auto event = first.Agent.Pump(START + 2s, MutableByteSpan{first.Out},
                                        MutableByteSpan{first.Scratch});
    REQUIRE_FALSE(event.has_value());
    CHECK(event.error().Session() == ESessionError::Superseded);
    CHECK(event.error().Disposition() == EDisposition::CloseConnection);

    first.Close(START + 3s);
    CHECK(ends.Agent.State() == ESessionState::Live);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a handshake that does not finish in time closes the connection") {
    Ends ends;
    std::vector<std::byte> frames(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> out(CONNECTION_OUT_SIZE);
    std::vector<std::byte> scratch(CONNECTION_SCRATCH_SIZE);
    AgentConnection agent{MutableByteSpan{frames}, START};

    CHECK(agent.NextDeadline().HasExpired(START + HANDSHAKE_TIMEOUT));
    CHECK_FALSE(
        agent.NextDeadline().HasExpired(START + HANDSHAKE_TIMEOUT - 1ns));

    const auto early =
        agent.Pump(START + 1s, MutableByteSpan{out}, MutableByteSpan{scratch});
    REQUIRE(early.has_value());
    CHECK(std::holds_alternative<Idle>(*early));

    const auto late =
        agent.Pump(START + HANDSHAKE_TIMEOUT, MutableByteSpan{out},
                   MutableByteSpan{scratch});
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().Session() == ESessionError::HandshakeTimeout);
    // A failed connection stays failed, and has no deadline left to keep.
    CHECK_FALSE(agent.NextDeadline().IsSet());
    const auto again =
        agent.Pump(START + HANDSHAKE_TIMEOUT, MutableByteSpan{out},
                   MutableByteSpan{scratch});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == late.error());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a cleartext Hello cannot reap the session it names") {
    // §5: VersionMismatch disposes as KillSession, and a Hello is unproved.
    // The cap PreAuthError puts on it has to survive the trip up to here.
    Ends ends;
    wire::HelloMessage hello{};
    hello.VersionMin = 9;
    hello.VersionMax = 9;
    hello.Id = ends.Agent.Ref().Id();
    hello.ClientNonce = NextNonce();
    std::vector<std::byte> frame(wire::HEADER_SIZE + wire::MAX_HANDSHAKE_FRAME);
    ByteWriter body{MutableByteSpan{frame}.subspan(wire::HEADER_SIZE)};
    REQUIRE(wire::WriteHello(body, hello));
    ByteWriter header{MutableByteSpan{frame}.first(wire::HEADER_SIZE)};
    REQUIRE(wire::WriteFrameHeader(
        header,
        wire::FrameHeader{.Length = static_cast<std::uint32_t>(body.Size()),
                          .Epoch = 0}));

    std::vector<std::byte> frames(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> out(CONNECTION_OUT_SIZE);
    std::vector<std::byte> scratch(CONNECTION_SCRATCH_SIZE);
    AgentConnection agent{MutableByteSpan{frames}, START};
    REQUIRE(
        agent.OnBytes(ByteSpan{frame}.first(wire::HEADER_SIZE + body.Size())));

    const auto need =
        agent.Pump(START, MutableByteSpan{out}, MutableByteSpan{scratch});
    REQUIRE(need.has_value());
    REQUIRE(std::holds_alternative<NeedsSession>(*need));
    agent.Route(ends.Agent.Ref(), NextNonce());

    const auto refused =
        agent.Pump(START, MutableByteSpan{out}, MutableByteSpan{scratch});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Proto() == EProtoError::VersionMismatch);
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
    CHECK(ends.Agent.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a Resume that does not open leaves the session detached") {
    Ends ends;
    Link link{ends, START};
    // Hello through AuthOk travel as usual; the client's first sealed frame,
    // Resume, has its last byte flipped on the way to the agent.
    bool bent = false;
    for (int step = 0; step < MAX_STEPS && !bent; ++step) {
        const bool agentBusy = link.PumpAgent(START);
        auto event = link.Client.Pump(START, MutableByteSpan{link.Out},
                                      MutableByteSpan{link.Scratch});
        REQUIRE(event.has_value());
        if (const auto* send = std::get_if<SendFrame>(&*event)) {
            std::vector<std::byte> bytes(
                link.Out.begin(),
                link.Out.begin() + static_cast<std::ptrdiff_t>(send->Written));
            if (link.Client.IsLive()) {
                bytes.back() ^= std::byte{0x01};
                bent = true;
            }
            REQUIRE(link.Agent.OnBytes(ByteSpan{bytes}));
        } else if (!agentBusy) {
            break;
        }
    }
    REQUIRE(bent);
    link.Shuttle(START);

    REQUIRE(link.AgentError.has_value());
    CHECK(link.AgentError->Proto() == EProtoError::AuthenticationFailed);
    CHECK(link.AgentError->Disposition() == EDisposition::CloseConnection);
    CHECK_FALSE(link.Agent.IsLive());
    CHECK(ends.Agent.State() == ESessionState::Detached);
}
