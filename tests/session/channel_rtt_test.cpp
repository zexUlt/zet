/// The channel's round-trip measurement (design.md §9 «RTT», ADR-038): the
/// client times its own Ping against the Pong that echoes the token, smooths
/// the samples per RFC 6298, and adds a Ping to the input it sends when the
/// last sample is old. Nothing of it is on the wire but Ping and Pong.
///
/// A client and an agent connection, live, with the frames between them held
/// by the test so that each travels for exactly the time the test says.

#include "grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using Frame = std::vector<std::byte>;

SessionLimits Limits(Duration keepalive = Duration{0}) {
    return SessionLimits{
        .ResendMax = 4096,
        .DisconnectMax = 4096,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = keepalive,
                                   .PeerSilenceTimeout = Duration{0}},
    };
}

struct RttLink {
    explicit RttLink(Rig& rig, bool echo = true)
        : R(rig),
          Agent{MutableByteSpan{rig.Sp.AgentFrames}, START},
          Client{rig.Client,
                 MakeNonce(++rig.Nonces),
                 MutableByteSpan{rig.Sp.ClientFrames},
                 START,
                 true,
                 wire::PROTOCOL_VERSION_MAX,
                 MutableByteSpan{rig.Sp.SnapshotBuf},
                 echo} {
        REQUIRE(
            DriveToLive(Agent, Client, rig.Agent, rig.Sp, START, rig.Nonces));
    }

    /// Every frame the client has to send at `now`.
    std::vector<Frame> ClientSends(Timestamp now) {
        std::vector<Frame> frames;
        for (int i = 0; i < 64; ++i) {
            auto event = PumpClient(Client, R.Sp, now);
            REQUIRE(event.has_value());
            const auto* sent = std::get_if<SendFrame>(&*event);
            if (sent == nullptr) {
                break;
            }
            frames.emplace_back(R.Sp.Out.begin(),
                                R.Sp.Out.begin() + sent->Written);
        }
        return frames;
    }

    /// Hands frames to the agent at `now` and returns what it sends back.
    std::vector<Frame> AgentTakes(const std::vector<Frame>& frames,
                                  Timestamp now) {
        for (const Frame& frame : frames) {
            REQUIRE(Agent.OnBytes(frame).has_value());
            Drain(now);
        }
        Drain(now);
        return std::exchange(Replies, {});
    }

    void Drain(Timestamp now) {
        for (int i = 0; i < 64; ++i) {
            auto event = PumpAgent(Agent, R.Sp, now);
            REQUIRE(event.has_value());
            if (const auto* sent = std::get_if<SendFrame>(&*event)) {
                Replies.emplace_back(R.Sp.Out.begin(),
                                     R.Sp.Out.begin() + sent->Written);
            } else if (const auto* deliver = std::get_if<Deliver>(&*event)) {
                REQUIRE(R.Agent.InputDelivered(deliver->Bytes.size()));
            } else if (std::holds_alternative<Idle>(*event)) {
                return;
            }
        }
    }

    void ClientTakes(const std::vector<Frame>& frames, Timestamp now) {
        for (const Frame& frame : frames) {
            REQUIRE(Client.OnBytes(frame).has_value());
            auto event = PumpClient(Client, R.Sp, now);
            REQUIRE(event.has_value());
        }
    }

    Rig& R;
    AgentConnection Agent;
    ClientConnection Client;
    std::vector<Frame> Replies;
};

const std::vector<std::byte> ONE_BYTE{std::byte{'k'}};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture, "there is no SRTT before a sample") {
    Rig r{Limits()};
    RttLink link{r};
    CHECK_FALSE(link.Client.Srtt().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the Pong of the client's own Ping gives the first sample "
                  "as it is") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    const auto sent = link.ClientSends(t0);
    REQUIRE(sent.size() >= 2);  // the Ping rides ahead of the input
    const auto replies = link.AgentTakes(sent, t0 + 10ms);
    REQUIRE_FALSE(replies.empty());
    link.ClientTakes(replies, t0 + 20ms);
    CHECK(link.Client.Srtt() == 20ms);
}

TEST_CASE_FIXTURE(SodiumFixture, "the next samples are smoothed by 1/8") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    Timestamp t = START + 10ms;
    link.ClientTakes(link.AgentTakes(link.ClientSends(t), t + 5ms), t + 20ms);
    REQUIRE(link.Client.Srtt() == 20ms);

    // Past RTT_PROBE_INTERVAL, with new input: a second probe, 100 ms.
    t = t + 2s;
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    link.ClientTakes(link.AgentTakes(link.ClientSends(t), t + 5ms), t + 100ms);
    // (7 * 20 + 100) / 8 = 30.
    CHECK(link.Client.Srtt() == 30ms);
}

TEST_CASE_FIXTURE(SodiumFixture, "a sample is at most RTT_SAMPLE_MAX") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    const auto replies = link.AgentTakes(link.ClientSends(t0), t0 + 1ms);
    link.ClientTakes(replies, t0 + RTT_SAMPLE_MAX);
    CHECK(link.Client.Srtt() == RTT_SAMPLE_MAX);
}

TEST_CASE_FIXTURE(SodiumFixture, "a sample over RTT_SAMPLE_MAX is dropped") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    const auto replies = link.AgentTakes(link.ClientSends(t0), t0 + 1ms);
    link.ClientTakes(replies, t0 + RTT_SAMPLE_MAX + 1ns);
    CHECK_FALSE(link.Client.Srtt().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a Pong with a token that is not the outstanding one "
                  "measures nothing") {
    // A keepalive Ping replaces the unanswered probe, so the Pong of the
    // probe arrives for a token the channel has given up.
    Rig r{Limits(100ms)};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    const auto probe = link.ClientSends(t0);
    REQUIRE_FALSE(probe.empty());
    const auto keepalive = link.ClientSends(t0 + 100ms);
    REQUIRE_FALSE(keepalive.empty());

    // The probe's Ping is answered late, after the keepalive went out.
    const auto late = link.AgentTakes(probe, t0 + 110ms);
    REQUIRE_FALSE(late.empty());
    link.ClientTakes(late, t0 + 150ms);
    CHECK_FALSE(link.Client.Srtt().has_value());

    // The keepalive's own Pong is a sample: 160 - 100 = 60 ms.
    const auto answer = link.AgentTakes(keepalive, t0 + 130ms);
    REQUIRE_FALSE(answer.empty());
    link.ClientTakes(answer, t0 + 160ms);
    CHECK(link.Client.Srtt() == 60ms);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a probe Ping goes with input no more often than "
                  "RTT_PROBE_INTERVAL") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    link.ClientTakes(link.AgentTakes(link.ClientSends(t0), t0), t0 + 10ms);
    REQUIRE(link.Client.Srtt() == 10ms);

    // Just short of the interval: the input goes alone.
    Timestamp t = t0 + 10ms + RTT_PROBE_INTERVAL - 1ns;
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    auto alone = link.ClientSends(t);
    REQUIRE(alone.size() == 1);
    link.ClientTakes(link.AgentTakes(alone, t), t + 40ms);
    CHECK(link.Client.Srtt() == 10ms);

    // At the interval: a Ping rides ahead of the input.
    t = t0 + 10ms + RTT_PROBE_INTERVAL;
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    auto probed = link.ClientSends(t);
    REQUIRE(probed.size() == 2);
    link.ClientTakes(link.AgentTakes(probed, t), t + 30ms);
    CHECK(link.Client.Srtt() == 12500us);
}

TEST_CASE_FIXTURE(SodiumFixture, "one probe at a time while it is unanswered") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);

    const Timestamp t0 = START + 10ms;
    const auto first = link.ClientSends(t0);
    REQUIRE(first.size() == 2);

    // More input a moment later, before any Pong: no second Ping.
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    const auto second = link.ClientSends(t0 + 50ms);
    CHECK(second.size() == 1);

    // A probe nobody answered in RTT_SAMPLE_MAX does not hold the next back.
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    const auto third = link.ClientSends(t0 + RTT_SAMPLE_MAX + 1ms);
    CHECK(third.size() == 2);
}

TEST_CASE_FIXTURE(SodiumFixture, "no Ping rides with input that is not sent") {
    Rig r{Limits()};
    RttLink link{r};
    CHECK(link.ClientSends(START + 10ms).empty());
}

TEST_CASE_FIXTURE(SodiumFixture, "the agent end does not probe") {
    Rig r{Limits()};
    RttLink link{r};
    REQUIRE(r.Agent.Write(ONE_BYTE) == 1);
    auto event = PumpAgent(link.Agent, r.Sp, START + 10ms);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<SendFrame>(*event));
    // The one frame is the Data; the next Pump has nothing, no Ping.
    auto next = PumpAgent(link.Agent, r.Sp, START + 10ms);
    REQUIRE(next.has_value());
    CHECK(std::holds_alternative<Idle>(*next));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client that did not offer ECHO_ACK sends no probe") {
    Rig r{Limits()};
    RttLink link{r, false};
    REQUIRE(r.Client.Write(ONE_BYTE) == 1);
    CHECK(link.ClientSends(START + 10ms).size() == 1);
}
