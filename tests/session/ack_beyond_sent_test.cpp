/// An Ack is checked against what was actually sent, not against what is
/// merely buffered (docs/design.md §7: an ack above what the sender sent is
/// CloseConnection). Bytes appended to the ring but not yet sealed into a Data
/// frame are unsent, and acknowledging them would drop bytes the peer never
/// received.
///
/// Covered: SendBuffer::Acknowledge and ResetSendNext (the resume offset of
/// the handshake), and both Channel roles through a bare Ack and through the
/// Ack field of Data. Analogue: the same AckBeyondSent reason the existing
/// "ack beyond end" refusal already raises.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/session/channel.hpp"
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

constexpr std::size_t BUFFERED = 10;
constexpr std::size_t SENT = 4;

/// One Channel of the given role whose own stream holds BUFFERED bytes, of
/// which SENT are sealed into Data frames and the rest are still waiting.
struct Side {
    explicit Side(ERole role)
        : Keys{DeriveConnectionKeys(
              DeriveSessionSecrets(MakeMaster(1)).ConnectionSeed, MakeNonce(1),
              MakeNonce(2))},
          Store(256),
          SendBuf{MutableByteSpan{Store}, 0},
          Recv{0},
          Chan{Keys,
               role,
               SendBuf,
               Recv,
               Timestamp{},
               MAX_FRAMES_PER_EPOCH,
               MAX_BYTES_PER_EPOCH,
               LivenessLimits{.KeepaliveInterval = Duration::zero(),
                              .PeerSilenceTimeout = Duration::zero()},
               wire::PROTOCOL_VERSION_MIN},
          PeerIsAgent{role == ERole::Client} {
        const std::vector<std::byte> data(BUFFERED, std::byte{0x41});
        REQUIRE(SendBuf.Append(ByteSpan{data}) == BUFFERED);
    }

    /// Frames from the peer, sealed with the peer's direction key.
    FrameSealer PeerSealer() const {
        return PeerIsAgent
                   ? FrameSealer{crypto::Key{Keys.ServerToClient.Expose()},
                                 Keys.SaltServerToClient}
                   : FrameSealer{crypto::Key{Keys.ClientToServer.Expose()},
                                 Keys.SaltClientToServer};
    }

    ChannelResult<IncomingEvent> Inject(ByteSpan plain) {
        auto sealer = PeerSealer();
        std::vector<std::byte> frame(wire::HEADER_SIZE + plain.size() +
                                     crypto::TAG_SIZE);
        const auto sealed = sealer.Seal(MutableByteSpan{frame}, plain);
        REQUIRE(sealed.has_value());
        const auto view =
            ReadFrame(ByteSpan{frame}.first(*sealed), wire::MAX_FRAME);
        REQUIRE(view.has_value());
        return Chan.Receive(Timestamp{}, *view, MutableByteSpan{OpenScratch});
    }

    ChannelResult<IncomingEvent> InjectAck(std::uint64_t ack) {
        std::array<std::byte, 1 + sizeof(std::uint64_t)> plain{};
        ByteWriter writer{MutableByteSpan{plain}};
        REQUIRE(wire::WriteAck(writer, wire::AckMessage{.Ack = ack}));
        return Inject(ByteSpan{plain});
    }

    ChannelResult<IncomingEvent> InjectDataWithAck(std::uint64_t ack) {
        std::vector<std::byte> plain(1 + 2 * sizeof(std::uint64_t) + 1,
                                     std::byte{0x5A});
        ByteWriter writer{MutableByteSpan{plain}};
        REQUIRE(writer.WriteU8(static_cast<std::uint8_t>(EMessageType::Data)));
        REQUIRE(writer.WriteU64BE(ack));
        REQUIRE(writer.WriteU64BE(0));
        return Inject(ByteSpan{plain});
    }

    ConnectionKeys Keys;
    std::vector<std::byte> Store;
    SendBuffer SendBuf;
    ReceiveCursor Recv;
    Channel Chan;
    bool PeerIsAgent;
    std::vector<std::byte> OpenScratch{wire::MAX_FRAME, std::byte{}};
};

/// Mark the first SENT buffered bytes as sent, the way Poll does when it
/// seals a Data frame.
void SealSome(Side& side) { side.SendBuf.AdvanceSendNext(SENT); }

}  // namespace

// SendBuffer: the rule itself, shared by both directions -------------------

TEST_CASE("Acknowledge: ack equal to sent is accepted") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    REQUIRE(buf.Acknowledge(SENT).has_value());
    CHECK(buf.Base() == SENT);
}

TEST_CASE("Acknowledge: ack one above sent closes, buffer untouched") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    const auto result = buf.Acknowledge(SENT + 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::AckBeyondSent);
    CHECK(buf.Base() == 0);
    CHECK(buf.Size() == BUFFERED);
}

TEST_CASE("Acknowledge: ack inside the buffered but unsent tail closes") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    for (const std::uint64_t ack : {SENT + 1, BUFFERED - 1, BUFFERED}) {
        CAPTURE(ack);
        const auto result = buf.Acknowledge(ack);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ESessionError::AckBeyondSent);
    }
    CHECK(buf.Base() == 0);
}

TEST_CASE("Acknowledge: nothing sent yet, any ack above zero closes") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);

    CHECK(buf.Acknowledge(0).has_value());
    const auto result = buf.Acknowledge(1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::AckBeyondSent);
}

// Resume across a reconnect -------------------------------------------------

TEST_CASE("ResetSendNext: resume offset up to what was sent is served") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    // The peer received all that was sent: resume exactly there.
    REQUIRE(buf.ResetSendNext(SENT).has_value());
    CHECK(buf.SendNext() == SENT);
    CHECK(buf.Base() == SENT);
}

TEST_CASE("ResetSendNext: resume offset into the unsent tail closes") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    const auto result = buf.ResetSendNext(SENT + 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::AckBeyondSent);
    CHECK(buf.Base() == 0);
    CHECK(buf.SendNext() == SENT);
}

TEST_CASE("after a rewind an ack up to the highest sealed offset is honest") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(8);

    // The peer holds 2: the new connection resends from 2.
    REQUIRE(buf.ResetSendNext(2).has_value());
    // Bytes 2..8 were sealed on the old connection and may have landed after
    // the peer quoted 2: acking them is not a lie, and the cursor follows.
    REQUIRE(buf.Acknowledge(5).has_value());
    CHECK(buf.Base() == 5);
    CHECK(buf.SendNext() == 5);
    // Above the highest sealed offset still is.
    CHECK_FALSE(buf.Acknowledge(9).has_value());
    // And a second resume is bounded by the same mark, not by the cursor.
    REQUIRE(buf.ResetSendNext(2 + 6).has_value());
}

TEST_CASE("Acknowledge clamps SendNext to the new Base") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(20, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == data.size());
    buf.AdvanceSendNext(15);
    // Rewound to 5: the peer held 5 when it resumed.
    REQUIRE(buf.ResetSendNext(5).has_value());

    // It now acks 15, sealed on the old connection: the cursor must not stay
    // below Base, where CopyOut has nothing to give.
    REQUIRE(buf.Acknowledge(15).has_value());
    CHECK(buf.Base() == 15);
    CHECK(buf.SendNext() == 15);
    CHECK(buf.SendNext() >= buf.Base());
}

TEST_CASE(
    "resend after an ack above the rewound cursor yields each byte once") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data(20);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data.at(i) = static_cast<std::byte>(i);
    }
    REQUIRE(buf.Append(ByteSpan{data}) == data.size());
    buf.AdvanceSendNext(12);
    REQUIRE(buf.ResetSendNext(3).has_value());
    REQUIRE(buf.Acknowledge(9).has_value());

    // What the peer holds is [0, 9); the resend continues at 9 and reaches End
    // without a hole or a repeat.
    std::vector<std::byte> resent;
    while (buf.Unsent() > 0) {
        std::array<std::byte, 4> chunk{};
        const auto out = buf.CopyOut(buf.SendNext(), MutableByteSpan{chunk});
        REQUIRE_FALSE(out.empty());
        resent.insert(resent.end(), out.begin(), out.end());
        buf.AdvanceSendNext(out.size());
    }
    CHECK(resent == std::vector<std::byte>(data.begin() + 9, data.end()));
    CHECK(buf.SendNext() == 20);
}

TEST_CASE("a new generation forgets what was sealed in the old one") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    const std::vector<std::byte> data(BUFFERED, std::byte{1});
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    buf.AdvanceSendNext(SENT);

    buf.NewGeneration();
    REQUIRE(buf.Append(ByteSpan{data}) == BUFFERED);
    // Nothing of the new generation is sealed: SENT would have been honest in
    // the old one, and is a lie now.
    const auto ack = buf.Acknowledge(SENT);
    REQUIRE_FALSE(ack.has_value());
    CHECK(ack.error() == ESessionError::AckBeyondSent);
    const auto resume = buf.ResetSendNext(SENT);
    REQUIRE_FALSE(resume.has_value());
    CHECK(resume.error() == ESessionError::AckBeyondSent);
}

// Channel: both roles, bare Ack and Ack inside Data -------------------------

TEST_CASE_FIXTURE(SodiumFixture, "channel: ack above sent closes the agent") {
    Side side{ERole::Agent};
    SealSome(side);

    SUBCASE("bare Ack") {
        const auto ev = side.InjectAck(SENT + 1);
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
        CHECK(side.SendBuf.Base() == 0);
    }
    SUBCASE("Ack inside Data") {
        const auto ev = side.InjectDataWithAck(BUFFERED);
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
        CHECK(side.Recv.RecvOffset() == 0);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "channel: ack above sent closes the client") {
    Side side{ERole::Client};
    SealSome(side);

    SUBCASE("bare Ack") {
        const auto ev = side.InjectAck(SENT + 1);
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
        CHECK(side.SendBuf.Base() == 0);
    }
    SUBCASE("Ack inside Data") {
        const auto ev = side.InjectDataWithAck(BUFFERED);
        REQUIRE_FALSE(ev.has_value());
        CHECK(ev.error().Session() == ESessionError::AckBeyondSent);
        CHECK(side.Recv.RecvOffset() == 0);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "channel: ack equal to sent is accepted") {
    for (const auto role : {ERole::Agent, ERole::Client}) {
        CAPTURE(static_cast<int>(role));
        Side side{role};
        SealSome(side);
        const auto ev = side.InjectAck(SENT);
        REQUIRE(ev.has_value());
        CHECK(side.SendBuf.Base() == SENT);
    }
}
