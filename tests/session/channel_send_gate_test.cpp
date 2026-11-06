/// The send gate (§9): a caller whose socket queue has no room for a frame
/// closes it, and the channel sends nothing until it reopens. Silence is still
/// judged, and no send timer wakes the caller for a send it cannot make.
///
/// One Channel on its own, time moved by hand.

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

TEST_CASE_FIXTURE(SodiumFixture, "a closed send gate sends nothing") {
    LonelyChannel lonely{KEEPALIVE};
    lonely.Chan.SetSendGate(false);

    const auto gated = lonely.Poll(LonelyChannel::START + KEEPALIVE);
    REQUIRE(gated.has_value());
    CHECK(*gated == 0);

    lonely.Chan.SetSendGate(true);
    const auto open = lonely.Poll(LonelyChannel::START + KEEPALIVE);
    REQUIRE(open.has_value());
    CHECK(*open > 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a closed send gate leaves the send timers out of the "
                  "deadline") {
    LonelyChannel lonely{KEEPALIVE};
    lonely.Chan.SetSendGate(false);
    CHECK(lonely.Chan.NextDeadline().At() == LonelyChannel::START + SILENCE);
}

TEST_CASE_FIXTURE(SodiumFixture, "a closed send gate still fails on silence") {
    LonelyChannel lonely{KEEPALIVE};
    lonely.Chan.SetSendGate(false);

    const auto silent = lonely.Poll(LonelyChannel::START + SILENCE);
    REQUIRE_FALSE(silent.has_value());
    CHECK(silent.error().Session() == ESessionError::PeerSilent);
}
