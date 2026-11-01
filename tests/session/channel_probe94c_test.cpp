// Channel-level verifier probes for PR 94.
//
// ViolationRig is defined inline here (same logic as grid_violation_test.cpp)
// rather than via #include to avoid duplicate TEST_CASE registrations.

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

// ── ViolationRig (local copy; mirrors grid_violation_test.cpp) ────────────

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
          Client{Keys,
                 ERole::Client,
                 ClientSendBuf,
                 ClientRecv,
                 Timestamp{},
                 MAX_FRAMES_PER_EPOCH,
                 MAX_BYTES_PER_EPOCH,
                 {},
                 2,
                 MutableByteSpan{ClientSnapshotStore}},
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

/// Build a GridBegin plaintext with a zero total_len (wire violation, §7).
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

}  // namespace

// ── PROBE7c: GridBegin with total_len == 0 is a wire violation
// ────────────────

TEST_CASE_FIXTURE(SodiumFixture, "PROBE7c GridBegin total_len==0 on the wire") {
    ViolationRig rig;
    const auto plain = GridBeginRaw(0, 1, 0, 0);
    const auto ev = rig.InjectToClient(ByteSpan{plain});
    MESSAGE("GridBegin total_len=0 -> has_value=" << ev.has_value());
    REQUIRE_FALSE(ev.has_value());
    MESSAGE("err proto=" << int(*ev.error().Proto()));
}

// ── PROBE7d: Channel::StartGrid(empty) is a no-op ────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE7d Channel::StartGrid(empty) behaviour") {
    ViolationRig rig;
    rig.Agent.SuppressDataUntilGrid();
    const std::vector<std::byte> junk(20, std::byte{1});
    std::ignore = rig.AgentSendBuf.Append(ByteSpan{junk});
    rig.Agent.StartGrid(ByteSpan{}, 0, 1, 0);
    MESSAGE("GridInFlight after empty StartGrid=" << rig.Agent.GridInFlight());
    std::array<std::byte, wire::HEADER_SIZE + wire::MAX_FRAME> out{};
    std::array<std::byte, Channel::POLL_SCRATCH_SIZE> scr{};
    auto n =
        rig.Agent.Poll(Timestamp{}, MutableByteSpan{out}, MutableByteSpan{scr});
    MESSAGE(
        "Poll n="
        << (n ? *n : 0)
        << " (data flows with no snapshot sent: empty StartGrid is a no-op)");
}
