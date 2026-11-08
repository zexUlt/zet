/// ECHO_ACK through a whole handshake (§7 TLV registry, §9 ADR-036): what
/// each end ends up with, and the EchoAck an agent queues reaching the client
/// as an event.

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <variant>
#include <vector>

#include "grid_path_helpers.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/frame.hpp"

namespace {

/// Both ends of one connection, live. The client offers ECHO_ACK as told and
/// each end has its own version ceiling.
struct Link {
    Link(bool clientEchoAck, std::uint16_t clientCeiling,
         std::uint16_t agentCeiling)
        : Agent{MutableByteSpan{R.Sp.AgentFrames}, START, agentCeiling},
          Client{R.Client,
                 MakeNonce(++R.Nonces),
                 MutableByteSpan{R.Sp.ClientFrames},
                 START,
                 true,
                 clientCeiling,
                 MutableByteSpan{R.Sp.SnapshotBuf},
                 clientEchoAck} {
        REQUIRE(DriveToLive(Agent, Client, R.Agent, R.Sp, START, R.Nonces));
    }

    /// The agent's next frame to the client, delivered; what the client made
    /// of it.
    ConnectionResult Deliver() {
        auto sent = PumpAgent(Agent, R.Sp, START);
        REQUIRE(sent.has_value());
        const auto* frame = std::get_if<SendFrame>(&*sent);
        REQUIRE(frame != nullptr);
        REQUIRE(Feed(Client, R.Sp, frame->Written));
        return PumpClient(Client, R.Sp, START);
    }

    Rig R;
    AgentConnection Agent;
    ClientConnection Client;
};

TEST_CASE_FIXTURE(SodiumFixture, "ECHO_ACK in Hello is negotiated at ver=2") {
    Link link{true, wire::PROTOCOL_VERSION_MAX, wire::PROTOCOL_VERSION_MAX};
    CHECK(link.Agent.EchoAckNegotiated());
}

TEST_CASE_FIXTURE(SodiumFixture, "without ECHO_ACK nothing is negotiated") {
    Link link{false, wire::PROTOCOL_VERSION_MAX, wire::PROTOCOL_VERSION_MAX};
    CHECK_FALSE(link.Agent.EchoAckNegotiated());
    CHECK_FALSE(link.Agent.QueueEchoAck(wire::EchoAckMessage{}));
}

TEST_CASE_FIXTURE(SodiumFixture, "ver=1 from either end drops ECHO_ACK") {
    // The client's ceiling keeps the TLV out of Hello altogether.
    Link clientCapped{true, wire::PROTOCOL_VERSION_MIN,
                      wire::PROTOCOL_VERSION_MAX};
    CHECK_FALSE(clientCapped.Agent.EchoAckNegotiated());

    // The agent's ceiling sees the TLV and lets it go: one version, ver=1.
    Link agentCapped{true, wire::PROTOCOL_VERSION_MAX,
                     wire::PROTOCOL_VERSION_MIN};
    CHECK_FALSE(agentCapped.Agent.EchoAckNegotiated());
    CHECK_FALSE(agentCapped.Agent.QueueEchoAck(wire::EchoAckMessage{}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck from the agent reaches the client") {
    Link link{true, wire::PROTOCOL_VERSION_MAX, wire::PROTOCOL_VERSION_MAX};
    const wire::EchoAckMessage ack{
        .StreamGen = 0, .OutputOffset = 0, .InputOffset = 0};
    REQUIRE(link.Agent.QueueEchoAck(ack));

    const auto event = link.Deliver();
    REQUIRE(event.has_value());
    const auto* acked = std::get_if<EchoAcked>(&*event);
    REQUIRE(acked != nullptr);
    CHECK(acked->Ack == ack);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "an EchoAck beyond the client's stream closes the connection") {
    Link link{true, wire::PROTOCOL_VERSION_MAX, wire::PROTOCOL_VERSION_MAX};
    // The client has written no input, so an input_offset of 1 is a lie.
    REQUIRE(link.Agent.QueueEchoAck(wire::EchoAckMessage{.InputOffset = 1}));

    auto sent = PumpAgent(link.Agent, link.R.Sp, START);
    REQUIRE(sent.has_value());
    const auto* frame = std::get_if<SendFrame>(&*sent);
    REQUIRE(frame != nullptr);
    REQUIRE(Feed(link.Client, link.R.Sp, frame->Written));

    const auto event = PumpClient(link.Client, link.R.Sp, START);
    REQUIRE_FALSE(event.has_value());
    CHECK(event.error().Disposition() == EDisposition::CloseConnection);
}

/// An agent fed a bare Hello carrying one TLV of `type` and `value`, pumped
/// to the point where it asks for the session: all that the pre-auth read of
/// the TLVs needs.
struct HelloProbe {
    HelloProbe(std::uint16_t type, std::span<const std::byte> value)
        : Frames(CONNECTION_FRAME_STORAGE),
          Out(CONNECTION_OUT_SIZE),
          Scratch(CONNECTION_SCRATCH_SIZE),
          Agent{MutableByteSpan{Frames}, START} {
        wire::HelloMessage hello{};
        hello.VersionMin = wire::PROTOCOL_VERSION_MIN;
        hello.VersionMax = wire::PROTOCOL_VERSION_MAX;
        hello.ClientNonce = MakeNonce(1);
        REQUIRE(hello.Options.Append(
            wire::TlvOption{.Type = type, .Value = value}));
        std::vector<std::byte> frame(wire::HEADER_SIZE +
                                     wire::MAX_HANDSHAKE_FRAME);
        ByteWriter body{MutableByteSpan{frame}.subspan(wire::HEADER_SIZE)};
        REQUIRE(wire::WriteHello(body, hello));
        ByteWriter header{MutableByteSpan{frame}.first(wire::HEADER_SIZE)};
        REQUIRE(wire::WriteFrameHeader(
            header,
            wire::FrameHeader{.Length = static_cast<std::uint32_t>(body.Size()),
                              .Epoch = 0}));
        REQUIRE(Agent.OnBytes(
            ByteSpan{frame}.first(wire::HEADER_SIZE + body.Size())));
        const auto need =
            Agent.Pump(START, MutableByteSpan{Out}, MutableByteSpan{Scratch});
        REQUIRE(need.has_value());
        REQUIRE(std::holds_alternative<NeedsSession>(*need));
    }

    std::vector<std::byte> Frames;
    std::vector<std::byte> Out;
    std::vector<std::byte> Scratch;
    AgentConnection Agent;
};

TEST_CASE_FIXTURE(SodiumFixture,
                  "an ECHO_ACK or GRID_CAPABLE TLV is read only as u16be 1") {
    const std::array<std::byte, 2> one{std::byte{0}, std::byte{1}};
    const std::array<std::byte, 2> zero{std::byte{0}, std::byte{0}};
    const std::array<std::byte, 2> two{std::byte{0}, std::byte{2}};
    const std::array<std::byte, 2> swapped{std::byte{1}, std::byte{0}};
    const std::array<std::byte, 1> shortValue{std::byte{1}};

    {
        HelloProbe probe{wire::TLV_ECHO_ACK, one};
        CHECK(probe.Agent.HelloOffersEchoAck());
        CHECK_FALSE(probe.Agent.HelloOffersGridCapable());
    }
    {
        HelloProbe probe{wire::TLV_GRID_CAPABLE, one};
        CHECK(probe.Agent.HelloOffersGridCapable());
        CHECK_FALSE(probe.Agent.HelloOffersEchoAck());
    }
    for (const auto type : {wire::TLV_ECHO_ACK, wire::TLV_GRID_CAPABLE}) {
        for (const std::span<const std::byte> value :
             {std::span<const std::byte>{zero}, std::span<const std::byte>{two},
              std::span<const std::byte>{swapped},
              std::span<const std::byte>{shortValue}}) {
            CAPTURE(type);
            CAPTURE(value.size());
            HelloProbe probe{type, value};
            CHECK_FALSE(probe.Agent.HelloOffersEchoAck());
            CHECK_FALSE(probe.Agent.HelloOffersGridCapable());
        }
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "ECHO_ACK is negotiated on path B too") {
    Rig r{TinyLimits()};
    {
        AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START};
        ClientConnection cc{r.Client,
                            MakeNonce(++r.Nonces),
                            MutableByteSpan{r.Sp.ClientFrames},
                            START,
                            true,
                            wire::PROTOCOL_VERSION_MAX,
                            MutableByteSpan{r.Sp.SnapshotBuf},
                            true};
        REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
        ac.Close(START);
    }
    // More output than the ring keeps, so the reconnect has to take path B.
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }

    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        true,
                        wire::PROTOCOL_VERSION_MAX,
                        MutableByteSpan{r.Sp.SnapshotBuf},
                        true};
    // The agent waits for the post-Resume Resize before asking for a snapshot.
    cc.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
    bool asked = false;
    for (int i = 0; i < 60 && !asked; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            asked = true;
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
    }
    REQUIRE(asked);

    CHECK(ac.EchoAckNegotiated());
    const std::vector<std::byte> snapshot(16, std::byte{0x22});
    ac.SupplySnapshot(START, ByteSpan{snapshot}, 0, 0);
    CHECK(ac.EchoAckNegotiated());
}

}  // namespace
