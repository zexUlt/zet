/// The read gate (§9): a caller that stops reading the socket because its own
/// consumer is full must not be told the peer went silent.
///
/// One Channel on its own, time moved by hand. Nothing arrives, so the only
/// thing under test is when the silence timer is allowed to fire.

#include "zet/session/channel.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <vector>

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

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;

constexpr Duration SILENCE = 20s;
constexpr Duration KEEPALIVE = 5s;

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

wire::HandshakeNonce MakeNonce(std::uint8_t seed) {
    wire::HandshakeNonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] =
            static_cast<std::byte>(seed * 7 + static_cast<std::uint8_t>(i));
    }
    return nonce;
}

/// A client channel that nothing ever answers.
struct LonelyChannel {
    explicit LonelyChannel(Duration keepalive)
        : Keys{wire::DeriveConnectionKeys(
              wire::DeriveSessionSecrets(MakeMaster(1)).ConnectionSeed,
              MakeNonce(1), MakeNonce(2))},
          Store(4096),
          SendBuf{MutableByteSpan{Store}, 0},
          Recv{0},
          Chan{Keys,
               ERole::Client,
               SendBuf,
               Recv,
               START,
               wire::MAX_FRAMES_PER_EPOCH,
               wire::MAX_BYTES_PER_EPOCH,
               LivenessLimits{.KeepaliveInterval = keepalive,
                              .PeerSilenceTimeout = SILENCE}} {}

    /// Poll at `now`; the result is the caller's to inspect.
    ChannelResult<std::size_t> Poll(Timestamp now) {
        return Chan.Poll(now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
    }

    static constexpr Timestamp START{};

    wire::ConnectionKeys Keys;
    std::vector<std::byte> Store;
    SendBuffer SendBuf;
    ReceiveCursor Recv;
    Channel Chan;
    std::vector<std::byte> Out{wire::HEADER_SIZE + wire::MAX_FRAME};
    std::vector<std::byte> Scratch{Channel::POLL_SCRATCH_SIZE};
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a closed read gate keeps the silence timer from firing") {
    LonelyChannel lonely{Duration::zero()};
    lonely.Chan.SetReadGate(false, LonelyChannel::START);

    const Timestamp late = LonelyChannel::START + 3 * SILENCE;
    CHECK(lonely.Poll(late).has_value());
    // The silence limit is out of the deadline too, so no timer wakes the
    // loop for nothing: what is left is the rekey.
    CHECK(lonely.Chan.NextDeadline().At() ==
          LonelyChannel::START + REKEY_INTERVAL);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "reopening the read gate restarts the silence timer") {
    LonelyChannel lonely{Duration::zero()};
    lonely.Chan.SetReadGate(false, LonelyChannel::START);
    const Timestamp reopened = LonelyChannel::START + 3 * SILENCE;
    lonely.Chan.SetReadGate(true, reopened);

    CHECK(lonely.Chan.NextDeadline().At() == reopened + SILENCE);
    CHECK(lonely.Poll(reopened).has_value());
    CHECK(lonely.Poll(reopened + SILENCE - 1ns).has_value());
    const auto silent = lonely.Poll(reopened + SILENCE);
    REQUIRE_FALSE(silent.has_value());
    CHECK(silent.error().Session() == ESessionError::PeerSilent);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an open gate told it is open again changes nothing") {
    LonelyChannel lonely{Duration::zero()};
    lonely.Chan.SetReadGate(true, LonelyChannel::START + SILENCE - 1s);

    const auto silent = lonely.Poll(LonelyChannel::START + SILENCE);
    REQUIRE_FALSE(silent.has_value());
    CHECK(silent.error().Session() == ESessionError::PeerSilent);
}

TEST_CASE_FIXTURE(SodiumFixture, "closing the gate twice keeps it closed") {
    LonelyChannel lonely{Duration::zero()};
    lonely.Chan.SetReadGate(false, LonelyChannel::START);
    lonely.Chan.SetReadGate(false, LonelyChannel::START + SILENCE);
    CHECK(lonely.Poll(LonelyChannel::START + 3 * SILENCE).has_value());
}

TEST_CASE_FIXTURE(SodiumFixture, "keepalives go on while the gate is closed") {
    LonelyChannel lonely{KEEPALIVE};
    lonely.Chan.SetReadGate(false, LonelyChannel::START);

    const auto ping = lonely.Poll(LonelyChannel::START + KEEPALIVE);
    REQUIRE(ping.has_value());
    CHECK(*ping > 0);
    CHECK(lonely.Poll(LonelyChannel::START + 3 * SILENCE).has_value());
}
