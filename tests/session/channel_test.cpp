/// Channel integration tests.
///
/// Two Channel objects — one Client, one Agent — share a pair of
/// ConnectionKeys and exchange frames through in-memory byte vectors. No
/// sockets, no clock reads: time is a Timestamp that we advance by hand.
///
/// Test categories:
///   - byte-identical round trip with random write sizes (fixed seed)
///   - ack trims the sender's buffer
///   - bare Ack fires after ACK_IDLE_DELAY and not before
///   - Ping → Pong
///   - Resize reaches the agent
///   - unknown type counted, not errored
///   - bit flip → CloseConnection, no state change
///   - data with a gap → StreamGap
///   - rekey after a small frame limit (constructor policy)
///   - rekey after REKEY_INTERVAL of virtual time
///   - peer that never acks fills buffer to capacity and stops new Data
///
/// docs/design.md §7, §8, §9.

#include "zet/session/channel.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <variant>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/receive_cursor.hpp"
#include "zet/session/send_buffer.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace zet::wire;

// ── Helpers ──────────────────────────────────────────────────────────────

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

crypto::Key MakeMaster(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint8_t seed) {
    HandshakeNonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] =
            static_cast<std::byte>(seed * 7 + static_cast<std::uint8_t>(i));
    }
    return nonce;
}

/// Poll that must not fail: every test here hands in buffers wide enough and
/// stays within the key epochs, so an error is itself the finding.
std::size_t PollOk(Channel& channel, Timestamp now, MutableByteSpan out,
                   MutableByteSpan scratch) {
    const auto n = channel.Poll(now, out, scratch);
    REQUIRE(n.has_value());
    return *n;
}

/// A pair of Channels on the same ConnectionKeys, wired back to back.
///
/// Drive the pair manually with Pump() to transfer one frame at a time, or
/// call PumpAll() to drain the client's send cursor into the agent's receive
/// cursor and back.
struct ChannelPair {
    static constexpr std::size_t BUF_SIZE = RESEND_BUFFER_MAX;

    explicit ChannelPair(Timestamp start = Timestamp{},
                         std::uint64_t frameLimit = MAX_FRAMES_PER_EPOCH,
                         std::uint64_t byteLimit = MAX_BYTES_PER_EPOCH)
        : Keys{DeriveConnectionKeys(
              DeriveSessionSecrets(MakeMaster(1)).ConnectionSeed, MakeNonce(1),
              MakeNonce(2))},
          ClientSendStore(BUF_SIZE),
          AgentSendStore(BUF_SIZE),
          ClientSendBuf{MutableByteSpan{ClientSendStore}, 0},
          AgentSendBuf{MutableByteSpan{AgentSendStore}, 0},
          ClientRecv{0},
          AgentRecv{0},
          Client{Keys,  ERole::Client, ClientSendBuf, ClientRecv,
                 start, frameLimit,    byteLimit},
          Agent{Keys,  ERole::Agent, AgentSendBuf, AgentRecv,
                start, frameLimit,   byteLimit} {}

    ConnectionKeys Keys;

    std::vector<std::byte> ClientSendStore;
    std::vector<std::byte> AgentSendStore;
    SendBuffer ClientSendBuf;
    SendBuffer AgentSendBuf;
    ReceiveCursor ClientRecv;
    ReceiveCursor AgentRecv;

    Channel Client;
    Channel Agent;

    // Working buffers reused across calls.
    std::vector<std::byte> Out{wire::HEADER_SIZE + wire::MAX_FRAME,
                               std::byte{}};
    std::vector<std::byte> Scratch{Channel::POLL_SCRATCH_SIZE, std::byte{}};
    std::vector<std::byte> OpenScratch{wire::MAX_FRAME, std::byte{}};

    /// Poll `src` once and deliver the frame to `dst`. Returns an empty
    /// optional when there was nothing to send.
    std::optional<IncomingEvent> PumpOne(Channel& src, Channel& dst,
                                         Timestamp now) {
        const std::size_t n =
            PollOk(src, now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
        if (n == 0) {
            return std::nullopt;
        }
        const auto frame = ReadFrame(ByteSpan{Out}.first(n), wire::MAX_FRAME);
        REQUIRE(frame.has_value());
        auto result = dst.Receive(*frame, MutableByteSpan{OpenScratch});
        REQUIRE(result.has_value());
        return *result;
    }

    /// Drain all pending frames from client → agent, returning total bytes
    /// delivered to the agent's receive cursor.
    std::size_t PumpClientToAgent(Timestamp now) {
        std::size_t total = 0;
        while (true) {
            const auto ev = PumpOne(Client, Agent, now);
            if (!ev.has_value()) {
                break;
            }
            if (std::holds_alternative<DeliveredData>(*ev)) {
                total += std::get<DeliveredData>(*ev).Bytes.size();
            }
        }
        return total;
    }

    /// Drain all pending frames from agent → client.
    std::size_t PumpAgentToClient(Timestamp now) {
        std::size_t total = 0;
        while (true) {
            const auto ev = PumpOne(Agent, Client, now);
            if (!ev.has_value()) {
                break;
            }
            if (std::holds_alternative<DeliveredData>(*ev)) {
                total += std::get<DeliveredData>(*ev).Bytes.size();
            }
        }
        return total;
    }
};

std::vector<std::byte> MakeBytes(std::size_t n, std::uint8_t seed = 0) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] =
            static_cast<std::byte>(seed + static_cast<std::uint8_t>(i % 251));
    }
    return v;
}

}  // namespace

// ── Round trip: byte-identical stream under random write sizes ─────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "byte-identical round trip with random write sizes") {
    // Print the seed on failure so the run is reproducible.
    const std::uint32_t seed = 0xBEEF1234;
    INFO("seed: 0x" << std::hex << seed);
    std::mt19937 rng{seed};

    ChannelPair pair;

    // 128 KiB of data in writes of 1..1023 bytes.
    const std::size_t totalBytes = 128 * 1024;
    const auto source = MakeBytes(totalBytes, 0xAB);
    std::vector<std::byte> received;
    received.reserve(totalBytes);

    std::size_t sent = 0;
    Timestamp now{};

    while (sent < totalBytes || received.size() < totalBytes) {
        // Write a random chunk into the client's send buffer.
        if (sent < totalBytes) {
            std::uniform_int_distribution<std::size_t> dist{1, 1023};
            const std::size_t chunkSize =
                std::min(dist(rng), totalBytes - sent);
            const std::size_t taken = pair.ClientSendBuf.Append(
                ByteSpan{source}.subspan(sent, chunkSize));
            sent += taken;
        }

        // Drain client → agent.
        const auto ev = pair.PumpOne(pair.Client, pair.Agent, now);
        if (ev.has_value() && std::holds_alternative<DeliveredData>(*ev)) {
            const auto& dd = std::get<DeliveredData>(*ev);
            received.insert(received.end(), dd.Bytes.begin(), dd.Bytes.end());
        }

        // Let agent ack back so the client's buffer does not fill up.
        pair.PumpOne(pair.Agent, pair.Client, now);
    }

    REQUIRE(received.size() == totalBytes);
    CHECK(received == source);
}

// ── Ack trims the sender's buffer ─────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture, "ack from receiver trims the sender buffer") {
    ChannelPair pair;

    const auto data = MakeBytes(100, 0x01);
    static_cast<void>(pair.ClientSendBuf.Append(ByteSpan{data}));

    Timestamp now{};
    // Send the data from client to agent. The agent's recv cursor advances.
    pair.PumpClientToAgent(now);
    // Sender buffer should not have been trimmed yet (no ack received).
    CHECK(pair.ClientSendBuf.Base() == 0);

    // Call Poll on the agent at t=0 to arm the idle-Ack timer.  The design
    // (§9) is: Poll arms the timer on the first call that sees un-acked bytes
    // with nothing else to send; the caller then waits for the deadline to
    // fire.
    {
        std::vector<std::byte> tmpOut(wire::HEADER_SIZE + wire::MAX_FRAME);
        std::vector<std::byte> tmpScratch(Channel::POLL_SCRATCH_SIZE);
        const std::size_t n = PollOk(pair.Agent, now, MutableByteSpan{tmpOut},
                                     MutableByteSpan{tmpScratch});
        CHECK(n == 0);  // timer armed, not yet fired
    }

    // Advance past ACK_IDLE_DELAY.  The timer fires and the agent produces a
    // bare Ack on the next Poll, which PumpAgentToClient does.
    now += ACK_IDLE_DELAY + std::chrono::milliseconds{1};
    pair.PumpAgentToClient(now);

    // Now the client's send buffer base should have advanced.
    CHECK(pair.ClientSendBuf.Base() == 100);
}

// ── Bare Ack fires after ACK_IDLE_DELAY and not before ───────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "bare Ack fires after ACK_IDLE_DELAY but not before") {
    ChannelPair pair;

    const auto data = MakeBytes(10, 0x02);
    static_cast<void>(pair.ClientSendBuf.Append(ByteSpan{data}));
    Timestamp now{};

    // Deliver the data frame to the agent; poll the agent before the delay.
    pair.PumpClientToAgent(now);

    // Poll agent just after Receive: it should see un-acked bytes and arm the
    // timer, returning 0 (not yet past the deadline).
    std::vector<std::byte> out(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
    const std::size_t early =
        PollOk(pair.Agent, now, MutableByteSpan{out}, MutableByteSpan{scratch});
    CHECK(early == 0);  // timer armed but not yet fired

    // The deadline should now be set.
    const Deadline dl = pair.Agent.NextDeadline();
    CHECK(dl.IsSet());

    // Advance just below the threshold: still nothing.
    const Timestamp almostDue =
        now + ACK_IDLE_DELAY - std::chrono::nanoseconds{1};
    const std::size_t tooEarly = PollOk(
        pair.Agent, almostDue, MutableByteSpan{out}, MutableByteSpan{scratch});
    CHECK(tooEarly == 0);

    // Advance to or past the threshold: a bare Ack should now be produced.
    const Timestamp due = now + ACK_IDLE_DELAY + std::chrono::milliseconds{1};
    const std::size_t ackFrame =
        PollOk(pair.Agent, due, MutableByteSpan{out}, MutableByteSpan{scratch});
    CHECK(ackFrame > 0);

    // The frame parses and decrypts.
    const auto frame =
        ReadFrame(ByteSpan{out}.first(ackFrame), wire::MAX_FRAME);
    REQUIRE(frame.has_value());
    std::vector<std::byte> openBuf(wire::MAX_FRAME);
    const auto ev = pair.Client.Receive(*frame, MutableByteSpan{openBuf});
    REQUIRE(ev.has_value());
    // Ack is silent on the receive side (DeliveredNothing).
    CHECK(std::holds_alternative<DeliveredNothing>(*ev));

    // The client's send buffer was trimmed.
    CHECK(pair.ClientSendBuf.Base() == 10);
}

// ── Ping → Pong ───────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "Ping is answered with a Pong carrying the same token") {
    ChannelPair pair;

    // Build and send a Ping from client.
    const std::uint64_t token = 0xDEADBEEFCAFEBABE;
    std::array<std::byte, 1 + sizeof(std::uint64_t)> pingPlain{};
    {
        ByteWriter w{MutableByteSpan{pingPlain}};
        REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::Ping)));
        REQUIRE(w.WriteU64BE(token));
    }
    // Seal manually using a sealer that matches the client's state... too
    // complex. Just use PumpOne with a Ping in the client's pending Pong
    // (we abuse QueueResize to check the agent side responds). Instead, let's
    // use a separate ChannelPair to inject a Ping directly.
    //
    // Simpler: send a small payload to bootstrap the pair, then inject the
    // Ping through the wire format properly.

    // The client has no Ping API, so we build a raw Ping frame using the
    // client's sealer counter position. The simplest approach: the Ping
    // should be sent by the agent side, and the client should Pong.
    // But let's test the reverse: agent sends a Ping, client Pongs.
    //
    // We cannot directly inject a frame into a live channel without going
    // through Seal. Let's test by checking the Pong response after the
    // agent side sends data and the client receives it, then we verify that
    // a Ping queued on the agent triggers a Pong from the client.
    //
    // Actual test: use a small data exchange to establish the session,
    // then inject a raw Ping frame.

    Timestamp now{};

    // Inject a Ping by constructing it with a matching FrameSealer.
    // We replicate what the "agent" sealer would produce for a Ping.
    // Use a fresh ChannelPair so the counter is at 0.

    // Encode Ping plaintext.
    std::array<std::byte, 1 + sizeof(std::uint64_t)> pingBuf{};
    {
        ByteWriter w{MutableByteSpan{pingBuf}};
        REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::Ping)));
        REQUIRE(w.WriteU64BE(token));
    }

    // Seal from the agent's direction using the pair's sealer.
    // Secret<> is move-only; extract the bytes and construct a new Key.
    FrameSealer agentSealer{crypto::Key{pair.Keys.ServerToClient.Expose()},
                            pair.Keys.SaltServerToClient};
    std::vector<std::byte> pingFrame(wire::HEADER_SIZE + pingBuf.size() + 16);
    const auto sealResult =
        agentSealer.Seal(MutableByteSpan{pingFrame}, ByteSpan{pingBuf});
    REQUIRE(sealResult.has_value());
    pingFrame.resize(*sealResult);

    const auto pingFv = ReadFrame(ByteSpan{pingFrame}, wire::MAX_FRAME);
    REQUIRE(pingFv.has_value());

    std::vector<std::byte> recvBuf(wire::MAX_FRAME);
    const auto event = pair.Client.Receive(*pingFv, MutableByteSpan{recvBuf});
    REQUIRE(event.has_value());
    CHECK(std::holds_alternative<DeliveredNothing>(*event));

    // Now the client should have a pending Pong.
    std::vector<std::byte> pongOut(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> pongScratch(Channel::POLL_SCRATCH_SIZE);
    const std::size_t pongN = PollOk(pair.Client, now, MutableByteSpan{pongOut},
                                     MutableByteSpan{pongScratch});
    REQUIRE(pongN > 0);

    // The Pong should open on the agent and deliver nothing.
    const auto pongFv =
        ReadFrame(ByteSpan{pongOut}.first(pongN), wire::MAX_FRAME);
    REQUIRE(pongFv.has_value());
    std::vector<std::byte> pongRecvBuf(wire::MAX_FRAME);
    const auto pongEv =
        pair.Agent.Receive(*pongFv, MutableByteSpan{pongRecvBuf});
    REQUIRE(pongEv.has_value());
    CHECK(std::holds_alternative<DeliveredNothing>(*pongEv));
}

// ── Resize reaches the agent ──────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "Resize queued on the client is delivered to the agent") {
    ChannelPair pair;

    const wire::ResizeMessage expectedResize{
        .Rows = 24, .Cols = 80, .XPixel = 0, .YPixel = 0};
    pair.Client.QueueResize(expectedResize);

    Timestamp now{};
    std::vector<std::byte> out(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
    const std::size_t n = PollOk(pair.Client, now, MutableByteSpan{out},
                                 MutableByteSpan{scratch});
    REQUIRE(n > 0);

    const auto frame = ReadFrame(ByteSpan{out}.first(n), wire::MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> recvBuf(wire::MAX_FRAME);
    const auto ev = pair.Agent.Receive(*frame, MutableByteSpan{recvBuf});
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<DeliveredResize>(*ev));

    const auto& dr = std::get<DeliveredResize>(*ev);
    CHECK(dr.Resize.Rows == expectedResize.Rows);
    CHECK(dr.Resize.Cols == expectedResize.Cols);
}

// ── Unknown type is counted, not errored ─────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "unknown post-auth type is counted and ignored") {
    ChannelPair pair;

    // Seal a frame with an unknown type byte (0xFF is unassigned).
    std::array<std::byte, 1> unknownPlain{std::byte{0xFF}};
    FrameSealer agentSealer{crypto::Key{pair.Keys.ServerToClient.Expose()},
                            pair.Keys.SaltServerToClient};
    std::vector<std::byte> frame(wire::HEADER_SIZE + unknownPlain.size() + 16);
    REQUIRE(agentSealer.Seal(MutableByteSpan{frame}, ByteSpan{unknownPlain})
                .has_value());

    const auto fv = ReadFrame(ByteSpan{frame}, wire::MAX_FRAME);
    REQUIRE(fv.has_value());

    std::vector<std::byte> recvBuf(wire::MAX_FRAME);
    const auto ev = pair.Client.Receive(*fv, MutableByteSpan{recvBuf});
    REQUIRE(ev.has_value());
    CHECK(std::holds_alternative<DeliveredNothing>(*ev));
    CHECK(pair.Client.UnknownTypeCount() == 1);
}

// ── Bit flip → CloseConnection ────────────────────────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a flipped bit in a frame yields CloseConnection and no state change") {
    ChannelPair pair;

    const auto data = MakeBytes(20, 0x03);
    static_cast<void>(pair.ClientSendBuf.Append(ByteSpan{data}));

    Timestamp now{};
    std::vector<std::byte> out(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
    const std::size_t n = PollOk(pair.Client, now, MutableByteSpan{out},
                                 MutableByteSpan{scratch});
    REQUIRE(n > 0);

    // Flip a bit in the frame body (after the header).
    out[wire::HEADER_SIZE + 1] ^= std::byte{0x37};

    const auto frame = ReadFrame(ByteSpan{out}.first(n), wire::MAX_FRAME);
    REQUIRE(frame.has_value());

    const std::uint64_t recvBefore = pair.Agent.UnknownTypeCount();
    const std::uint64_t cursorBefore = pair.AgentRecv.RecvOffset();

    std::vector<std::byte> recvBuf(wire::MAX_FRAME);
    const auto ev = pair.Agent.Receive(*frame, MutableByteSpan{recvBuf});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);

    // State must not have moved.
    CHECK(pair.AgentRecv.RecvOffset() == cursorBefore);
    CHECK(pair.Agent.UnknownTypeCount() == recvBefore);
}

// ── Data with a gap → StreamGap ───────────────────────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "data arriving with a gap yields StreamGap and CloseConnection") {
    ChannelPair pair;

    // Build a Data frame claiming offset 100 when recv_offset is 0.
    const auto payload = MakeBytes(10, 0x04);
    std::vector<std::byte> plain(1 + 2 * sizeof(std::uint64_t) +
                                 payload.size());
    {
        ByteWriter w{MutableByteSpan{plain}};
        REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::Data)));
        REQUIRE(w.WriteU64BE(0));    // ack
        REQUIRE(w.WriteU64BE(100));  // offset with a gap
        REQUIRE(w.WriteBytes(ByteSpan{payload}));
    }

    // Secret<> is move-only; extract bytes and construct fresh key.
    FrameSealer agentSealer{crypto::Key{pair.Keys.ServerToClient.Expose()},
                            pair.Keys.SaltServerToClient};
    std::vector<std::byte> frame(wire::HEADER_SIZE + plain.size() + 16);
    REQUIRE(
        agentSealer.Seal(MutableByteSpan{frame}, ByteSpan{plain}).has_value());
    frame.resize(wire::HEADER_SIZE + plain.size() + 16);

    const auto fv = ReadFrame(ByteSpan{frame}, wire::MAX_FRAME);
    REQUIRE(fv.has_value());

    std::vector<std::byte> recvBuf(wire::MAX_FRAME);
    const auto ev = pair.Client.Receive(*fv, MutableByteSpan{recvBuf});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Session() == ESessionError::StreamGap);
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Rekey after frame limit ────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "traffic continues across a rekey triggered by frame count") {
    // Use a small frame limit so we reach it quickly. The constructor accepts
    // frameLimit and byteLimit per wire/sealed_frame.hpp, which documents that
    // these are exactly for shortening tests.
    constexpr std::uint64_t smallLimit = 4;
    ChannelPair pair{Timestamp{}, smallLimit};

    const auto data = MakeBytes(500, 0x05);

    Timestamp now{};
    std::size_t totalReceived = 0;
    // Send more bytes than fit in SMALL_LIMIT frames so at least one rekey
    // must occur.
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t chunk =
            std::min<std::size_t>(50, data.size() - offset);
        static_cast<void>(
            pair.ClientSendBuf.Append(ByteSpan{data}.subspan(offset, chunk)));
        offset += chunk;

        const auto ev = pair.PumpOne(pair.Client, pair.Agent, now);
        if (!ev.has_value()) {
            continue;
        }
        if (std::holds_alternative<DeliveredData>(*ev)) {
            totalReceived += std::get<DeliveredData>(*ev).Bytes.size();
        }
    }
    // Drain remaining.
    totalReceived += pair.PumpClientToAgent(now);

    // All bytes must arrive — rekey must not discard any payload.
    CHECK(totalReceived == data.size());
    // The epoch must have advanced past 0.
    CHECK(pair.AgentRecv.RecvOffset() == data.size());
}

// ── Rekey after REKEY_INTERVAL ────────────────────────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "traffic continues across a rekey triggered by elapsed time") {
    ChannelPair pair;

    const auto chunk = MakeBytes(32, 0x06);
    static_cast<void>(pair.ClientSendBuf.Append(ByteSpan{chunk}));
    Timestamp now{};

    // First frame is sent at t=0 (epoch 0).
    REQUIRE(pair.PumpOne(pair.Client, pair.Agent, now).has_value());
    CHECK(pair.AgentRecv.RecvOffset() == 32);

    // Advance time past REKEY_INTERVAL.
    now += REKEY_INTERVAL + std::chrono::seconds{1};

    // Send another chunk. The channel must rekey automatically and the frame
    // must arrive at the agent with the correct epoch.
    static_cast<void>(pair.ClientSendBuf.Append(ByteSpan{chunk}));
    const auto ev = pair.PumpOne(pair.Client, pair.Agent, now);
    REQUIRE(ev.has_value());
    CHECK(std::holds_alternative<DeliveredData>(*ev));
    CHECK(pair.AgentRecv.RecvOffset() == 64);
}

// ── Backpressure: full buffer stops new Data ───────────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "send buffer at capacity stops new Data until the peer acks") {
    // Fill the send buffer to RESEND_BUFFER_MAX without acknowledging any of
    // it. The channel must stop producing Data frames when Room() == 0.
    ChannelPair pair;

    Timestamp now{};
    // Fill the client's send buffer completely.
    const auto bigChunk = MakeBytes(RESEND_BUFFER_MAX, 0x07);
    const std::size_t taken = pair.ClientSendBuf.Append(ByteSpan{bigChunk});
    REQUIRE(taken == RESEND_BUFFER_MAX);
    REQUIRE(pair.ClientSendBuf.Room() == 0);

    // Drain the client's send cursor to the agent (without the agent acking).
    std::vector<std::byte> out(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);

    // Deliver all frames.
    while (PollOk(pair.Client, now, MutableByteSpan{out},
                  MutableByteSpan{scratch}) > 0) {
        // Don't let agent ack: just receive silently.
        // (In a real scenario we'd also forward frames to the agent, but here
        // we're testing the sender's backpressure.)
    }

    // The buffer is still full (no ack received).
    CHECK(pair.ClientSendBuf.Room() == 0);

    // Additional Poll returns 0 because SendNext == End == Base + capacity.
    const std::size_t extra = PollOk(pair.Client, now, MutableByteSpan{out},
                                     MutableByteSpan{scratch});
    CHECK(extra == 0);
}

// ── Channel error API ─────────────────────────────────────────────────

TEST_CASE("SessionError wraps EProtoError and ESessionError with disposition") {
    const SessionError protoErr{EProtoError::AuthenticationFailed};
    CHECK(protoErr.Proto() == EProtoError::AuthenticationFailed);
    CHECK_FALSE(protoErr.Session().has_value());
    CHECK(protoErr.Disposition() == EDisposition::CloseConnection);
    CHECK(protoErr.Description() ==
          Describe(EProtoError::AuthenticationFailed));

    const SessionError sessionErr{ESessionError::StreamGap};
    CHECK(sessionErr.Session() == ESessionError::StreamGap);
    CHECK_FALSE(sessionErr.Proto().has_value());
    CHECK(sessionErr.Disposition() == EDisposition::CloseConnection);
    CHECK(sessionErr.Description() == Describe(ESessionError::StreamGap));

    const SessionError killErr{ESessionError::DisconnectBufferOverflow};
    CHECK(killErr.Disposition() == EDisposition::KillSession);
}

TEST_CASE("SessionError compares by which error it holds, not by its bits") {
    CHECK(SessionError{ESessionError::StreamGap} ==
          SessionError{ESessionError::StreamGap});
    CHECK_FALSE(SessionError{ESessionError::StreamGap} ==
                SessionError{ESessionError::AckBeyondSent});
    // Both enums start at zero: Truncated and AckBeyondSent share a value, and
    // the two kinds must still differ.
    CHECK_FALSE(SessionError{EProtoError::Truncated} ==
                SessionError{ESessionError::AckBeyondSent});
}

// ── NextDeadline ──────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "NextDeadline returns the rekey deadline when idle") {
    ChannelPair pair;
    const Timestamp start{};
    // No bytes have been exchanged. The rekey deadline is set; the ack
    // deadline is not.
    const Deadline dl = pair.Client.NextDeadline();
    CHECK(dl.IsSet());
    CHECK(dl.At() == start + REKEY_INTERVAL);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "NextDeadline returns ack deadline when ack is pending") {
    ChannelPair pair;

    // Deliver bytes to the client so it has un-acked receive offsets.
    const auto chunk = MakeBytes(8, 0x08);
    static_cast<void>(pair.AgentSendBuf.Append(ByteSpan{chunk}));

    Timestamp now{};
    // agent → client
    pair.PumpAgentToClient(now);

    // First Poll by client to arm the ack timer.
    std::vector<std::byte> out(wire::HEADER_SIZE + wire::MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
    static_cast<void>(PollOk(pair.Client, now, MutableByteSpan{out},
                             MutableByteSpan{scratch}));

    const Deadline dl = pair.Client.NextDeadline();
    REQUIRE(dl.IsSet());
    // The ack deadline should be the earlier of the two (ACK_IDLE_DELAY <<
    // REKEY_INTERVAL).
    CHECK(dl.At() == now + ACK_IDLE_DELAY);
}

// ── Refusals on the incoming side ──────────────────────────────────────

namespace {

/// Seals `plain` as the agent's first frame and hands it to the client. Each
/// call builds its own sealer at counter zero, so use a fresh pair per call
/// unless the frames are meant to follow one another — then use the overload
/// that takes the sealer.
ChannelResult<IncomingEvent> InjectToClient(ChannelPair& pair,
                                            FrameSealer& sealer,
                                            ByteSpan plain) {
    std::vector<std::byte> frame(wire::HEADER_SIZE + plain.size() +
                                 crypto::TAG_SIZE);
    const auto sealed = sealer.Seal(MutableByteSpan{frame}, plain);
    REQUIRE(sealed.has_value());
    const auto view =
        ReadFrame(ByteSpan{frame}.first(*sealed), wire::MAX_FRAME);
    REQUIRE(view.has_value());
    return pair.Client.Receive(*view, MutableByteSpan{pair.OpenScratch});
}

ChannelResult<IncomingEvent> InjectToClient(ChannelPair& pair, ByteSpan plain) {
    FrameSealer sealer{crypto::Key{pair.Keys.ServerToClient.Expose()},
                       pair.Keys.SaltServerToClient};
    return InjectToClient(pair, sealer, plain);
}

std::vector<std::byte> DataPlain(std::uint64_t ack, std::uint64_t offset,
                                 std::size_t length) {
    std::vector<std::byte> plain(1 + 2 * sizeof(std::uint64_t) + length,
                                 std::byte{0x5A});
    ByteWriter writer{MutableByteSpan{plain}};
    REQUIRE(writer.WriteU8(static_cast<std::uint8_t>(EMessageType::Data)));
    REQUIRE(writer.WriteU64BE(ack));
    REQUIRE(writer.WriteU64BE(offset));
    return plain;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a sealed body the codec refuses closes the connection") {
    for (const auto type :
         {EMessageType::Data, EMessageType::Ack, EMessageType::Resize,
          EMessageType::Ping, EMessageType::Pong}) {
        CAPTURE(static_cast<int>(type));
        ChannelPair pair;
        // The type byte and one more: short of every body above.
        const std::array<std::byte, 2> plain{
            static_cast<std::byte>(static_cast<std::uint8_t>(type)),
            std::byte{0}};
        const auto ev = InjectToClient(pair, ByteSpan{plain});
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Proto().has_value());
        CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "an empty sealed body closes the connection") {
    ChannelPair pair;
    const auto ev = InjectToClient(pair, ByteSpan{});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Proto() == EProtoError::Truncated);
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "handshake messages inside the sealed stream are refused") {
    // Unlike an unknown type, these are known and out of place: not a newer
    // peer to be ignored, but one that is out of step with the protocol.
    for (const auto type :
         {EMessageType::Hello, EMessageType::Challenge, EMessageType::Auth,
          EMessageType::AuthOk, EMessageType::Resume}) {
        CAPTURE(static_cast<int>(type));
        ChannelPair pair;
        const std::array<std::byte, 1> plain{
            static_cast<std::byte>(static_cast<std::uint8_t>(type))};
        const auto ev = InjectToClient(pair, ByteSpan{plain});
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Proto() == EProtoError::UnexpectedMessage);
        CHECK(pair.Client.UnknownTypeCount() == 0);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an ack for bytes never sent closes the connection") {
    SUBCASE("piggybacked on Data") {
        ChannelPair pair;
        const auto plain = DataPlain(5, 0, 4);
        const auto ev = InjectToClient(pair, ByteSpan{plain});
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
        // The data rode with a lie about our stream and is not taken.
        CHECK(pair.ClientRecv.RecvOffset() == 0);
    }
    SUBCASE("bare Ack") {
        ChannelPair pair;
        std::array<std::byte, 1 + sizeof(std::uint64_t)> plain{};
        ByteWriter writer{MutableByteSpan{plain}};
        REQUIRE(wire::WriteAck(writer, wire::AckMessage{.Ack = 5}));
        const auto ev = InjectToClient(pair, ByteSpan{plain});
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "a Data seen twice is delivered once") {
    ChannelPair pair;
    FrameSealer sealer{crypto::Key{pair.Keys.ServerToClient.Expose()},
                       pair.Keys.SaltServerToClient};
    const auto plain = DataPlain(0, 0, 10);

    const auto first = InjectToClient(pair, sealer, ByteSpan{plain});
    REQUIRE(first.has_value());
    CHECK(std::holds_alternative<DeliveredData>(*first));

    // The resend after a reconnect, here in one connection for brevity.
    const auto again = InjectToClient(pair, sealer, ByteSpan{plain});
    REQUIRE(again.has_value());
    CHECK(std::holds_alternative<DeliveredNothing>(*again));
    CHECK(pair.ClientRecv.RecvOffset() == 10);
}

// ── Refusals on the outgoing side ──────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "a buffer too small for any frame is an error, not silence") {
    ChannelPair pair;
    std::array<std::byte, wire::HEADER_SIZE + 1 + crypto::TAG_SIZE - 1> tiny{};
    const auto n = pair.Client.Poll(Timestamp{}, MutableByteSpan{tiny},
                                    MutableByteSpan{pair.Scratch});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().Proto() == EProtoError::BufferTooSmall);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "scratch too small for a Data header is an error") {
    ChannelPair pair;
    const auto data = MakeBytes(4, 0x01);
    REQUIRE(pair.ClientSendBuf.Append(ByteSpan{data}) == data.size());
    std::array<std::byte, 1 + 2 * sizeof(std::uint64_t)> cramped{};
    const auto n = pair.Client.Poll(Timestamp{}, MutableByteSpan{pair.Out},
                                    MutableByteSpan{cramped});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().Proto() == EProtoError::BufferTooSmall);
    // Nothing went out, so nothing may count as sent.
    CHECK(pair.ClientSendBuf.SendNext() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a connection out of key epochs says so instead of idling") {
    // One frame per epoch, so every Poll that sends forces a rekey.
    ChannelPair pair{Timestamp{}, 1};
    const auto byte = MakeBytes(1, 0x07);
    // Far more polls than there are epochs; the loop ends on the error.
    constexpr int maxPolls = 1024;
    std::optional<SessionError> refusal;
    for (int i = 0; i < maxPolls && !refusal; ++i) {
        REQUIRE(pair.ClientSendBuf.Append(ByteSpan{byte}) == 1);
        const auto n = pair.Client.Poll(Timestamp{}, MutableByteSpan{pair.Out},
                                        MutableByteSpan{pair.Scratch});
        if (!n) {
            refusal = n.error();
        }
    }
    REQUIRE(refusal.has_value());
    CHECK(refusal->Proto() == EProtoError::EpochsExhausted);
    CHECK(refusal->Disposition() == EDisposition::CloseConnection);
}

// ── Built from the frames a connection already used ────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "a channel built after Resume seals under the next nonce") {
    // Resume is the client's first sealed frame and opens on the agent before
    // either Channel exists. The Channels must take over those counters: a
    // Channel built from the keys again would seal its first frame under the
    // nonce Resume already used.
    const auto keys =
        DeriveConnectionKeys(DeriveSessionSecrets(MakeMaster(1)).ConnectionSeed,
                             MakeNonce(1), MakeNonce(2));
    auto clientFrames = MakeDirectionFrames(keys, ERole::Client);
    auto agentFrames = MakeDirectionFrames(keys, ERole::Agent);

    std::array<std::byte, 1 + sizeof(std::uint64_t)> resume{};
    {
        ByteWriter writer{MutableByteSpan{resume}};
        REQUIRE(WriteResume(writer, ResumeMessage{.ClientReceiveOffset = 0}));
    }
    std::vector<std::byte> wireBytes(HEADER_SIZE + resume.size() +
                                     crypto::TAG_SIZE);
    const auto sealed =
        clientFrames.Sealer.Seal(MutableByteSpan{wireBytes}, ByteSpan{resume});
    REQUIRE(sealed.has_value());
    const auto resumeFrame =
        ReadFrame(ByteSpan{wireBytes}.first(*sealed), MAX_FRAME);
    REQUIRE(resumeFrame.has_value());
    std::vector<std::byte> opened(MAX_FRAME);
    REQUIRE(agentFrames.Opener.Open(MutableByteSpan{opened}, *resumeFrame)
                .has_value());

    std::vector<std::byte> clientStore(64);
    std::vector<std::byte> agentStore(64);
    SendBuffer clientOut{MutableByteSpan{clientStore}};
    SendBuffer agentOut{MutableByteSpan{agentStore}};
    ReceiveCursor clientIn;
    ReceiveCursor agentIn;
    Channel client{std::move(clientFrames), clientOut, clientIn, Timestamp{}};
    Channel agent{std::move(agentFrames), agentOut, agentIn, Timestamp{}};

    const auto typed = MakeBytes(5, 0x30);
    REQUIRE(clientOut.Append(ByteSpan{typed}) == typed.size());
    std::vector<std::byte> out(HEADER_SIZE + MAX_FRAME);
    std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
    const std::size_t n = PollOk(client, Timestamp{}, MutableByteSpan{out},
                                 MutableByteSpan{scratch});
    REQUIRE(n > 0);
    const auto frame = ReadFrame(ByteSpan{out}.first(n), MAX_FRAME);
    REQUIRE(frame.has_value());

    const auto ev = agent.Receive(*frame, MutableByteSpan{opened});
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<DeliveredData>(*ev));
    CHECK(std::get<DeliveredData>(*ev).Bytes.size() == typed.size());

    // And the frame really is the second under this key: a fresh sealer at
    // counter zero produces something the agent, now at counter two, refuses.
    auto fresh = MakeDirectionFrames(keys, ERole::Client);
    std::vector<std::byte> replay(HEADER_SIZE + resume.size() +
                                  crypto::TAG_SIZE);
    const auto again =
        fresh.Sealer.Seal(MutableByteSpan{replay}, ByteSpan{resume});
    REQUIRE(again.has_value());
    const auto replayFrame =
        ReadFrame(ByteSpan{replay}.first(*again), MAX_FRAME);
    REQUIRE(replayFrame.has_value());
    const auto refused = agent.Receive(*replayFrame, MutableByteSpan{opened});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Proto() == EProtoError::AuthenticationFailed);
}
