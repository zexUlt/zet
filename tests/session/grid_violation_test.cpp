/// Grid snapshot protocol violation tests.
///
/// Each test corresponds to one row of the §9 ADR-030 violation table.  Every
/// violation closes the connection (CloseConnection disposition).  There are
/// no KillSession violations in the grid path: a misbehaving peer gets the
/// connection dropped, not the session reaped.
///
/// The tests inject raw frames directly into Channel::Receive rather than
/// going through a full connection, which lets each test focus on one
/// violation without having to drive a handshake first.

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

// ── Test fixture ──────────────────────────────────────────────────────────

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// A minimal channel pair (client + agent on shared keys) with helpers to
/// inject a sealed frame carrying arbitrary plaintext bytes.
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
          Client{Keys,
                 ERole::Client,
                 ClientSendBuf,
                 ClientRecv,
                 Timestamp{},
                 MAX_FRAMES_PER_EPOCH,
                 MAX_BYTES_PER_EPOCH,
                 {},
                 2},
          Agent{Keys,
                ERole::Agent,
                AgentSendBuf,
                AgentRecv,
                Timestamp{},
                MAX_FRAMES_PER_EPOCH,
                MAX_BYTES_PER_EPOCH,
                {},
                2},
          ClientInjector{CloneKey(Keys.ServerToClient),
                         Keys.SaltServerToClient},
          AgentInjector{CloneKey(Keys.ClientToServer),
                        Keys.SaltClientToServer} {}

    static crypto::Key MakeKey(std::uint8_t seed) {
        crypto::KeyBytes b{};
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
        }
        return crypto::Key{b};
    }

    static HandshakeNonce MakeNonce(std::uint8_t seed) {
        HandshakeNonce n{};
        for (std::size_t i = 0; i < n.size(); ++i) {
            n[i] =
                static_cast<std::byte>(seed * 7 + static_cast<std::uint8_t>(i));
        }
        return n;
    }

    static crypto::Key CloneKey(const crypto::Key& src) noexcept {
        crypto::Key copy;
        copy.Expose() = src.Expose();
        return copy;
    }

    /// Seal `plain` with the agent→client key and deliver it to the client.
    /// Uses a persistent sealer so the nonce counter advances correctly across
    /// multiple calls: a fresh sealer per call would reset to nonce 0 and
    /// cause the channel's opener to fail on the second inject.
    ChannelResult<IncomingEvent> InjectToClient(ByteSpan plain) {
        return Inject(Client, ClientInjector, plain, OpenScratch);
    }

    /// Seal `plain` with the client→agent key and deliver it to the agent.
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

    Channel Client;
    Channel Agent;

    // Persistent sealers: nonce counter must stay alive across multiple
    // injects so Channel::Opener sees a monotonically increasing counter.
    FrameSealer ClientInjector;  // seals agent→client frames for Client
    FrameSealer AgentInjector;   // seals client→agent frames for Agent

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

/// Build a well-formed GridBegin plaintext for the given parameters.
/// The caller controls each field directly (bypassing WriteGridBegin
/// validation) so we can produce illegal combinations for violation tests.
std::vector<std::byte> GridBeginRaw(std::uint8_t flags, std::uint32_t streamGen,
                                    std::uint32_t scrollbackLen,
                                    std::uint32_t totalLen) {
    std::vector<std::byte> plain(
        14);  // type + flags + gen + scrollback + total
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridBegin)));
    REQUIRE(w.WriteU8(flags));
    REQUIRE(w.WriteU32BE(streamGen));
    REQUIRE(w.WriteU32BE(scrollbackLen));
    REQUIRE(w.WriteU32BE(totalLen));
    return plain;
}

/// Build a well-formed GridChunk plaintext.
std::vector<std::byte> GridChunkRaw(std::uint32_t offset, ByteSpan data) {
    std::vector<std::byte> plain(1 + 4 + data.size());
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridChunk)));
    REQUIRE(w.WriteU32BE(offset));
    for (auto b : data) {
        REQUIRE(w.WriteU8(static_cast<std::uint8_t>(b)));
    }
    return plain;
}

/// Build a GridEnd plaintext.
std::vector<std::byte> GridEndRaw() {
    std::vector<std::byte> plain(1);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridEnd)));
    return plain;
}

/// Build a GridApplied plaintext.
std::vector<std::byte> GridAppliedRaw(std::uint32_t streamGen) {
    std::vector<std::byte> plain(5);
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::GridApplied)));
    REQUIRE(w.WriteU32BE(streamGen));
    return plain;
}

/// Build a Data plaintext carrying `length` payload bytes from offset 0,
/// ack=0.
std::vector<std::byte> DataRaw(std::uint32_t length) {
    std::vector<std::byte> plain(1 + 8 + 8 + length, std::byte{0xAB});
    ByteWriter w{MutableByteSpan{plain}};
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(EMessageType::Data)));
    REQUIRE(w.WriteU64BE(0));  // ack
    REQUIRE(w.WriteU64BE(0));  // offset
    return plain;
}

/// Deliver a complete, valid grid snapshot to `rig.Client` for `totalLen`
/// bytes, using `streamGen`.  The snapshot content is all-zero bytes.
/// Returns true and leaves the client ready to send GridApplied.
bool DeliverValidSnapshot(ViolationRig& rig, std::uint32_t totalLen,
                          std::uint32_t streamGen) {
    const auto begin = GridBeginRaw(0, streamGen, 0, totalLen);
    const auto ev1 = rig.InjectToClient(ByteSpan{begin});
    if (!ev1.has_value()) {
        return false;
    }

    // Send the payload in one chunk.
    std::vector<std::byte> payload(totalLen, std::byte{});
    const auto chunk = GridChunkRaw(0, ByteSpan{payload});
    const auto ev2 = rig.InjectToClient(ByteSpan{chunk});
    if (!ev2.has_value()) {
        return false;
    }

    const auto end = GridEndRaw();
    const auto ev3 = rig.InjectToClient(ByteSpan{end});
    return ev3.has_value();
}

}  // namespace

// ── Violation 1: GridBegin directed to the wrong role (agent) ─────────────
//
// The agent sends GridBegin; it must not also receive it.

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridBegin received by the agent closes the "
                  "connection") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 0, 16);
    const auto ev = rig.InjectToAgent(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 2: stream_gen ≤ current AgentStreamGen_ ────────────────────
//
// A GridBegin with stream_gen=0 can never be valid because the initial
// AgentStreamGen_ is 0 and the rule is strictly greater.

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridBegin stream_gen not strictly greater than "
                  "current closes the connection") {
    ViolationRig rig;
    // stream_gen=0 ≤ AgentStreamGen_=0 → violation.
    const auto plain = GridBeginRaw(0, 0, 0, 16);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 3: total_len > SNAPSHOT_MAX_BYTES ───────────────────────────
//
// ParseGridBegin rejects total_len > SNAPSHOT_MAX_BYTES as LengthLimitExceeded.

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridBegin total_len above limit closes the "
                  "connection") {
    ViolationRig rig;
    const std::uint32_t tooBig = SNAPSHOT_MAX_BYTES + 1;
    const auto plain = GridBeginRaw(0, 1, 0, tooBig);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 4: scrollback_len > total_len ───────────────────────────────
//
// ParseGridBegin rejects this as LengthLimitExceeded.

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridBegin scrollback_len exceeds total_len "
                  "closes the connection") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 100, 50);  // scrollback > total
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 5: second GridBegin before GridEnd ──────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: second GridBegin before GridEnd closes the "
                  "connection") {
    ViolationRig rig;
    const auto first = GridBeginRaw(0, 1, 0, 16);
    REQUIRE(rig.InjectToClient(ByteSpan{first}).has_value());

    // Another GridBegin with a higher stream_gen while the first is pending.
    const auto second = GridBeginRaw(0, 2, 0, 16);
    const auto ev = rig.InjectToClient(ByteSpan{second});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 6: GridChunk without preceding GridBegin ────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridChunk without preceding GridBegin closes "
                  "the connection") {
    ViolationRig rig;
    std::array<std::byte, 4> payload{};
    const auto plain = GridChunkRaw(0, ByteSpan{payload});
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 7: GridChunk offset does not match running total ────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridChunk with wrong offset closes the "
                  "connection") {
    ViolationRig rig;
    const auto begin = GridBeginRaw(0, 1, 0, 16);
    REQUIRE(rig.InjectToClient(ByteSpan{begin}).has_value());

    // ChunkOffset=5, but we have received 0 bytes so far.
    std::array<std::byte, 4> payload{};
    const auto chunk = GridChunkRaw(5, ByteSpan{payload});
    const auto ev = rig.InjectToClient(ByteSpan{chunk});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 8: GridChunk would exceed total_len ─────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridChunk exceeding total_len closes the "
                  "connection") {
    ViolationRig rig;
    const auto begin = GridBeginRaw(0, 1, 0, 4);
    REQUIRE(rig.InjectToClient(ByteSpan{begin}).has_value());

    // Total=4 but we're sending 8 bytes.
    std::array<std::byte, 8> payload{};
    const auto chunk = GridChunkRaw(0, ByteSpan{payload});
    const auto ev = rig.InjectToClient(ByteSpan{chunk});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 9: GridEnd when received_bytes ≠ total_len ─────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridEnd with incomplete data closes the "
                  "connection") {
    ViolationRig rig;
    // Announce 16 bytes but deliver none.
    const auto begin = GridBeginRaw(0, 1, 0, 16);
    REQUIRE(rig.InjectToClient(ByteSpan{begin}).has_value());

    const auto end = GridEndRaw();
    const auto ev = rig.InjectToClient(ByteSpan{end});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Violation 10: Data from the agent while GridReceiving is in progress ──

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: Data during grid reception window closes the "
                  "connection") {
    ViolationRig rig;
    const auto begin = GridBeginRaw(0, 1, 0, 16);
    REQUIRE(rig.InjectToClient(ByteSpan{begin}).has_value());

    const auto data = DataRaw(8);
    const auto ev = rig.InjectToClient(ByteSpan{data});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Extra: GridEnd without preceding GridBegin ────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridEnd without GridBegin closes the "
                  "connection") {
    ViolationRig rig;
    const auto end = GridEndRaw();
    const auto ev = rig.InjectToClient(ByteSpan{end});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Extra: GridApplied directed to the client ─────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "violation: GridApplied received by the client closes the "
                  "connection") {
    ViolationRig rig;
    const auto plain = GridAppliedRaw(1);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}

// ── Happy path sanity: full GridBegin → GridChunk → GridEnd round trip ────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a complete grid snapshot is delivered as DeliveredGridSnapshot") {
    ViolationRig rig;
    const std::uint32_t kTotal = 32;
    const bool ok = DeliverValidSnapshot(rig, kTotal, 1);
    REQUIRE(ok);
    // The test only verifies that no violation was triggered and that
    // RecvCursor was reset to 0 after delivery. The connection test covers
    // the full round-trip including GridApplied.
    CHECK(rig.ClientRecv.RecvOffset() == 0);
}

// ── Happy path: generation rollback guard is per-channel ─────────────────
//
// After stream_gen=1 is applied, a second snapshot with gen=2 succeeds;
// a repeat of gen=1 is rejected.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "second snapshot with higher gen succeeds; repeat gen fails") {
    ViolationRig rig;
    REQUIRE(DeliverValidSnapshot(rig, 8, 1));

    // gen=2: fine.
    REQUIRE(DeliverValidSnapshot(rig, 8, 2));

    // gen=1 again: generation rollback → violation.
    const auto plain = GridBeginRaw(0, 1, 0, 8);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    REQUIRE_FALSE(ev.has_value());
    CHECK(ev.error().Disposition() == EDisposition::CloseConnection);
}
