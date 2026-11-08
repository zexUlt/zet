#include <tuple>
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

SessionStreams Streams(std::vector<std::byte>& storage,
                       SessionLimits limits = {}) {
    return SessionStreams{
        .Storage = MutableByteSpan{storage}, .Now = START, .Limits = limits};
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
    /// Every SessionEnd the client heard, and how much of the agent's stream
    /// it held at each.
    std::vector<std::pair<wire::SessionEndMessage, std::size_t>> EndsHeard;
    /// True once the client received a SessionOverflowed event (kind=2, §7).
    bool OverflowEnded{false};

    explicit Ends(std::uint8_t agentMaster = 1, std::uint8_t clientMaster = 1,
                  SessionLimits limits = {})
        : Agent(DeriveSessionSecrets(MakeKey(agentMaster)),
                Streams(AgentStorage, limits)),
          Client(DeriveSessionSecrets(MakeKey(clientMaster)),
                 Streams(ClientStorage, limits)) {}
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
    /// The widest frame body either end put on the wire, header excluded.
    std::size_t MaxAgentBody{0};
    std::size_t MaxClientBody{0};
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
            MaxAgentBody =
                std::max(MaxAgentBody, send->Written - wire::HEADER_SIZE);
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
            MaxClientBody =
                std::max(MaxClientBody, send->Written - wire::HEADER_SIZE);
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
        if (const auto* ended = std::get_if<Ended>(&*event)) {
            Session.EndsHeard.emplace_back(ended->End,
                                           Session.AtTerminal.size());
            return true;
        }
        if (std::holds_alternative<SessionOverflowed>(*event)) {
            Session.OverflowEnded = true;
            return true;
        }
        return false;
    }

    void Close(Timestamp now) {
        Agent.Close(now);
        Client.Close(now);
    }
};

/// Frames the client put on an old connection that reach the agent only after
/// the agent has already started answering a newer one. The agent's reply to
/// the newer one (AuthOk) quotes the receive offset from before they landed;
/// the client rewinds its send cursor to that quote, and the agent then
/// honestly acks the larger offset it holds now. The ack is below what the
/// client sealed on the old connection, so it must not close the connection.
TEST_CASE_FIXTURE(SodiumFixture,
                  "an ack above the rewound send cursor but below what was "
                  "sealed on the old connection is not a lie") {
    Ends ends;
    Link a{ends, START};
    a.Shuttle(START);
    REQUIRE(a.Agent.IsLive());
    REQUIRE(a.Client.IsLive());

    // The user pastes: several frames sealed on A, none of them landed yet.
    const auto typed = Pattern(4 * wire::SEND_DATA_PAYLOAD_MAX, 7);
    REQUIRE(ends.Client.Write(ByteSpan{typed}) == typed.size());
    std::vector<std::vector<std::byte>> inFlight;
    for (;;) {
        auto event = a.Client.Pump(START, MutableByteSpan{a.Out},
                                   MutableByteSpan{a.Scratch});
        REQUIRE(event.has_value());
        const auto* send = std::get_if<SendFrame>(&*event);
        if (send == nullptr) {
            break;
        }
        inFlight.emplace_back(
            a.Out.begin(),
            a.Out.begin() + static_cast<std::ptrdiff_t>(send->Written));
    }
    REQUIRE(inFlight.size() >= 4);

    // The client gives up on A and dials B; A is still alive at the agent.
    a.Client.Close(START);
    Link b{ends, START};
    REQUIRE(b.PumpClient(START));  // Hello
    REQUIRE(b.PumpAgent(START));   // NeedsSession -> Route: offset quoted now
    REQUIRE(ends.AtPty.empty());

    // The in-flight frames land on A.
    for (const auto& frame : inFlight) {
        REQUIRE(a.Agent.OnBytes(ByteSpan{frame}));
        for (int i = 0; i < 4; ++i) {
            std::ignore = a.PumpAgent(START);
        }
    }
    REQUIRE(ends.AtPty == typed);
    // The pty took it: only now does the agent acknowledge it.
    REQUIRE(ends.Agent.InputDelivered(typed.size()));

    // The shell printed something, so the agent's first frame on B is Data
    // carrying the ack of everything it now holds.
    const auto output = Pattern(3000, 1);
    REQUIRE(ends.Agent.Write(ByteSpan{output}) == output.size());
    b.Shuttle(START);

    if (b.ClientError) {
        MESSAGE("client error: " << b.ClientError->Description());
    }
    CHECK_FALSE(b.ClientError.has_value());
    CHECK_FALSE(b.AgentError.has_value());
    CHECK(b.Client.IsLive());
    CHECK(ends.AtPty == typed);
    CHECK(ends.AtTerminal == output);

    // The resend after such an ack neither loses nor repeats a byte: more
    // input follows on B and reaches the pty exactly once.
    const auto more = Pattern(2 * wire::SEND_DATA_PAYLOAD_MAX, 99);
    REQUIRE(ends.Client.Write(ByteSpan{more}) == more.size());
    b.Shuttle(START);
    CHECK_FALSE(b.ClientError.has_value());
    CHECK_FALSE(b.AgentError.has_value());
    auto expected = typed;
    expected.insert(expected.end(), more.begin(), more.end());
    CHECK(ends.AtPty == expected);
}

}  // namespace
