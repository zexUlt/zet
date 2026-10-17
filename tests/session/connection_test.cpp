#include "zet/session/connection.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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

// ── Refusals and corners ───────────────────────────────────────────────

namespace {

/// A frame as it would come off the socket: header, then `body` as is.
std::vector<std::byte> RawFrame(std::uint8_t epoch, ByteSpan body,
                                std::uint32_t declared = UINT32_MAX) {
    std::vector<std::byte> frame(wire::HEADER_SIZE + body.size());
    ByteWriter header{MutableByteSpan{frame}.first(wire::HEADER_SIZE)};
    REQUIRE(wire::WriteFrameHeader(
        header, wire::FrameHeader{
                    .Length = declared == UINT32_MAX
                                  ? static_cast<std::uint32_t>(body.size())
                                  : declared,
                    .Epoch = epoch}));
    std::ranges::copy(body, frame.begin() + wire::HEADER_SIZE);
    return frame;
}

/// Buffers for driving one end by hand.
struct Hands {
    std::vector<std::byte> Frames =
        std::vector<std::byte>(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> Out = std::vector<std::byte>(CONNECTION_OUT_SIZE);
    std::vector<std::byte> Scratch =
        std::vector<std::byte>(CONNECTION_SCRATCH_SIZE);

    template <typename TConnection>
    ConnectionResult Pump(TConnection& connection, Timestamp now) {
        return connection.Pump(now, MutableByteSpan{Out},
                               MutableByteSpan{Scratch});
    }
};

/// Drives `client` against an agent routed to `session` until either end
/// refuses or both go quiet. Returns the agent's refusal, if any.
std::optional<SessionError> DriveAgainst(ClientSession& client,
                                         AgentSession& session,
                                         HandshakeNonce nonce, Timestamp now) {
    Hands agentHands;
    Hands clientHands;
    AgentConnection agent{MutableByteSpan{agentHands.Frames}, now};
    ClientConnection connection{client, nonce,
                                MutableByteSpan{clientHands.Frames}, now};
    for (int step = 0; step < MAX_STEPS; ++step) {
        bool busy = false;
        auto toAgent = clientHands.Pump(connection, now);
        if (!toAgent) {
            return std::nullopt;
        }
        if (const auto* send = std::get_if<SendFrame>(&*toAgent)) {
            busy = true;
            REQUIRE(
                agent.OnBytes(ByteSpan{clientHands.Out}.first(send->Written)));
        }
        auto toClient = agentHands.Pump(agent, now);
        if (!toClient) {
            return toClient.error();
        }
        if (std::holds_alternative<NeedsSession>(*toClient)) {
            busy = true;
            agent.Route(session.Ref(), NextNonce());
        } else if (const auto* send = std::get_if<SendFrame>(&*toClient)) {
            busy = true;
            REQUIRE(connection.OnBytes(
                ByteSpan{agentHands.Out}.first(send->Written)));
        }
        if (!busy) {
            agent.Close(now);
            connection.Close(now);
            return std::nullopt;
        }
    }
    FAIL("never went quiet");
    return std::nullopt;
}

/// Pumps both ends of `link` until the client is live and the agent has
/// nothing left to say, for tests that start from a live connection.
void BringUp(Link& link, Timestamp now) {
    link.Shuttle(now);
    REQUIRE(link.Client.IsLive());
    REQUIRE(link.Agent.IsLive());
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "buffers smaller than Pump asks for are refused, not used") {
    Ends ends;
    Hands hands;
    AgentConnection agent{MutableByteSpan{hands.Frames}, START};
    std::vector<std::byte> small(wire::HEADER_SIZE);
    const auto refused = agent.Pump(START, MutableByteSpan{small},
                                    MutableByteSpan{hands.Scratch});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Proto() == EProtoError::BufferTooSmall);
    // A defect in the caller, not the peer: the connection carries on.
    CHECK(hands.Pump(agent, START).has_value());

    ClientConnection client{ends.Client, NextNonce(),
                            MutableByteSpan{hands.Frames}, START};
    const auto cramped =
        client.Pump(START, MutableByteSpan{hands.Out}, MutableByteSpan{small});
    REQUIRE_FALSE(cramped.has_value());
    CHECK(cramped.error().Proto() == EProtoError::BufferTooSmall);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "garbage before a Hello closes the connection, capped") {
    struct Case {
        const char* Name;
        std::vector<std::byte> Bytes;
    };
    const std::vector<std::byte> junk(8, std::byte{0x7F});
    const std::vector<Case> cases{
        {"a frame longer than a stranger may send",
         RawFrame(0, ByteSpan{junk}, wire::MAX_HANDSHAKE_FRAME + 1)},
        {"a sealed epoch on a cleartext frame", RawFrame(1, ByteSpan{junk})},
        {"a body that is no Hello", RawFrame(0, ByteSpan{junk})},
    };
    for (const auto& c : cases) {
        CAPTURE(c.Name);
        Hands hands;
        AgentConnection agent{MutableByteSpan{hands.Frames}, START};
        REQUIRE(agent.OnBytes(ByteSpan{c.Bytes}));
        const auto refused = hands.Pump(agent, START);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
        // Failed stays failed, for bytes as for pumps.
        CHECK_FALSE(agent.OnBytes(ByteSpan{junk}).has_value());
        CHECK_FALSE(hands.Pump(agent, START).has_value());
        CHECK_FALSE(agent.NextDeadline().IsSet());
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "more bytes than one handshake frame can hold are refused") {
    Hands hands;
    AgentConnection agent{MutableByteSpan{hands.Frames}, START};
    const std::vector<std::byte> flood(
        wire::HEADER_SIZE + wire::MAX_HANDSHAKE_FRAME + 1, std::byte{0});
    const auto refused = agent.OnBytes(ByteSpan{flood});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the agent keeps asking for a session until it is told") {
    Ends ends;
    Link link{ends, START};
    REQUIRE(link.PumpClient(START));
    const auto first = link.Agent.Pump(START, MutableByteSpan{link.Out},
                                       MutableByteSpan{link.Scratch});
    REQUIRE(first.has_value());
    REQUIRE(std::holds_alternative<NeedsSession>(*first));
    const auto again = link.Agent.Pump(START, MutableByteSpan{link.Out},
                                       MutableByteSpan{link.Scratch});
    REQUIRE(again.has_value());
    CHECK(std::get_if<NeedsSession>(&*again)->Id == ends.Agent.Ref().Id());

    // Routing twice is a no-op: the attempt is built once.
    link.Agent.Route(ends.Agent.Ref(), NextNonce());
    link.Agent.RouteUnknown(ends.LocalSecret, NextNonce());
    link.Shuttle(START);
    CHECK(link.Client.IsLive());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a sealed epoch inside the handshake closes both ends") {
    Ends ends;
    Link link{ends, START};
    REQUIRE(link.PumpClient(START));  // Hello
    REQUIRE(link.PumpAgent(START));   // NeedsSession
    REQUIRE(link.PumpAgent(START));   // Challenge, into the client
    // The agent now waits for Auth and has nothing to say.
    const auto waiting = link.Agent.Pump(START, MutableByteSpan{link.Out},
                                         MutableByteSpan{link.Scratch});
    REQUIRE(waiting.has_value());
    CHECK(std::holds_alternative<Idle>(*waiting));

    const std::vector<std::byte> junk(4, std::byte{0x01});
    REQUIRE(link.Agent.OnBytes(ByteSpan{RawFrame(1, ByteSpan{junk})}));
    const auto agentRefused = link.Agent.Pump(START, MutableByteSpan{link.Out},
                                              MutableByteSpan{link.Scratch});
    REQUIRE_FALSE(agentRefused.has_value());
    CHECK(agentRefused.error().Disposition() == EDisposition::CloseConnection);

    // The client, waiting on Challenge, takes the same refusal.
    Ends other;
    Hands hands;
    ClientConnection client{other.Client, NextNonce(),
                            MutableByteSpan{hands.Frames}, START};
    REQUIRE(hands.Pump(client, START).has_value());  // Hello
    const auto idle = hands.Pump(client, START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));
    REQUIRE(client.OnBytes(ByteSpan{RawFrame(1, ByteSpan{junk})}));
    const auto clientRefused = hands.Pump(client, START);
    REQUIRE_FALSE(clientRefused.has_value());
    CHECK(clientRefused.error().Disposition() == EDisposition::CloseConnection);
    CHECK(other.Client.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a client refuses a Challenge that is not one, and times out") {
    Ends ends;
    Hands hands;
    ClientConnection client{ends.Client, NextNonce(),
                            MutableByteSpan{hands.Frames}, START};
    REQUIRE(hands.Pump(client, START).has_value());  // Hello
    const std::vector<std::byte> junk(6, std::byte{0x02});
    REQUIRE(client.OnBytes(ByteSpan{RawFrame(0, ByteSpan{junk})}));
    const auto refused = hands.Pump(client, START);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);

    Hands later;
    ClientConnection slow{ends.Client, NextNonce(),
                          MutableByteSpan{later.Frames}, START};
    const auto expired = later.Pump(slow, START + HANDSHAKE_TIMEOUT);
    REQUIRE_FALSE(expired.has_value());
    CHECK(expired.error().Session() == ESessionError::HandshakeTimeout);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client will not send Hello on a nonce it has used") {
    // §5 rule three on the client's own generator: a nonce from a finished
    // handshake is caught before a byte goes out.
    Ends ends;
    const HandshakeNonce nonce = NextNonce();
    REQUIRE_FALSE(DriveAgainst(ends.Client, ends.Agent, nonce, START));
    REQUIRE(ends.Client.RememberedNonces() == 2);

    Hands hands;
    ClientConnection repeat{ends.Client, nonce, MutableByteSpan{hands.Frames},
                            START + 1s};
    const auto refused = hands.Pump(repeat, START + 1s);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Proto() == EProtoError::NonceReused);
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a live connection reports its channel's deadline and resizes") {
    Ends ends;
    Link link{ends, START};
    BringUp(link, START);
    // Live: no handshake deadline any more, only the channel's own.
    CHECK_FALSE(
        link.Agent.NextDeadline().HasExpired(START + HANDSHAKE_TIMEOUT));
    CHECK(link.Agent.NextDeadline().HasExpired(START + REKEY_INTERVAL));

    link.Client.QueueResize(
        wire::ResizeMessage{.Rows = 24, .Cols = 80, .XPixel = 0, .YPixel = 0});
    link.Shuttle(START + 1ms);
    REQUIRE(ends.LastResize.has_value());
    CHECK(ends.LastResize->Cols == 80);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a bent frame on a live connection closes it, not the session") {
    Ends ends;
    Link link{ends, START};
    BringUp(link, START);
    const std::vector<std::byte> junk(32, std::byte{0x03});
    REQUIRE(link.Agent.OnBytes(ByteSpan{RawFrame(0, ByteSpan{junk})}));
    const auto refused = link.Agent.Pump(START, MutableByteSpan{link.Out},
                                         MutableByteSpan{link.Scratch});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
    link.Close(START);
    CHECK(ends.Agent.State() == ESessionState::Detached);

    // And on the client's side: a frame declaring more than MAX_FRAME.
    Link second{ends, START + 1s};
    BringUp(second, START + 1s);
    REQUIRE(second.Client.OnBytes(
        ByteSpan{RawFrame(0, ByteSpan{junk}, wire::MAX_FRAME + 1)}));
    const auto tooLong =
        second.Client.Pump(START + 1s, MutableByteSpan{second.Out},
                           MutableByteSpan{second.Scratch});
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a superseded client connection stands down too") {
    Ends ends;
    Link first{ends, START};
    BringUp(first, START);
    Link second{ends, START + 1s};
    BringUp(second, START + 1s);
    const auto stale = first.Client.Pump(START + 2s, MutableByteSpan{first.Out},
                                         MutableByteSpan{first.Scratch});
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error().Session() == ESessionError::Superseded);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client claiming bytes the agent never sent is refused") {
    // The client of this session once held 100 bytes; the agent it now meets
    // has the same secrets, the same seed, and a stream that never got that
    // far — as if it had been restored from an older snapshot.
    Ends ends;
    const auto output = Pattern(100, 7);
    REQUIRE(ends.Agent.Write(ByteSpan{output}) == output.size());
    {
        Link first{ends, START};
        first.Shuttle(START);
        REQUIRE(ends.Client.ReceiveOffset() == output.size());
        first.Close(START + 1s);
    }
    // One connection with nothing in it brings the other agent's seed to
    // where the client's is now.
    Ends behind;
    {
        Link warm{behind, START};
        warm.Shuttle(START);
        REQUIRE(warm.Client.IsLive());
        warm.Close(START + 1s);
    }

    const auto refusal =
        DriveAgainst(ends.Client, behind.Agent, NextNonce(), START + 2s);
    REQUIRE(refusal.has_value());
    CHECK(refusal->Session() == ESessionError::AckBeyondSent);
    CHECK(refusal->Disposition() == EDisposition::CloseConnection);
    CHECK(behind.Agent.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client that lost what it acknowledged ends the session") {
    // §9: the agent trimmed what the client confirmed holding; a client that
    // now asks for it again cannot be served whole, and losing bytes silently
    // is not allowed — the session ends, with the reason.
    Ends ends;
    const auto output = Pattern(100, 8);
    REQUIRE(ends.Agent.Write(ByteSpan{output}) == output.size());
    {
        Link first{ends, START};
        first.Shuttle(START);
        // Let the idle Ack go out, so the agent trims what the client holds.
        first.Shuttle(START + ACK_IDLE_DELAY);
        first.Shuttle(START + 2 * ACK_IDLE_DELAY);
        first.Close(START + 1s);
    }
    // A client with the same secrets, a step behind on the seed like this
    // one, and nothing received: one empty connection to another agent.
    Ends forgetful;
    {
        Link warm{forgetful, START};
        warm.Shuttle(START);
        warm.Close(START + 1s);
    }
    REQUIRE(forgetful.Client.ReceiveOffset() == 0);

    const auto refusal =
        DriveAgainst(forgetful.Client, ends.Agent, NextNonce(), START + 2s);
    REQUIRE(refusal.has_value());
    CHECK(refusal->Session() == ESessionError::PeerBehindBuffer);
    CHECK(refusal->Disposition() == EDisposition::KillSession);
    CHECK(ends.Agent.State() == ESessionState::Reaped);
    CHECK(ends.Agent.ReapReason() == ESessionError::PeerBehindBuffer);
}
