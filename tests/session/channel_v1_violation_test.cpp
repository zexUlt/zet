/// Grid protocol violation tests for v1 channels.
///
/// At wire version 1 the grid extension does not exist.  Every Grid* and
/// GridApplied message a v1 channel receives must be treated as
/// UnexpectedMessage → CloseConnection.
///
/// The channel constructor's `version` argument is set to
/// PROTOCOL_VERSION_MIN (1) for all channels in this file, which is the
/// version that PROTOCOL_VERSION_MAX-capped peers negotiate.
///
/// Note: two tests from the v2 violation file ("a complete grid snapshot is
/// delivered" and "second snapshot with higher gen") were omitted because
/// their REQUIRE(DeliverValidSnapshot(...)) assertions contradict v1 design —
/// at v1, GridBegin is UnexpectedMessage and DeliverValidSnapshot returns
/// false.

#include "zet/session/channel.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
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

struct ViolationRig {
    static constexpr std::size_t BUF_SIZE = RESEND_BUFFER_MAX;

    explicit ViolationRig()
        : Keys{DeriveConnectionKeys(
              DeriveSessionSecrets(MakeKey(1)).ConnectionSeed, MakeNonce(1),
              MakeNonce(2))},
          ClientSendStore(BUF_SIZE),
          AgentSendStore(BUF_SIZE),
          ClientSendBuf{MutableByteSpan{ClientSendStore}, 0},
          AgentSendBuf{MutableByteSpan{AgentSendStore}, 0},
          ClientRecv{0},
          AgentRecv{0},
          ClientSnapshotStore(wire::SNAPSHOT_MAX_BYTES, std::byte{}),
          // version=1 (PROTOCOL_VERSION_MIN): grid protocol not active.
          Client{Keys,
                 ERole::Client,
                 ClientSendBuf,
                 ClientRecv,
                 Timestamp{},
                 MAX_FRAMES_PER_EPOCH,
                 MAX_BYTES_PER_EPOCH,
                 {},
                 1,
                 MutableByteSpan{ClientSnapshotStore}},
          Agent{Keys,
                ERole::Agent,
                AgentSendBuf,
                AgentRecv,
                Timestamp{},
                MAX_FRAMES_PER_EPOCH,
                MAX_BYTES_PER_EPOCH,
                {},
                1},
          ClientInjector{CloneKey(Keys.ServerToClient),
                         Keys.SaltServerToClient},
          AgentInjector{CloneKey(Keys.ClientToServer),
                        Keys.SaltClientToServer} {}

    static crypto::Key MakeKey(std::uint8_t seed) {
        crypto::KeyBytes b{};
        for (std::size_t i = 0; i < b.size(); ++i)
            b[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
        return crypto::Key{b};
    }

    static HandshakeNonce MakeNonce(std::uint8_t seed) {
        HandshakeNonce n{};
        for (std::size_t i = 0; i < n.size(); ++i)
            n[i] =
                static_cast<std::byte>(seed * 7 + static_cast<std::uint8_t>(i));
        return n;
    }

    static crypto::Key CloneKey(const crypto::Key& src) noexcept {
        crypto::Key copy;
        copy.Expose() = src.Expose();
        return copy;
    }

    ChannelResult<IncomingEvent> InjectToClient(ByteSpan plain) {
        return Inject(Client, ClientInjector, plain, OpenScratch);
    }
    ChannelResult<IncomingEvent> InjectToAgent(ByteSpan plain) {
        return Inject(Agent, AgentInjector, plain, OpenScratch);
    }

    ConnectionKeys Keys;
    std::vector<std::byte> ClientSendStore;
    std::vector<std::byte> AgentSendStore;
    SendBuffer ClientSendBuf;
    SendBuffer AgentSendBuf;
    ReceiveCursor ClientRecv;
    ReceiveCursor AgentRecv;
    std::vector<std::byte> ClientSnapshotStore;
    Channel Client;
    Channel Agent;
    FrameSealer ClientInjector;
    FrameSealer AgentInjector;
    std::vector<std::byte> OpenScratch{wire::MAX_FRAME, std::byte{}};

private:
    static ChannelResult<IncomingEvent> Inject(
        Channel& ch, FrameSealer& sealer, ByteSpan plain,
        std::vector<std::byte>& scratch) {
        std::vector<std::byte> frame(HEADER_SIZE + plain.size() +
                                     crypto::TAG_SIZE);
        const auto sealed = sealer.Seal(MutableByteSpan{frame}, plain);
        REQUIRE(sealed.has_value());
        const auto view = ReadFrame(ByteSpan{frame}.first(*sealed), MAX_FRAME);
        REQUIRE(view.has_value());
        return ch.Receive(Timestamp{}, *view, MutableByteSpan{scratch});
    }
};

// Raw plaintext builders (bypass WriteGridBegin validation for illegal values).
std::vector<std::byte> GridBeginRaw(std::uint8_t flags, std::uint32_t streamGen,
                                    std::uint32_t scrollbackLen,
                                    std::uint32_t totalLen) {
    std::vector<std::byte> plain(14);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridBegin)));
    REQUIRE(w.WriteU8(flags));
    REQUIRE(w.WriteU32BE(streamGen));
    REQUIRE(w.WriteU32BE(scrollbackLen));
    REQUIRE(w.WriteU32BE(totalLen));
    return plain;
}
std::vector<std::byte> GridChunkRaw(std::uint32_t offset, ByteSpan data) {
    std::vector<std::byte> plain(1 + 4 + data.size());
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridChunk)));
    REQUIRE(w.WriteU32BE(offset));
    for (auto b : data) REQUIRE(w.WriteU8(static_cast<std::uint8_t>(b)));
    return plain;
}
std::vector<std::byte> GridEndRaw() {
    std::vector<std::byte> plain(1);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridEnd)));
    return plain;
}
std::vector<std::byte> GridAppliedRaw(std::uint32_t streamGen) {
    std::vector<std::byte> plain(5);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridApplied)));
    REQUIRE(w.WriteU32BE(streamGen));
    return plain;
}
std::vector<std::byte> DataWithAck(std::uint64_t ack, std::uint32_t length) {
    std::vector<std::byte> plain(1 + 8 + 8 + length, std::byte{0xBB});
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::Data)));
    REQUIRE(w.WriteU64BE(ack));
    REQUIRE(w.WriteU64BE(0));
    return plain;
}

}  // namespace

// ── At v1, every Grid* message is UnexpectedMessage → CloseConnection
// ─────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridBegin received by agent is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 0, 16);
    const auto ev = rig.InjectToAgent(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridBegin received by client is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 0, 16);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridBegin with stream_gen=0 is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 0, 0, 16);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v1: GridBegin with total_len above limit is UnexpectedMessage") {
    ViolationRig rig;
    const std::uint32_t tooBig = SNAPSHOT_MAX_BYTES + 1;
    const auto plain = GridBeginRaw(0, 1, 0, tooBig);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v1: GridBegin with scrollback exceeding total is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 100, 50);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridChunk received by client is UnexpectedMessage") {
    ViolationRig rig;
    std::array<std::byte, 4> payload{};
    const auto plain = GridChunkRaw(0, ByteSpan{payload});
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridEnd received by client is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridEndRaw();
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridApplied received by client is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridAppliedRaw(1);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridApplied received by agent is UnexpectedMessage") {
    ViolationRig rig;
    const auto plain = GridAppliedRaw(99);
    const auto ev = rig.InjectToAgent(ByteSpan{plain});
    MESSAGE(
        "GridApplied(99) with nothing awaited: has_value=" << ev.has_value());
    CHECK_FALSE(ev.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: GridApplied to agent when grid emission pending is "
                  "UnexpectedMessage") {
    ViolationRig rig;
    const std::vector<std::byte> snap(32, std::byte{0xCC});
    rig.Agent.StartGrid(ByteSpan{snap}, 0, 1, 0);
    const auto plain = GridAppliedRaw(99);
    const auto ev = rig.InjectToAgent(ByteSpan{plain});
    MESSAGE(
        "GridApplied(99) when awaiting gen 1: has_value=" << ev.has_value());
    CHECK_FALSE(ev.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1: stale acks during grid emission do not advance Base") {
    ViolationRig rig;
    const std::vector<std::byte> snap(32, std::byte{0xCC});
    const std::vector<std::byte> junk(200, std::byte{1});
    std::ignore = rig.AgentSendBuf.Append(ByteSpan{junk});
    rig.Agent.StartGrid(ByteSpan{snap}, 0, 1, 0);
    for (std::uint64_t ack : {50ull, 1000ull}) {
        const auto plain = DataWithAck(ack, 4);
        const auto ev = rig.InjectToAgent(ByteSpan{plain});
        CHECK(ev.has_value());
        CHECK(rig.AgentSendBuf.Base() == 0);
    }
}
