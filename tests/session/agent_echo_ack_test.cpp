/// The agent's EchoAck (design.md §9, «Предиктивное эхо»), end to end over a
/// client and an agent connection on virtual time: when it goes, what it
/// says, and what keeps it from starving the Data behind it.
///
/// The harness plays the agent loop's part. A Deliver event is input the pty
/// takes at once (InputWritten at the time of the exchange); a test calls
/// Settle(now) as the loop does on its echo timer (EchoDue, then SettleEcho);
/// output is whatever the test puts into the session.

#include "grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t CEILING = wire::PROTOCOL_VERSION_MAX;

SessionLimits Roomy(EForcePath force = EForcePath::None) {
    return SessionLimits{
        .ResendMax = 16384,
        .DisconnectMax = 16384,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = Duration{0},
                                   .PeerSilenceTimeout = Duration{0}},
        .ForcePath = force,
    };
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

struct Ack {
    std::uint32_t Gen;
    std::uint64_t Out;
    std::uint64_t In;
    friend bool operator==(const Ack&, const Ack&) = default;
};

doctest::String toString(const Ack& a) {
    return doctest::String(("{" + std::to_string(a.Gen) + "," +
                            std::to_string(a.Out) + "," + std::to_string(a.In) +
                            "}")
                               .c_str());
}

/// An agent and a client connection over one pair of sessions, with the
/// events of the client recorded and the agent's Deliver written to the pty
/// at once.
struct EchoLink {
    explicit EchoLink(SessionLimits limits = Roomy(), bool clientEcho = true,
                      std::uint16_t clientCeiling = CEILING,
                      std::uint16_t agentCeiling = CEILING,
                      bool capable = false)
        : R(limits),
          ClientEcho(clientEcho),
          ClientCeiling(clientCeiling),
          AgentCeiling(agentCeiling),
          Capable(capable) {
        Connect(START, capable);
    }

    /// A fresh pair of connections; the old ones, if any, are kept alive.
    void Connect(Timestamp now, bool resize = false) {
        Frames.emplace_back(CONNECTION_FRAME_STORAGE);
        auto& agentFrames = Frames.back();
        Frames.emplace_back(CONNECTION_FRAME_STORAGE);
        auto& clientFrames = Frames.back();
        if (Agent) {
            Retired.push_back(std::move(Agent));
        }
        Agent = std::make_unique<AgentConnection>(MutableByteSpan{agentFrames},
                                                  now, AgentCeiling);
        Client = std::make_unique<ClientConnection>(
            R.Client, MakeNonce(++R.Nonces), MutableByteSpan{clientFrames}, now,
            Capable, ClientCeiling, MutableByteSpan{R.Sp.SnapshotBuf},
            ClientEcho);
        if (resize) {
            Client->QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        }
        Exchange(now);
    }

    /// A reconnect of the same sessions: the old connection just stops.
    void Reconnect(Timestamp now, bool resize = false) {
        Received = ReceivedBase;
        Connect(now, resize);
    }

    bool StepAgent(Timestamp now) {
        auto event = PumpAgent(*Agent, R.Sp, now);
        REQUIRE(event.has_value());
        if (std::holds_alternative<Idle>(*event)) {
            return false;
        }
        if (std::holds_alternative<NeedsSession>(*event)) {
            Agent->Route(R.Agent.Ref(), MakeNonce(++R.Nonces));
        } else if (const auto* sent = std::get_if<SendFrame>(&*event)) {
            REQUIRE(Feed(*Client, R.Sp, sent->Written));
        } else if (const auto* deliver = std::get_if<Deliver>(&*event)) {
            InputSeen += deliver->Bytes.size();
            REQUIRE(R.Agent.InputWritten(deliver->Bytes.size(), now));
        } else if (std::holds_alternative<NeedsSnapshot>(*event)) {
            if (!AutoSnapshot) {
                NeedsSnap = true;
                return false;
            }
            Snapshot.assign(SnapshotSize, std::byte{0x22});
            Agent->SupplySnapshot(now, ByteSpan{Snapshot}, 0, 0);
        }
        return true;
    }

    bool StepClient(Timestamp now) {
        auto event = PumpClient(*Client, R.Sp, now);
        REQUIRE(event.has_value());
        if (std::holds_alternative<Idle>(*event)) {
            return false;
        }
        if (const auto* sent = std::get_if<SendFrame>(&*event)) {
            REQUIRE(Feed(*Agent, R.Sp, sent->Written));
        } else if (const auto* deliver = std::get_if<Deliver>(&*event)) {
            Received += deliver->Bytes.size();
            ReceivedBase = Received;
            Order += 'D';
        } else if (const auto* acked = std::get_if<EchoAcked>(&*event)) {
            Order += 'E';
            Acks.push_back(Ack{acked->Ack.StreamGen, acked->Ack.OutputOffset,
                               acked->Ack.InputOffset});
            AckAt.push_back(now);
            ReceivedAtAck.push_back(Received);
        } else if (const auto* snap = std::get_if<ApplySnapshot>(&*event)) {
            Order += 'S';
            Received = 0;
            ReceivedBase = 0;
            PendingApply = snap->StreamGen;
            if (AutoApply) {
                Client->ConfirmApply(snap->StreamGen);
                PendingApply.reset();
            }
        }
        return true;
    }

    /// Moves frames both ways until neither end has anything to do at `now`.
    void Exchange(Timestamp now) {
        for (int i = 0; i < 100000; ++i) {
            const bool a = StepAgent(now);
            const bool c = StepClient(now);
            if (!a && !c) {
                return;
            }
        }
        FAIL("the exchange does not settle");
    }

    /// The client types `text`; the agent's pty takes it at `now`.
    void Type(Timestamp now, std::string_view text) {
        const auto bytes = Bytes(text);
        REQUIRE(R.Client.Write(bytes) == bytes.size());
        Exchange(now);
    }

    /// The program prints `text`; sent at `now`.
    void Print(Timestamp now, std::string_view text) {
        const auto bytes = Bytes(text);
        REQUIRE(R.Agent.Write(bytes) == bytes.size());
        Exchange(now);
    }

    /// The agent loop's echo timer at `now`.
    void Settle(Timestamp now) {
        if (Agent->EchoDue(now)) {
            Agent->SettleEcho(now);
        }
        Exchange(now);
    }

    void ConfirmApply(Timestamp now) {
        REQUIRE(PendingApply.has_value());
        Client->ConfirmApply(*PendingApply);
        PendingApply.reset();
        Exchange(now);
    }

    Rig R;
    bool ClientEcho;
    std::uint16_t ClientCeiling;
    std::uint16_t AgentCeiling;
    bool Capable;
    std::deque<std::vector<std::byte>> Frames;
    std::unique_ptr<AgentConnection> Agent;
    std::unique_ptr<ClientConnection> Client;
    std::vector<std::unique_ptr<AgentConnection>> Retired;

    bool AutoSnapshot{true};
    bool AutoApply{true};
    bool NeedsSnap{false};
    std::size_t SnapshotSize{16};
    std::vector<std::byte> Snapshot;
    std::optional<std::uint32_t> PendingApply;

    std::string Order;
    std::vector<Ack> Acks;
    std::vector<Timestamp> AckAt;
    std::vector<std::uint64_t> ReceivedAtAck;
    std::uint64_t Received{0};
    std::uint64_t ReceivedBase{0};
    std::uint64_t InputSeen{0};
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "not negotiated: nothing is sent, no deadline") {
    SUBCASE("the client did not offer ECHO_ACK") {
        EchoLink l{Roomy(), false};
        l.Type(START + 1ms, "a");
        l.Print(START + 2ms, "A");
        CHECK_FALSE(l.Agent->EchoDeadline().IsSet());
        l.Agent->SettleEcho(START + 1s);
        l.Exchange(START + 1s);
        CHECK(l.Acks.empty());
    }
    SUBCASE("ver=1") {
        EchoLink l{Roomy(), true, wire::PROTOCOL_VERSION_MIN,
                   wire::PROTOCOL_VERSION_MIN};
        l.Type(START + 1ms, "a");
        CHECK_FALSE(l.Agent->EchoDeadline().IsSet());
        l.Agent->SettleEcho(START + 1s);
        l.Exchange(START + 1s);
        CHECK(l.Acks.empty());
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the first EchoAck goes before the first Data") {
    EchoLink l{Roomy()};
    CHECK(l.Acks == std::vector<Ack>{{0, 0, 0}});

    EchoLink m{Roomy()};
    m.Print(START + 1ms, "hello");
    CHECK(m.Order == "ED");
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output written before the connection: the EchoAck is still "
                  "ahead of it") {
    EchoLink l{Roomy()};
    const auto text = Bytes("hello");
    REQUIRE(l.R.Agent.Write(text) == text.size());
    l.Reconnect(START + 1ms);
    REQUIRE(l.Order.size() >= 2);
    CHECK(l.Order.substr(0, 1) == "E");
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the EchoAck waits ECHO_TIMEOUT after the write") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");

    REQUIRE(l.Agent->EchoDeadline().IsSet());
    CHECK(l.Agent->EchoDeadline().At() == t0 + ECHO_TIMEOUT);
    CHECK_FALSE(l.Agent->EchoDue(t0 + ECHO_TIMEOUT - 1ms));
    CHECK(l.Agent->EchoDue(t0 + ECHO_TIMEOUT));

    // Forced a moment too early: input is not old enough, nothing goes.
    l.Agent->SettleEcho(t0 + ECHO_TIMEOUT - 1ms);
    l.Exchange(t0 + ECHO_TIMEOUT - 1ms);
    CHECK(l.Acks.size() == 1);

    l.Settle(t0 + ECHO_TIMEOUT);
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1] == Ack{0, 0, 1});
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output the pty wrote before the EchoAck is in it, sent "
                  "ahead of it") {
    EchoLink l{Roomy()};
    l.Order.clear();
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Print(t0 + 10ms, "echo");
    l.Print(t0 + 40ms, "more");
    l.Settle(t0 + ECHO_TIMEOUT);
    CHECK(l.Order == "DDE");
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1] == Ack{0, 8, 1});
    CHECK(l.ReceivedAtAck[1] == 8);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the EchoAck covers output written at settle") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    // Written, not yet sent to the client: the settle still counts it, and
    // the Data goes first.
    const auto text = Bytes("xyz");
    REQUIRE(l.R.Agent.Write(text) == text.size());
    l.Order.clear();
    l.Settle(t0 + ECHO_TIMEOUT);
    CHECK(l.Order == "DE");
    CHECK(l.Acks.back() == Ack{0, 3, 1});
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "one EchoAck per firing, spaced by the interval") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Type(t0 + 5ms, "b");
    l.Type(t0 + 10ms, "c");

    l.Settle(t0 + 50ms);
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1] == Ack{0, 0, 1});

    // 55 and 60 ms: the rest is old enough, the interval is not.
    REQUIRE(l.Agent->EchoDeadline().IsSet());
    CHECK(l.Agent->EchoDeadline().At() == t0 + 50ms + ECHO_ACK_INTERVAL_MIN);
    l.Settle(t0 + 60ms);
    CHECK(l.Acks.size() == 2);
    // Settled by hand inside the interval: the same.
    l.Agent->SettleEcho(t0 + 60ms);
    l.Exchange(t0 + 60ms);
    CHECK(l.Acks.size() == 2);

    l.Settle(t0 + 50ms + ECHO_ACK_INTERVAL_MIN);
    REQUIRE(l.Acks.size() == 3);
    CHECK(l.Acks[2] == Ack{0, 0, 3});

    // Everything reported: nothing is due, ever.
    CHECK_FALSE(l.Agent->EchoDeadline().IsSet());
    l.Settle(t0 + 10s);
    CHECK(l.Acks.size() == 3);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a continuous input stream does not starve the Data") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    std::uint64_t printed = 0;
    for (int ms = 0; ms <= 300; ++ms) {
        const Timestamp now = t0 + std::chrono::milliseconds{ms};
        const auto in = Bytes("k");
        REQUIRE(l.R.Client.Write(in) == 1);
        const std::string out(100, 'o');
        const auto bytes = Bytes(out);
        REQUIRE(l.R.Agent.Write(bytes) == bytes.size());
        printed += bytes.size();
        if (l.Agent->EchoDue(now)) {
            l.Agent->SettleEcho(now);
        }
        l.Exchange(now);
        CHECK(l.Received == printed);
    }
    CHECK(l.Received == printed);

    // Spaced, bounded, strictly growing in input, never ahead of the client.
    CHECK(l.Acks.size() <= 300 / 20 + 2);
    CHECK(l.Acks.size() >= 5);
    for (std::size_t i = 1; i < l.Acks.size(); ++i) {
        CHECK(l.AckAt[i] - l.AckAt[i - 1] >= ECHO_ACK_INTERVAL_MIN);
        CHECK(l.Acks[i].In >= l.Acks[i - 1].In);
        CHECK(l.Acks[i].Out >= l.Acks[i - 1].Out);
    }
    for (std::size_t i = 0; i < l.Acks.size(); ++i) {
        CHECK(l.Acks[i].Out <= l.ReceivedAtAck[i]);
    }
    for (std::size_t i = 2; i < l.Acks.size(); ++i) {
        CHECK(l.Acks[i].In > l.Acks[i - 1].In);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a full history delays the EchoAck, never advances it") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    for (std::size_t i = 0; i < ECHO_HISTORY_MAX - 1; ++i) {
        l.Type(t0, "a");
    }
    // Writes 64..70 all fold into the last pair, stamped later.
    for (int i = 0; i < 7; ++i) {
        l.Type(t0 + 30ms, "b");
    }
    l.Settle(t0 + ECHO_TIMEOUT);
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1].In == ECHO_HISTORY_MAX - 1);

    l.Settle(t0 + 30ms + ECHO_TIMEOUT);
    REQUIRE(l.Acks.size() == 3);
    CHECK(l.Acks[2].In == ECHO_HISTORY_MAX + 6);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the EchoAck waits for the Data its output_offset names") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Agent->SetSendGate(false);
    const auto text = Bytes("0123456789");
    REQUIRE(l.R.Agent.Write(text) == text.size());
    l.Order.clear();

    l.Settle(t0 + ECHO_TIMEOUT);
    CHECK(l.Order.empty());
    CHECK(l.Acks.size() == 1);

    l.Agent->SetSendGate(true);
    l.Exchange(t0 + 60ms);
    CHECK(l.Order == "DE");
    CHECK(l.Acks.back() == Ack{0, 10, 1});
    CHECK(l.ReceivedAtAck.back() == 10);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a closed pty read settles and releases nothing") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");

    l.Agent->SetPtyReadOpen(false);
    CHECK_FALSE(l.Agent->EchoDeadline().IsSet());
    CHECK_FALSE(l.Agent->EchoDue(t0 + 1s));
    l.Agent->SettleEcho(t0 + 1s);
    l.Exchange(t0 + 1s);
    CHECK(l.Acks.size() == 1);

    l.Agent->SetPtyReadOpen(true);
    CHECK(l.Agent->EchoDue(t0 + 1s));
    l.Settle(t0 + 1s);
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1].In == 1);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck settled and waiting is not released while the "
                  "pty read is closed") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Agent->SetSendGate(false);
    const auto text = Bytes("zz");
    REQUIRE(l.R.Agent.Write(text) == text.size());
    l.Settle(t0 + ECHO_TIMEOUT);

    l.Agent->SetPtyReadOpen(false);
    l.Agent->SetSendGate(true);
    l.Exchange(t0 + 60ms);
    CHECK(l.Order.back() == 'D');
    CHECK(l.Acks.size() == 1);

    l.Agent->SetPtyReadOpen(true);
    l.Exchange(t0 + 70ms);
    CHECK(l.Acks.size() == 2);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a reconnect starts from the connection's own reference") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Print(t0 + 10ms, "AB");
    l.Settle(t0 + ECHO_TIMEOUT);
    REQUIRE(l.Acks.back() == Ack{0, 2, 1});

    l.Order.clear();
    l.Acks.clear();
    l.Reconnect(t0 + 100ms);
    // The client holds 2 bytes and has had 1 input byte reported.
    REQUIRE(l.Acks.size() == 1);
    CHECK(l.Acks[0] == Ack{0, 2, 1});

    l.Print(t0 + 110ms, "C");
    CHECK(l.Order == "ED");
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "input still unreported at a reconnect is reported later") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Acks.clear();
    l.Reconnect(t0 + 10ms);
    REQUIRE(l.Acks.size() == 1);
    CHECK(l.Acks[0] == Ack{0, 0, 0});

    // The old pair is still in the history; the new connection reports it
    // at its own time, which is at most the old moment plus the timeout.
    REQUIRE(l.Agent->EchoDeadline().IsSet());
    l.Settle(l.Agent->EchoDeadline().At());
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1].In == 1);
}

TEST_CASE_FIXTURE(SodiumFixture, "a superseded connection settles nothing") {
    EchoLink l{Roomy()};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    AgentConnection* old = l.Agent.get();
    l.Reconnect(t0 + 10ms);
    REQUIRE(l.Retired.size() == 1);
    REQUIRE(l.Retired[0].get() == old);
    CHECK_FALSE(old->EchoDeadline().IsSet());
    old->SettleEcho(t0 + 1s);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: the first EchoAck names the new generation") {
    EchoLink l{Roomy(EForcePath::B), true, CEILING, CEILING, true};
    const Timestamp t0 = START + 100ms;
    l.Type(t0, "a");
    l.Print(t0 + 5ms, "text");
    l.Settle(t0 + ECHO_TIMEOUT);
    // ForcePath B: every connection, the first too, opens a generation.
    REQUIRE(l.Acks.back() == Ack{1, 4, 1});

    l.Order.clear();
    l.Acks.clear();
    l.Reconnect(t0 + 100ms, true);
    CHECK(l.Order == "SE");
    REQUIRE(l.Acks.size() == 1);
    CHECK(l.Acks[0] == Ack{2, 0, 1});

    // And the connection then works as any: the next input is reported with
    // output counted in the new generation.
    l.Type(t0 + 110ms, "b");
    l.Print(t0 + 115ms, "xy");
    l.Settle(t0 + 110ms + ECHO_TIMEOUT);
    REQUIRE(l.Acks.size() == 2);
    CHECK(l.Acks[1] == Ack{2, 2, 2});
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: a multi-chunk snapshot, then the EchoAck") {
    EchoLink l{Roomy(EForcePath::B), true, CEILING, CEILING, true};
    l.SnapshotSize = 3000;
    l.Order.clear();
    l.Acks.clear();
    l.Reconnect(START + 100ms, true);
    CHECK(l.Order == "SE");
    REQUIRE(l.Acks.size() == 1);
    CHECK(l.Acks[0].Gen == 2);
    CHECK(l.Acks[0].Out == 0);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "path B: nothing is settled while the snapshot is in flight") {
    EchoLink l{Roomy(EForcePath::B), true, CEILING, CEILING, true};
    l.AutoApply = false;
    const Timestamp t0 = START + 100ms;
    l.Acks.clear();
    l.Reconnect(t0, true);
    REQUIRE(l.PendingApply.has_value());
    const std::size_t before = l.Acks.size();

    l.Type(t0 + 1ms, "a");
    CHECK_FALSE(l.Agent->EchoDeadline().IsSet());
    l.Agent->SettleEcho(t0 + 1s);
    l.Exchange(t0 + 1s);
    CHECK(l.Acks.size() == before);

    l.ConfirmApply(t0 + 1s);
    REQUIRE(l.Agent->EchoDeadline().IsSet());
    l.Settle(t0 + 2s);
    REQUIRE_FALSE(l.Acks.empty());
    CHECK(l.Acks.back().Gen == 2);
    CHECK(l.Acks.back().In == 1);
}
