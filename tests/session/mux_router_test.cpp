/// The routing decisions of zet-muxd, sans-I/O (design.md §6).

#include "zet/session/mux_router.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <variant>
#include <vector>

#include "zet/core/byte_writer.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace std::chrono_literals;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// The router and the storage it borrows, off the stack.
struct Rig {
    std::vector<std::byte> Storage =
        std::vector<std::byte>(MUX_PENDING_STORAGE);
    std::unique_ptr<MuxRouter> Router =
        std::make_unique<MuxRouter>(MutableByteSpan{Storage});
};

crypto::Key Registration(std::uint8_t seed) {
    crypto::Key key;
    for (std::size_t i = 0; i < key.Expose().size(); ++i) {
        key.Expose()[i] = static_cast<std::byte>(seed * 3 + i);
    }
    return key;
}

PeerAddress Address(std::uint8_t last) {
    PeerAddress address{};
    address.back() = static_cast<std::byte>(last);
    return address;
}

/// A client's Hello for `id`, framed as it arrives on the wire.
std::vector<std::byte> HelloFor(const wire::SessionId& id) {
    wire::HelloMessage hello{};
    hello.Id = id;
    std::vector<std::byte> body(wire::MAX_HANDSHAKE_FRAME);
    ByteWriter bodyWriter{MutableByteSpan{body}};
    REQUIRE(wire::WriteHello(bodyWriter, hello));
    body.resize(bodyWriter.Size());
    std::vector<std::byte> frame(wire::HEADER_SIZE + body.size());
    ByteWriter writer{MutableByteSpan{frame}};
    REQUIRE(wire::WriteFrameHeader(
        writer,
        wire::FrameHeader{.Length = static_cast<std::uint32_t>(body.size()),
                          .Epoch = 0}));
    REQUIRE(writer.WriteBytes(ByteSpan{body}));
    return frame;
}

constexpr Timestamp START = Timestamp::FromNanos(1'000'000);
constexpr Uid ALICE = 1000;
constexpr Uid MALLORY = 1001;

}  // namespace

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a Hello for a registered sid goes to its agent, prefix and all") {
    Rig rig;
    const auto r = Registration(1);
    REQUIRE(rig.Router->Register(7, ALICE, r));
    CHECK(rig.Router->Registrations() == 1);
    CHECK(rig.Router->Find(wire::SessionIdOf(r)) == MuxAgentId{7});

    REQUIRE(rig.Router->Accept(100, Address(1), START));
    CHECK(rig.Router->Pending() == 1);
    auto bytes = HelloFor(wire::SessionIdOf(r));
    const auto hello = bytes;
    // A byte the client sent after its Hello still belongs to the agent.
    bytes.push_back(std::byte{0x5A});
    const MuxDecision decision = rig.Router->Feed(100, ByteSpan{bytes});
    const auto* handoff = std::get_if<MuxHandoff>(&decision);
    REQUIRE(handoff != nullptr);
    CHECK(handoff->Connection == 100);
    CHECK(handoff->Agent == 7);
    CHECK(std::ranges::equal(handoff->Prefix, bytes));
    CHECK(handoff->Prefix.size() == hello.size() + 1);
    CHECK(rig.Router->Pending() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "a Hello for an unknown sid gets the decoy") {
    Rig rig;
    REQUIRE(rig.Router->Register(7, ALICE, Registration(1)));
    REQUIRE(rig.Router->Accept(100, Address(1), START));
    const auto bytes = HelloFor(wire::SessionIdOf(Registration(2)));
    const MuxDecision decision = rig.Router->Feed(100, ByteSpan{bytes});
    const auto* decoy = std::get_if<MuxDecoy>(&decision);
    REQUIRE(decoy != nullptr);
    CHECK(std::ranges::equal(decoy->Prefix, bytes));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a sid read off the wire claims nothing without R") {
    // What an eavesdropper has is the sid; registering means showing an R
    // that hashes to it.
    Rig rig;
    const auto r = Registration(1);
    const wire::SessionId sid = wire::SessionIdOf(r);
    crypto::Key guess;
    std::ranges::copy(sid, guess.Expose().begin());
    REQUIRE(rig.Router->Register(9, MALLORY, guess));
    CHECK_FALSE(rig.Router->Find(sid).has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "another uid cannot take a live sid; the same uid moves it") {
    Rig rig;
    REQUIRE(rig.Router->Register(7, ALICE, Registration(1)));
    CHECK(rig.Router->Register(9, MALLORY, Registration(1)).error() ==
          wire::EMuxRefusal::Taken);
    CHECK(rig.Router->Find(wire::SessionIdOf(Registration(1))) ==
          MuxAgentId{7});

    // Alice's agent reconnected before the mux saw its old link go.
    REQUIRE(rig.Router->Register(8, ALICE, Registration(1)));
    CHECK(rig.Router->Find(wire::SessionIdOf(Registration(1))) ==
          MuxAgentId{8});
    CHECK(rig.Router->Registrations() == 1);

    // Once it is gone, the sid is free for whoever proves R.
    rig.Router->Unregister(8);
    CHECK(rig.Router->Registrations() == 0);
    CHECK(rig.Router->Register(9, MALLORY, Registration(1)).has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "unregistering an agent frees every sid it held") {
    Rig rig;
    REQUIRE(rig.Router->Register(7, ALICE, Registration(1)));
    REQUIRE(rig.Router->Register(7, ALICE, Registration(2)));
    REQUIRE(rig.Router->Register(8, ALICE, Registration(3)));
    rig.Router->Unregister(7);
    CHECK(rig.Router->Registrations() == 1);
    CHECK_FALSE(rig.Router->Find(wire::SessionIdOf(Registration(1))));
    CHECK(rig.Router->Find(wire::SessionIdOf(Registration(3))) ==
          MuxAgentId{8});
}

TEST_CASE_FIXTURE(SodiumFixture, "one uid cannot fill the table") {
    Rig rig;
    for (std::size_t i = 0; i < MAX_REGISTRATIONS_PER_UID; ++i) {
        REQUIRE(rig.Router->Register(
            7, ALICE, Registration(static_cast<std::uint8_t>(i))));
    }
    CHECK(rig.Router->Register(7, ALICE, Registration(200)).error() ==
          wire::EMuxRefusal::Limit);
    CHECK(rig.Router->Register(9, MALLORY, Registration(200)).has_value());
}

TEST_CASE_FIXTURE(SodiumFixture, "a full table refuses everybody") {
    Rig rig;
    std::size_t made = 0;
    for (Uid uid = 0; made < MAX_MUX_REGISTRATIONS; ++uid) {
        for (std::size_t i = 0;
             i < MAX_REGISTRATIONS_PER_UID && made < MAX_MUX_REGISTRATIONS;
             ++i, ++made) {
            crypto::Key key;
            ByteWriter writer{MutableByteSpan{key.Expose()}};
            REQUIRE(writer.WriteU32BE(uid));
            REQUIRE(writer.WriteU32BE(static_cast<std::uint32_t>(i)));
            REQUIRE(rig.Router->Register(uid, uid, key));
        }
    }
    CHECK(rig.Router->Registrations() == MAX_MUX_REGISTRATIONS);
    CHECK(rig.Router->Register(1, 999'999, Registration(1)).error() ==
          wire::EMuxRefusal::Limit);
}

TEST_CASE_FIXTURE(SodiumFixture, "a Hello may come in pieces") {
    Rig rig;
    REQUIRE(rig.Router->Register(7, ALICE, Registration(1)));
    REQUIRE(rig.Router->Accept(100, Address(1), START));
    const auto bytes = HelloFor(wire::SessionIdOf(Registration(1)));
    CHECK(rig.Router->Room(100) == MUX_PENDING_BUFFER);
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        CAPTURE(i);
        CHECK(std::holds_alternative<MuxNeedMore>(
            rig.Router->Feed(100, ByteSpan{bytes}.subspan(i, 1))));
    }
    CHECK(rig.Router->Room(100) == MUX_PENDING_BUFFER - (bytes.size() - 1));
    const MuxDecision last = rig.Router->Feed(100, ByteSpan{bytes}.last(1));
    const auto* handoff = std::get_if<MuxHandoff>(&last);
    REQUIRE(handoff != nullptr);
    CHECK(std::ranges::equal(handoff->Prefix, bytes));
}

TEST_CASE_FIXTURE(SodiumFixture, "what is not a Hello is closed") {
    Rig rig;
    REQUIRE(rig.Router->Accept(100, Address(1), START));
    SUBCASE("garbage") {
        const std::vector<std::byte> junk(16, std::byte{0xFF});
        const MuxDecision decision = rig.Router->Feed(100, ByteSpan{junk});
        const auto* close = std::get_if<MuxClose>(&decision);
        REQUIRE(close != nullptr);
        CHECK(close->Reason == EMuxClose::Malformed);
    }
    SUBCASE("a frame longer than a handshake") {
        std::vector<std::byte> header(wire::HEADER_SIZE);
        ByteWriter writer{MutableByteSpan{header}};
        REQUIRE(wire::WriteFrameHeader(
            writer, wire::FrameHeader{.Length = wire::MAX_HANDSHAKE_FRAME + 1,
                                      .Epoch = 0}));
        const MuxDecision decision = rig.Router->Feed(100, ByteSpan{header});
        CHECK(std::get_if<MuxClose>(&decision) != nullptr);
    }
    SUBCASE("more than a buffer, still incomplete") {
        // A header claiming the most a handshake frame may be, then bytes:
        // the room fills before the frame does, and the rest is refused.
        std::vector<std::byte> header(wire::HEADER_SIZE);
        ByteWriter writer{MutableByteSpan{header}};
        REQUIRE(wire::WriteFrameHeader(
            writer, wire::FrameHeader{.Length = wire::MAX_HANDSHAKE_FRAME,
                                      .Epoch = 0}));
        std::vector<std::byte> stream = header;
        stream.push_back(std::byte{0x01});  // Hello's type
        stream.resize(MUX_PENDING_BUFFER + 1, std::byte{0});
        const MuxDecision decision = rig.Router->Feed(100, ByteSpan{stream});
        const auto* close = std::get_if<MuxClose>(&decision);
        REQUIRE(close != nullptr);
        CHECK(close->Reason == EMuxClose::Malformed);
    }
    CHECK(rig.Router->Pending() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "pending connections are bounded") {
    Rig rig;
    for (std::size_t i = 0; i < MAX_PENDING_PER_ADDRESS; ++i) {
        REQUIRE(rig.Router->Accept(i, Address(1), START));
    }
    CHECK(rig.Router->Accept(999, Address(1), START).error() ==
          EMuxClose::Limit);
    // Another address has its own share, until the whole table is full.
    std::size_t id = MAX_PENDING_PER_ADDRESS;
    for (std::uint8_t peer = 2; rig.Router->Pending() < MAX_PENDING_CONNECTIONS;
         ++peer) {
        for (std::size_t i = 0; i < MAX_PENDING_PER_ADDRESS &&
                                rig.Router->Pending() < MAX_PENDING_CONNECTIONS;
             ++i) {
            REQUIRE(rig.Router->Accept(id++, Address(peer), START));
        }
    }
    CHECK(rig.Router->Accept(id, Address(250), START).error() ==
          EMuxClose::Limit);
    // And the same number twice is the caller's mistake.
    CHECK(rig.Router->Accept(0, Address(3), START).error() ==
          EMuxClose::Unknown);
}

TEST_CASE_FIXTURE(SodiumFixture, "a connection that never says Hello expires") {
    Rig rig;
    CHECK_FALSE(rig.Router->NextDeadline().IsSet());
    REQUIRE(rig.Router->Accept(100, Address(1), START));
    REQUIRE(rig.Router->Accept(101, Address(1), START + 1s));
    CHECK(rig.Router->NextDeadline().At() == START + HANDSHAKE_TIMEOUT);

    CHECK_FALSE(rig.Router->Expire(START + HANDSHAKE_TIMEOUT - 1ns));
    const auto first = rig.Router->Expire(START + HANDSHAKE_TIMEOUT);
    REQUIRE(first.has_value());
    CHECK(first->Connection == 100);
    CHECK(first->Reason == EMuxClose::Expired);
    CHECK_FALSE(rig.Router->Expire(START + HANDSHAKE_TIMEOUT));
    CHECK(rig.Router->NextDeadline().At() == START + 1s + HANDSHAKE_TIMEOUT);
    CHECK(rig.Router->Pending() == 1);
}

TEST_CASE_FIXTURE(SodiumFixture, "a closed or unknown connection is let go") {
    Rig rig;
    REQUIRE(rig.Router->Accept(100, Address(1), START));
    rig.Router->Closed(100);
    CHECK(rig.Router->Pending() == 0);
    CHECK(rig.Router->Room(100) == 0);
    rig.Router->Closed(100);
    const std::vector<std::byte> bytes{std::byte{0}};
    const MuxDecision decision = rig.Router->Feed(100, ByteSpan{bytes});
    const auto* close = std::get_if<MuxClose>(&decision);
    REQUIRE(close != nullptr);
    CHECK(close->Reason == EMuxClose::Unknown);
}

TEST_CASE_FIXTURE(SodiumFixture, "less storage is fewer slots, never more") {
    std::vector<std::byte> storage(2 * MUX_PENDING_BUFFER + 1);
    auto router = std::make_unique<MuxRouter>(MutableByteSpan{storage});
    REQUIRE(router->Accept(1, Address(1), START));
    REQUIRE(router->Accept(2, Address(2), START));
    CHECK(router->Accept(3, Address(3), START).error() == EMuxClose::Limit);
}
