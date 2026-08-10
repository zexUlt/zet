#include "zet/wire/key_schedule.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace {

using namespace zet;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

crypto::Key MakeMaster(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(seed + i);
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint8_t seed) {
    HandshakeNonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<std::byte>(seed * 3 + i);
    }
    return nonce;
}

std::vector<std::byte> Bytes(const crypto::Key& key) {
    return {key.Expose().begin(), key.Expose().end()};
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "the same master gives the same session secrets") {
    // Both ends run this independently and have to arrive at the same place:
    // the tree is derivation, not negotiation.
    const auto first = DeriveSessionSecrets(MakeMaster(1));
    const auto second = DeriveSessionSecrets(MakeMaster(1));

    CHECK(first.Id == second.Id);
    CHECK(Bytes(first.Auth) == Bytes(second.Auth));
    CHECK(Bytes(first.ConnectionSeed) == Bytes(second.ConnectionSeed));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the branches of the session tree are independent") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    // The identifier travels in the clear. If it shared bytes with either key,
    // publishing it would publish part of a secret.
    const std::vector<std::byte> id{secrets.Id.begin(), secrets.Id.end()};
    const auto auth = Bytes(secrets.Auth);
    const auto seed = Bytes(secrets.ConnectionSeed);

    CHECK(auth != seed);
    CHECK_FALSE(std::equal(id.begin(), id.end(), auth.begin()));
    CHECK_FALSE(std::equal(id.begin(), id.end(), seed.begin()));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a different master gives a different session") {
    const auto mine = DeriveSessionSecrets(MakeMaster(1));
    const auto theirs = DeriveSessionSecrets(MakeMaster(2));

    CHECK(mine.Id != theirs.Id);
    CHECK(Bytes(mine.Auth) != Bytes(theirs.Auth));
    CHECK(Bytes(mine.ConnectionSeed) != Bytes(theirs.ConnectionSeed));
}

TEST_CASE_FIXTURE(SodiumFixture, "both ends derive the same connection keys") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));
    const auto client = MakeNonce(1);
    const auto server = MakeNonce(2);

    const auto here =
        DeriveConnectionKeys(secrets.ConnectionSeed, client, server);
    const auto there =
        DeriveConnectionKeys(secrets.ConnectionSeed, client, server);

    CHECK(Bytes(here.ClientToServer) == Bytes(there.ClientToServer));
    CHECK(Bytes(here.ServerToClient) == Bytes(there.ServerToClient));
    CHECK(here.SaltClientToServer == there.SaltClientToServer);
    CHECK(here.SaltServerToClient == there.SaltServerToClient);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the two directions never share a key or a salt") {
    // Sharing either would make a frame sent one way openable when reflected
    // back the other.
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));
    const auto keys = DeriveConnectionKeys(secrets.ConnectionSeed, MakeNonce(1),
                                           MakeNonce(2));

    CHECK(Bytes(keys.ClientToServer) != Bytes(keys.ServerToClient));
    CHECK(keys.SaltClientToServer != keys.SaltServerToClient);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "every connection of a session gets its own keys") {
    // This is what makes a reconnect safe: fresh keys and a counter from zero,
    // so no nonce is ever reused across connections.
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    std::set<std::vector<std::byte>> seen;
    crypto::Key seed{secrets.ConnectionSeed.Expose()};
    for (int connection = 0; connection < 50; ++connection) {
        const auto keys =
            DeriveConnectionKeys(seed, MakeNonce(1), MakeNonce(2));
        seen.insert(Bytes(keys.ClientToServer));
        seed = AdvanceConnectionSeed(seed);
    }

    // Identical nonces every time, so nothing but the ratcheted seed kept the
    // fifty connections apart.
    CHECK(seen.size() == 50);
}

TEST_CASE_FIXTURE(SodiumFixture, "the ratchet does not run backwards") {
    // The point of advancing at all: whoever reads the seed of connection n+1
    // out of memory must not be able to reach connection n, whose nonces they
    // already have from the wire.
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    crypto::Key first{secrets.ConnectionSeed.Expose()};
    const auto second = AdvanceConnectionSeed(first);
    const auto third = AdvanceConnectionSeed(second);

    CHECK(Bytes(first) != Bytes(second));
    CHECK(Bytes(second) != Bytes(third));

    // Derivation only goes one way, so the seeds ahead say nothing about the
    // keys behind them.
    const auto behind = DeriveConnectionKeys(first, MakeNonce(1), MakeNonce(2));
    const auto ahead = DeriveConnectionKeys(second, MakeNonce(1), MakeNonce(2));
    CHECK(Bytes(behind.ClientToServer) != Bytes(ahead.ClientToServer));
}

TEST_CASE_FIXTURE(SodiumFixture, "the ratchet is the same on both ends") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    crypto::Key here{secrets.ConnectionSeed.Expose()};
    crypto::Key there{secrets.ConnectionSeed.Expose()};

    CHECK(Bytes(AdvanceConnectionSeed(here)) ==
          Bytes(AdvanceConnectionSeed(there)));
}

TEST_CASE_FIXTURE(SodiumFixture, "either nonce changes the connection keys") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    const auto base = DeriveConnectionKeys(secrets.ConnectionSeed, MakeNonce(1),
                                           MakeNonce(2));
    const auto otherClient = DeriveConnectionKeys(secrets.ConnectionSeed,
                                                  MakeNonce(9), MakeNonce(2));
    const auto otherServer = DeriveConnectionKeys(secrets.ConnectionSeed,
                                                  MakeNonce(1), MakeNonce(9));

    // Neither side alone decides the key, so a peer that replays its own nonce
    // cannot force a repeat of a previous connection.
    CHECK(Bytes(base.ClientToServer) != Bytes(otherClient.ClientToServer));
    CHECK(Bytes(base.ClientToServer) != Bytes(otherServer.ClientToServer));
}

TEST_CASE_FIXTURE(SodiumFixture, "swapping the nonces gives different keys") {
    // The two nonces are concatenated in a fixed order. If they were combined
    // symmetrically, a peer could hand back what it received and land on the
    // same key.
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));

    const auto forward = DeriveConnectionKeys(secrets.ConnectionSeed,
                                              MakeNonce(1), MakeNonce(2));
    const auto swapped = DeriveConnectionKeys(secrets.ConnectionSeed,
                                              MakeNonce(2), MakeNonce(1));

    CHECK(Bytes(forward.ClientToServer) != Bytes(swapped.ClientToServer));
}

TEST_CASE_FIXTURE(SodiumFixture, "connection keys say nothing about the seed") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));
    const auto keys = DeriveConnectionKeys(secrets.ConnectionSeed, MakeNonce(1),
                                           MakeNonce(2));

    CHECK(Bytes(keys.ClientToServer) != Bytes(secrets.ConnectionSeed));
    CHECK(Bytes(keys.ServerToClient) != Bytes(secrets.ConnectionSeed));
    CHECK(Bytes(keys.ClientToServer) != Bytes(secrets.Auth));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the derived salt is the size the sealer wants") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));
    const auto keys = DeriveConnectionKeys(secrets.ConnectionSeed, MakeNonce(1),
                                           MakeNonce(2));

    // The salt fills everything in the nonce that the counter does not.
    CHECK(keys.SaltClientToServer.size() == SALT_SIZE);
    CHECK(SALT_SIZE + sizeof(std::uint64_t) == crypto::NONCE_SIZE);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the keys go straight into a sealer and open") {
    const auto secrets = DeriveSessionSecrets(MakeMaster(1));
    const auto keys = DeriveConnectionKeys(secrets.ConnectionSeed, MakeNonce(1),
                                           MakeNonce(2));

    crypto::Key sealerKey{keys.ClientToServer.Expose()};
    crypto::Key openerKey{keys.ClientToServer.Expose()};
    FrameSealer sealer{std::move(sealerKey), keys.SaltClientToServer};
    FrameOpener opener{std::move(openerKey), keys.SaltClientToServer};

    const std::array<std::byte, 4> plaintext{std::byte{'p'}, std::byte{'i'},
                                             std::byte{'n'}, std::byte{'g'}};
    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> out(plaintext.size());
    const auto opened = opener.Open(MutableByteSpan{out}, *frame);
    REQUIRE(opened.has_value());
    CHECK(std::equal(plaintext.begin(), plaintext.end(), opened->begin()));
}
