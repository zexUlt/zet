/// EchoAck on the Channel (§7 violation table, §9 ADR-036).
///
/// Each violation row has its own test and closes the connection
/// (CloseConnection): the peer is authenticated, but lies about the stream.
/// Frames are injected into Channel::Receive directly, as in
/// grid_violation_test.cpp, so each test needs no handshake. The send side is
/// driven the other way round: an agent Channel's Poll feeds a client
/// Channel's Receive.

#include "zet/session/channel.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/receive_cursor.hpp"
#include "zet/session/send_buffer.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"
#include "zet/wire/sealed_frame.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

crypto::Key CloneKey(const crypto::Key& src) {
    crypto::Key copy;
    copy.Expose() = src.Expose();
    return copy;
}

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return crypto::Key{b};
}

HandshakeNonce MakeNonce(std::uint8_t seed) {
    HandshakeNonce n{};
    for (std::size_t i = 0; i < n.size(); ++i) {
        n[i] = static_cast<std::byte>(seed * 7 + static_cast<std::uint8_t>(i));
    }
    return n;
}

/// A client and an agent Channel on shared keys. `Inject*` seal a frame by
/// hand for one of them; the agent's Poll output goes to the client through
/// Relay. A test uses one or the other, not both: the two seal under the same
/// nonce counter.
struct Rig {
    static constexpr std::size_t BUF_SIZE = RESEND_BUFFER_MAX;
    static constexpr std::size_t SNAPSHOT_SIZE = 4;

    explicit Rig(std::uint16_t version = PROTOCOL_VERSION_GRID,
                 bool negotiated = true)
        : Keys{DeriveConnectionKeys(
              DeriveSessionSecrets(MakeKey(1)).ConnectionSeed, MakeNonce(1),
              MakeNonce(2))},
          ClientSendStore(BUF_SIZE),
          AgentSendStore(BUF_SIZE),
          ClientSendBuf{MutableByteSpan{ClientSendStore}, 0},
          AgentSendBuf{MutableByteSpan{AgentSendStore}, 0},
          ClientRecv{0},
          AgentRecv{0},
          SnapshotStore(SNAPSHOT_SIZE, std::byte{}),
          Client{Keys,
                 ERole::Client,
                 ClientSendBuf,
                 ClientRecv,
                 Timestamp{},
                 MAX_FRAMES_PER_EPOCH,
                 MAX_BYTES_PER_EPOCH,
                 {},
                 version,
                 MutableByteSpan{SnapshotStore}},
          Agent{
              Keys,        ERole::Agent,         AgentSendBuf,        AgentRecv,
              Timestamp{}, MAX_FRAMES_PER_EPOCH, MAX_BYTES_PER_EPOCH, {},
              version},
          ToClient{CloneKey(Keys.ServerToClient), Keys.SaltServerToClient},
          ToAgent{CloneKey(Keys.ClientToServer), Keys.SaltClientToServer} {
        if (negotiated) {
            Client.EnableEchoAck();
            Agent.EnableEchoAck();
        }
    }

    ChannelResult<IncomingEvent> InjectToClient(ByteSpan plain) {
        return Inject(Client, ToClient, plain);
    }
    ChannelResult<IncomingEvent> InjectToAgent(ByteSpan plain) {
        return Inject(Agent, ToAgent, plain);
    }

    ChannelResult<IncomingEvent> Receive(Channel& channel, ByteSpan frame) {
        const auto view = ReadFrame(frame, MAX_FRAME);
        REQUIRE(view.has_value());
        return channel.Receive(Timestamp{}, *view, MutableByteSpan{Scratch});
    }

    /// One frame from the agent's Poll to the client, if the agent has one.
    std::optional<ChannelResult<IncomingEvent>> Relay() {
        std::vector<std::byte> out(HEADER_SIZE + MAX_FRAME);
        std::vector<std::byte> scratch(Channel::POLL_SCRATCH_SIZE);
        const auto n = Agent.Poll(Timestamp{}, MutableByteSpan{out},
                                  MutableByteSpan{scratch});
        REQUIRE(n.has_value());
        if (*n == 0) {
            return std::nullopt;
        }
        return Receive(Client, ByteSpan{out}.first(*n));
    }

    /// The client has received `count` bytes of output at `offset`.
    void ReceiveOutput(std::uint64_t offset, std::size_t count) {
        std::vector<std::byte> payload(count, std::byte{0x41});
        std::vector<std::byte> plain(1 + 16 + count);
        ByteWriter w{MutableByteSpan{plain}};
        REQUIRE(WriteData(
            w, DataMessage{
                   .Ack = 0, .Offset = offset, .Payload = ByteSpan{payload}}));
        REQUIRE(InjectToClient(w.Written()).has_value());
    }

    /// The agent opens generation `gen`: a whole snapshot, which leaves the
    /// client's receive offset at 0.
    void ReceiveSnapshot(std::uint32_t gen) {
        std::vector<std::byte> plain(32);
        ByteWriter begin{MutableByteSpan{plain}};
        REQUIRE(WriteGridBegin(
            begin,
            GridBeginMessage{.StreamGen = gen, .TotalLen = SNAPSHOT_SIZE}));
        REQUIRE(InjectToClient(begin.Written()).has_value());

        const std::array<std::byte, SNAPSHOT_SIZE> body{};
        ByteWriter chunk{MutableByteSpan{plain}};
        REQUIRE(WriteGridChunk(
            chunk, GridChunkMessage{.ChunkOffset = 0, .Data = ByteSpan{body}}));
        REQUIRE(InjectToClient(chunk.Written()).has_value());

        ByteWriter end{MutableByteSpan{plain}};
        REQUIRE(WriteGridEnd(end, GridEndMessage{}));
        const auto delivered = InjectToClient(end.Written());
        REQUIRE(delivered.has_value());
        REQUIRE(std::holds_alternative<DeliveredGridSnapshot>(*delivered));
    }

    /// The client has written `count` bytes of input.
    void WriteInput(std::size_t count) {
        const std::vector<std::byte> bytes(count, std::byte{0x42});
        REQUIRE(ClientSendBuf.Append(ByteSpan{bytes}) == count);
    }

    ConnectionKeys Keys;
    std::vector<std::byte> ClientSendStore;
    std::vector<std::byte> AgentSendStore;
    SendBuffer ClientSendBuf;
    SendBuffer AgentSendBuf;
    ReceiveCursor ClientRecv;
    ReceiveCursor AgentRecv;
    std::vector<std::byte> SnapshotStore;
    Channel Client;
    Channel Agent;
    FrameSealer ToClient;
    FrameSealer ToAgent;
    std::vector<std::byte> Scratch{MAX_FRAME, std::byte{}};

private:
    ChannelResult<IncomingEvent> Inject(Channel& channel, FrameSealer& sealer,
                                        ByteSpan plain) {
        std::vector<std::byte> frame(HEADER_SIZE + plain.size() +
                                     crypto::TAG_SIZE);
        const auto sealed = sealer.Seal(MutableByteSpan{frame}, plain);
        REQUIRE(sealed.has_value());
        return Receive(channel, ByteSpan{frame}.first(*sealed));
    }
};

std::vector<std::byte> EchoAckRaw(std::uint32_t gen, std::uint64_t output,
                                  std::uint64_t input) {
    std::vector<std::byte> plain(1 + 20);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(WriteEchoAck(w, EchoAckMessage{.StreamGen = gen,
                                           .OutputOffset = output,
                                           .InputOffset = input}));
    return plain;
}

/// A body of `size` bytes after the EchoAck type byte.
std::vector<std::byte> EchoAckOfSize(std::size_t size) {
    std::vector<std::byte> plain(1 + size, std::byte{});
    plain[0] = static_cast<std::byte>(EMessageType::EchoAck);
    return plain;
}

void CheckCloses(const ChannelResult<IncomingEvent>& result) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Disposition() == EDisposition::CloseConnection);
}

void CheckCloses(const ChannelResult<IncomingEvent>& result,
                 EProtoError error) {
    CheckCloses(result);
    CHECK(result.error().Proto() == error);
}

/// An EchoAck the stream allows is delivered unchanged. Run after a violation
/// to show it changed nothing: the channel keeps no state from a rejected
/// frame.
void CheckDelivers(Rig& rig, std::uint32_t gen, std::uint64_t output,
                   std::uint64_t input) {
    const auto result =
        rig.InjectToClient(ByteSpan{EchoAckRaw(gen, output, input)});
    REQUIRE(result.has_value());
    const auto* delivered = std::get_if<DeliveredEchoAck>(&*result);
    REQUIRE(delivered != nullptr);
    CHECK(delivered->Ack == EchoAckMessage{.StreamGen = gen,
                                           .OutputOffset = output,
                                           .InputOffset = input});
}

TEST_CASE_FIXTURE(SodiumFixture, "a valid EchoAck is delivered to the client") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    rig.WriteInput(5);
    CheckDelivers(rig, 0, 10, 5);
}

TEST_CASE_FIXTURE(SodiumFixture, "an EchoAck at the very start is valid") {
    Rig rig;
    CheckDelivers(rig, 0, 0, 0);
}

// ── The violation table, one row per test ──────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture, "EchoAck from the client closes the agent") {
    Rig rig;
    CheckCloses(rig.InjectToAgent(ByteSpan{EchoAckRaw(0, 0, 0)}));
}

TEST_CASE_FIXTURE(SodiumFixture, "EchoAck at ver=1 closes the client") {
    Rig rig{PROTOCOL_VERSION_MIN, true};
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 0)}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "EchoAck without ECHO_ACK closes the client at ver=2") {
    Rig rig{PROTOCOL_VERSION_GRID, false};
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 0)}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck body that is not 20 bytes closes") {
    Rig rig;
    for (std::size_t size = 0; size < 20; ++size) {
        CAPTURE(size);
        CheckCloses(rig.InjectToClient(ByteSpan{EchoAckOfSize(size)}),
                    EProtoError::Truncated);
    }
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckOfSize(21)}),
                EProtoError::MalformedField);
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckOfSize(64)}),
                EProtoError::MalformedField);
    CheckDelivers(rig, 0, 0, 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "EchoAck between GridBegin and GridEnd closes") {
    Rig rig;
    std::vector<std::byte> plain(32);
    ByteWriter begin{MutableByteSpan{plain}};
    REQUIRE(WriteGridBegin(
        begin,
        GridBeginMessage{.StreamGen = 1, .TotalLen = Rig::SNAPSHOT_SIZE}));
    REQUIRE(rig.InjectToClient(begin.Written()).has_value());

    // Even one that is valid in the generation still current: only the grid
    // window rejects it, and as an unexpected message.
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 0)}),
                EProtoError::UnexpectedMessage);
    // And one that names the generation being opened.
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(1, 0, 0)}));
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "EchoAck right after GridEnd is valid in the new generation") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    rig.ReceiveSnapshot(1);
    CheckDelivers(rig, 1, 0, 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "EchoAck for another generation closes") {
    Rig rig;
    rig.ReceiveOutput(0, 10);

    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(1, 0, 0)}));
    CheckDelivers(rig, 0, 10, 0);

    // After a snapshot the old generation is the wrong one too.
    rig.ReceiveSnapshot(1);
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 0)}));
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(2, 0, 0)}));
    CheckDelivers(rig, 1, 0, 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "EchoAck beyond the received output closes") {
    Rig rig;
    rig.ReceiveOutput(0, 10);

    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 11, 0)}));
    CheckDelivers(rig, 0, 10, 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the output bound is the new generation's after a snapshot") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    rig.ReceiveSnapshot(1);

    // Ten bytes were received, but in generation 0.
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(1, 1, 0)}));
    rig.ReceiveOutput(0, 3);
    CheckDelivers(rig, 1, 3, 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "EchoAck beyond the written input closes") {
    Rig rig;
    rig.WriteInput(5);

    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 6)}));
    CheckDelivers(rig, 0, 0, 5);
}

TEST_CASE_FIXTURE(SodiumFixture, "input_offset going back closes") {
    Rig rig;
    rig.WriteInput(9);
    CheckDelivers(rig, 0, 0, 7);

    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 6)}));
    // The rejected frame left the previous one in force.
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 0, 3)}));
    CheckDelivers(rig, 0, 0, 7);
    CheckDelivers(rig, 0, 0, 9);
}

TEST_CASE_FIXTURE(SodiumFixture, "output_offset going back closes") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    CheckDelivers(rig, 0, 7, 0);

    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(0, 6, 0)}));
    CheckDelivers(rig, 0, 7, 0);
    CheckDelivers(rig, 0, 10, 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck repeated with the same values is valid") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    rig.WriteInput(5);
    CheckDelivers(rig, 0, 10, 5);
    CheckDelivers(rig, 0, 10, 5);
    CheckDelivers(rig, 0, 10, 5);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output restarts with the generation, input does not") {
    Rig rig;
    rig.ReceiveOutput(0, 10);
    rig.WriteInput(5);
    CheckDelivers(rig, 0, 10, 5);

    rig.ReceiveSnapshot(1);
    // Output may restart below the old generation's last value.
    CheckDelivers(rig, 1, 0, 5);
    // Input never restarts.
    CheckCloses(rig.InjectToClient(ByteSpan{EchoAckRaw(1, 0, 4)}));
}

// ── Sending ────────────────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture, "an EchoAck is queued only where it may go") {
    const EchoAckMessage ack{};
    {
        Rig rig;
        CHECK(rig.Agent.QueueEchoAck(ack));
    }
    {
        Rig rig{PROTOCOL_VERSION_GRID, false};
        CHECK_FALSE(rig.Agent.QueueEchoAck(ack));
    }
    {
        Rig rig{PROTOCOL_VERSION_MIN, true};
        CHECK_FALSE(rig.Agent.QueueEchoAck(ack));
    }
    {
        Rig rig;
        CHECK_FALSE(rig.Client.QueueEchoAck(ack));
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "a queued EchoAck goes before Data") {
    Rig rig;
    const std::vector<std::byte> output(10, std::byte{0x43});
    REQUIRE(rig.AgentSendBuf.Append(ByteSpan{output}) == output.size());
    REQUIRE(rig.Agent.QueueEchoAck(
        EchoAckMessage{.StreamGen = 0, .OutputOffset = 0, .InputOffset = 0}));

    const auto first = rig.Relay();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK(std::holds_alternative<DeliveredEchoAck>(**first));

    const auto second = rig.Relay();
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK(std::holds_alternative<DeliveredData>(**second));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a later queued EchoAck replaces an unsent one") {
    Rig rig;
    rig.WriteInput(9);
    REQUIRE(rig.Agent.QueueEchoAck(EchoAckMessage{.InputOffset = 3}));
    REQUIRE(rig.Agent.QueueEchoAck(EchoAckMessage{.InputOffset = 9}));

    const auto sent = rig.Relay();
    REQUIRE(sent.has_value());
    REQUIRE(sent->has_value());
    const auto* echo = std::get_if<DeliveredEchoAck>(&**sent);
    REQUIRE(echo != nullptr);
    CHECK(echo->Ack.InputOffset == 9);
    CHECK_FALSE(rig.Relay().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck waits out the snapshot and follows GridEnd") {
    Rig rig;
    const std::array<std::byte, Rig::SNAPSHOT_SIZE> snapshot{};
    const std::vector<std::byte> output(10, std::byte{0x43});
    REQUIRE(rig.AgentSendBuf.Append(ByteSpan{output}) == output.size());
    REQUIRE(rig.Agent.StartGrid(ByteSpan{snapshot}, 0, 1, 0));
    REQUIRE(rig.Agent.QueueEchoAck(
        EchoAckMessage{.StreamGen = 1, .OutputOffset = 0, .InputOffset = 0}));

    // GridBegin, GridChunk, GridEnd: nothing else in between.
    for (int i = 0; i < 2; ++i) {
        const auto grid = rig.Relay();
        REQUIRE(grid.has_value());
        REQUIRE(grid->has_value());
        CHECK(std::holds_alternative<DeliveredNothing>(**grid));
    }
    const auto end = rig.Relay();
    REQUIRE(end.has_value());
    REQUIRE(end->has_value());
    CHECK(std::holds_alternative<DeliveredGridSnapshot>(**end));

    // The first EchoAck of the generation, ahead of any Data and before the
    // client has answered with GridApplied.
    const auto echo = rig.Relay();
    REQUIRE(echo.has_value());
    REQUIRE(echo->has_value());
    CHECK(std::holds_alternative<DeliveredEchoAck>(**echo));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an EchoAck is not sent before the snapshot is supplied") {
    Rig rig;
    rig.Agent.SuppressDataUntilGrid();
    REQUIRE(rig.Agent.QueueEchoAck(EchoAckMessage{}));
    CHECK_FALSE(rig.Relay().has_value());
}

}  // namespace
