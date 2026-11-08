/// ECHO_ACK through a whole handshake (§7 TLV registry, §9 ADR-036): what
/// each end ends up with, and the EchoAck an agent queues reaching the client
/// as an event.

#include <doctest/doctest.h>

#include <optional>
#include <variant>

#include "grid_path_helpers.hpp"

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

}  // namespace
