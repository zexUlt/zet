#include "zet/wire/sealed_frame.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

namespace {

using namespace zet;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(seed + i);
    }
    return crypto::Key{bytes};
}

DirectionSalt MakeSalt(std::uint8_t seed) {
    DirectionSalt salt{};
    for (std::size_t i = 0; i < salt.size(); ++i) {
        salt[i] = static_cast<std::byte>(seed * 2 + i);
    }
    return salt;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

/// Both ends of one direction: same key, same salt, counters starting together.
struct Pair {
    FrameSealer Sealer{MakeKey(1), MakeSalt(1)};
    FrameOpener Opener{MakeKey(1), MakeSalt(1)};
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture, "a sealed frame opens back to the plaintext") {
    Pair pair;
    const auto plaintext = Bytes("hello");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    const auto sealed =
        pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext});
    REQUIRE(sealed.has_value());
    CHECK(*sealed == wire.size());

    const auto frame = ReadFrame(ByteSpan{wire}.first(*sealed), MAX_FRAME);
    REQUIRE(frame.has_value());
    CHECK(frame->Header.Length == plaintext.size() + crypto::TAG_SIZE);

    // A round trip alone would stay green if sealing and opening both degraded
    // into a copy, which is the shape of ET's entire crypto test. So: the body
    // is longer than the plaintext by exactly a tag, and none of the plaintext
    // shows through it.
    const auto body = frame->Body;
    REQUIRE(body.size() == plaintext.size() + crypto::TAG_SIZE);
    CHECK_FALSE(std::equal(plaintext.begin(), plaintext.end(), body.begin()));

    std::vector<std::byte> out(plaintext.size());
    const auto opened = pair.Opener.Open(MutableByteSpan{out}, *frame);
    REQUIRE(opened.has_value());
    CHECK(std::vector<std::byte>(opened->begin(), opened->end()) == plaintext);

    // And the plaintext came out of the opener, not out of a buffer that was
    // never written: a no-op Open would leave the zeros it started with.
    CHECK(out != std::vector<std::byte>(plaintext.size(), std::byte{0}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the same plaintext seals differently every time") {
    // The counter is what makes this true, and it is the property that keeps a
    // nonce from repeating under one key.
    Pair pair;
    const auto plaintext = Bytes("repeat");

    std::vector<std::byte> first(HEADER_SIZE + plaintext.size() +
                                 crypto::TAG_SIZE);
    std::vector<std::byte> second(first.size());

    REQUIRE(pair.Sealer.Seal(MutableByteSpan{first}, ByteSpan{plaintext})
                .has_value());
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{second}, ByteSpan{plaintext})
                .has_value());

    CHECK(first != second);
    CHECK(pair.Sealer.Counter() == 2);
}

TEST_CASE_FIXTURE(SodiumFixture, "a long run of frames never repeats a nonce") {
    Pair pair;
    const auto plaintext = Bytes("x");
    std::set<std::vector<std::byte>> bodies;

    for (int i = 0; i < 2000; ++i) {
        std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                    crypto::TAG_SIZE);
        REQUIRE(pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext})
                    .has_value());
        bodies.insert(
            std::vector<std::byte>(wire.begin() + HEADER_SIZE, wire.end()));
    }

    // Identical plaintext under a repeated nonce would seal identically, so a
    // collision here is exactly the failure this checks for.
    CHECK(bodies.size() == 2000);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "frames must be opened in the order they were sealed") {
    Pair pair;
    const auto first = Bytes("first");
    const auto second = Bytes("second");

    std::vector<std::byte> wireFirst(HEADER_SIZE + first.size() +
                                     crypto::TAG_SIZE);
    std::vector<std::byte> wireSecond(HEADER_SIZE + second.size() +
                                      crypto::TAG_SIZE);
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wireFirst}, ByteSpan{first})
                .has_value());
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wireSecond}, ByteSpan{second})
                .has_value());

    const auto frameSecond = ReadFrame(ByteSpan{wireSecond}, MAX_FRAME);
    REQUIRE(frameSecond.has_value());

    // The counter is implicit, so the second frame does not open first: a peer
    // out of step fails the tag rather than decrypting something plausible.
    std::vector<std::byte> out(second.size());
    const auto opened = pair.Opener.Open(MutableByteSpan{out}, *frameSecond);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == EProtoError::AuthenticationFailed);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a rejected frame does not move the receiver's counter") {
    // Without this an injected byte would desynchronise the stream for good,
    // which is a denial of service anyone on the path could mount.
    Pair pair;
    const auto plaintext = Bytes("payload");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext})
                .has_value());

    auto damaged = wire;
    damaged[HEADER_SIZE + 2] ^= std::byte{0x40};
    const auto damagedFrame = ReadFrame(ByteSpan{damaged}, MAX_FRAME);
    REQUIRE(damagedFrame.has_value());

    std::vector<std::byte> out(plaintext.size());
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto opened =
            pair.Opener.Open(MutableByteSpan{out}, *damagedFrame);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == EProtoError::AuthenticationFailed);
    }
    CHECK(pair.Opener.Counter() == 0);

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());
    const auto opened = pair.Opener.Open(MutableByteSpan{out}, *frame);
    REQUIRE(opened.has_value());
    CHECK(std::vector<std::byte>(opened->begin(), opened->end()) == plaintext);
}

TEST_CASE_FIXTURE(SodiumFixture, "tampering with the header fails the open") {
    Pair pair;
    const auto plaintext = Bytes("in the clear, but signed");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext})
                .has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> out(plaintext.size());

    SUBCASE("the epoch") {
        FrameView forged = *frame;
        forged.Header.Epoch ^= 0x01;
        CHECK(pair.Opener.Open(MutableByteSpan{out}, forged).error() ==
              EProtoError::AuthenticationFailed);
    }

    SUBCASE("the declared length") {
        FrameView forged = *frame;
        forged.Header.Length -= 1;
        forged.Body = forged.Body.first(forged.Body.size() - 1);
        CHECK(pair.Opener.Open(MutableByteSpan{out}, forged).error() ==
              EProtoError::AuthenticationFailed);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "a different key does not open the frame") {
    FrameSealer sealer{MakeKey(1), MakeSalt(1)};
    FrameOpener stranger{MakeKey(9), MakeSalt(1)};
    const auto plaintext = Bytes("not for you");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> out(plaintext.size());
    CHECK(stranger.Open(MutableByteSpan{out}, *frame).error() ==
          EProtoError::AuthenticationFailed);
}

TEST_CASE_FIXTURE(SodiumFixture, "the salt separates the two directions") {
    // Same key, opposite directions. Without a per-direction salt a frame sent
    // one way would open when reflected back the other.
    FrameSealer sealer{MakeKey(1), MakeSalt(1)};
    FrameOpener otherDirection{MakeKey(1), MakeSalt(2)};
    const auto plaintext = Bytes("reflected");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> out(plaintext.size());
    CHECK(otherDirection.Open(MutableByteSpan{out}, *frame).error() ==
          EProtoError::AuthenticationFailed);
}

TEST_CASE_FIXTURE(SodiumFixture, "an empty payload still seals and opens") {
    Pair pair;

    std::vector<std::byte> wire(HEADER_SIZE + crypto::TAG_SIZE);
    const auto sealed = pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{});
    REQUIRE(sealed.has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());
    CHECK(frame->Header.Length == crypto::TAG_SIZE);

    std::vector<std::byte> out(1);
    const auto opened = pair.Opener.Open(MutableByteSpan{out}, *frame);
    REQUIRE(opened.has_value());
    CHECK(opened->empty());
}

TEST_CASE_FIXTURE(SodiumFixture, "a payload past MAX_FRAME is refused") {
    Pair pair;
    const std::vector<std::byte> oversized(MAX_FRAME, std::byte{0});
    std::vector<std::byte> out(HEADER_SIZE + oversized.size() +
                               crypto::TAG_SIZE);

    CHECK(pair.Sealer.Seal(MutableByteSpan{out}, ByteSpan{oversized}).error() ==
          EProtoError::LengthLimitExceeded);
}

TEST_CASE_FIXTURE(SodiumFixture, "sealing into too small a buffer is refused") {
    Pair pair;
    const auto plaintext = Bytes("needs room for the tag");
    std::vector<std::byte> tooSmall(HEADER_SIZE + plaintext.size());

    CHECK(pair.Sealer.Seal(MutableByteSpan{tooSmall}, ByteSpan{plaintext})
              .error() == EProtoError::BufferTooSmall);
    // The refusal happens before the counter moves, so nothing is lost.
    CHECK(pair.Sealer.Counter() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a key that has sealed its limit refuses to seal again") {
    // The real limit is sixteen million frames, so the test hands in a small
    // one: what is being checked is the refusal, not the size of the number.
    FrameSealer sealer{MakeKey(1), MakeSalt(1), 2};
    const auto plaintext = Bytes("ok");
    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);

    REQUIRE(
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).has_value());
    REQUIRE(
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).has_value());

    const auto exhausted =
        sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext});
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error() == EProtoError::RekeyRequired);
    CHECK(sealer.Counter() == 2);

    // Nothing about the message was wrong, so the refusal has to be stable
    // rather than something a retry gets past.
    CHECK(sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext}).error() ==
          EProtoError::RekeyRequired);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a receiver at its limit refuses to open further frames") {
    FrameSealer sealer{MakeKey(1), MakeSalt(1)};
    FrameOpener opener{MakeKey(1), MakeSalt(1), 1};
    const auto plaintext = Bytes("second one is too many");

    std::vector<std::byte> first(HEADER_SIZE + plaintext.size() +
                                 crypto::TAG_SIZE);
    std::vector<std::byte> second(first.size());
    REQUIRE(
        sealer.Seal(MutableByteSpan{first}, ByteSpan{plaintext}).has_value());
    REQUIRE(
        sealer.Seal(MutableByteSpan{second}, ByteSpan{plaintext}).has_value());

    std::vector<std::byte> out(plaintext.size());
    const auto firstFrame = ReadFrame(ByteSpan{first}, MAX_FRAME);
    REQUIRE(firstFrame.has_value());
    REQUIRE(opener.Open(MutableByteSpan{out}, *firstFrame).has_value());

    const auto secondFrame = ReadFrame(ByteSpan{second}, MAX_FRAME);
    REQUIRE(secondFrame.has_value());
    const auto refused = opener.Open(MutableByteSpan{out}, *secondFrame);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == EProtoError::RekeyRequired);
}

TEST_CASE_FIXTURE(SodiumFixture, "opening into too small a buffer is refused") {
    Pair pair;
    const auto plaintext = Bytes("longer than the buffer given back");

    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);
    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext})
                .has_value());

    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());

    std::vector<std::byte> tooSmall(plaintext.size() - 1);
    const auto opened = pair.Opener.Open(MutableByteSpan{tooSmall}, *frame);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == EProtoError::BufferTooSmall);
    CHECK(pair.Opener.Counter() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the sealer reports the epoch it stamps into the header") {
    Pair pair;
    const auto plaintext = Bytes("epoch zero");
    std::vector<std::byte> wire(HEADER_SIZE + plaintext.size() +
                                crypto::TAG_SIZE);

    REQUIRE(pair.Sealer.Seal(MutableByteSpan{wire}, ByteSpan{plaintext})
                .has_value());
    const auto frame = ReadFrame(ByteSpan{wire}, MAX_FRAME);
    REQUIRE(frame.has_value());
    CHECK(frame->Header.Epoch == pair.Sealer.Epoch());
}
