#include "zet/crypto/crypto.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace zet;
using namespace zet::crypto;

/// The vectors were taken from libsodium 1.0.20 and frozen here on purpose.
///
/// They do not prove libsodium itself correct — the distribution runs its test
/// suite when building the package. They catch something else: arguments
/// swapped in the wrapper, and a silent divergence when the library is
/// replaced, which is what this module was split off for.
constexpr std::array<std::uint8_t, KEY_SIZE> KAT_KEY = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

constexpr std::array<std::uint8_t, NONCE_SIZE> KAT_NONCE = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
    0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57};

/// A frame header: len = 0x11, epoch = 7. The same bytes go into the AD.
constexpr std::array<std::uint8_t, 5> KAT_AD = {0x00, 0x00, 0x00, 0x11, 0x07};

constexpr std::string_view KAT_PLAINTEXT = "zet frame payload";

constexpr std::array<std::uint8_t, 33> KAT_CIPHERTEXT = {
    0xAE, 0x5C, 0x71, 0x50, 0xB6, 0x92, 0x18, 0x7B, 0xEA, 0xD4, 0xF7,
    0xDF, 0xD6, 0xF0, 0x0A, 0xF3, 0xF6, 0x32, 0x4D, 0x1E, 0xCA, 0x02,
    0xE1, 0x28, 0x3D, 0x22, 0xD0, 0xB3, 0xB6, 0x59, 0xA9, 0x4C, 0xE5};

constexpr std::array<std::uint8_t, KEY_SIZE> KAT_SUBKEY = {
    0xB2, 0x7D, 0x4A, 0xBF, 0xBC, 0xE6, 0x65, 0xD8, 0xF9, 0x65, 0x24,
    0x8D, 0x12, 0x86, 0x40, 0xFE, 0xC7, 0x6F, 0x73, 0x1B, 0xCD, 0x87,
    0xA0, 0xBE, 0x61, 0x48, 0x12, 0x32, 0x6A, 0xA2, 0xC5, 0xFE};

constexpr std::array<std::uint8_t, MAC_SIZE> KAT_MAC = {
    0x61, 0x6E, 0xB3, 0x13, 0x0F, 0x01, 0x84, 0xF9, 0x51, 0x27, 0x40,
    0x1A, 0x75, 0xD6, 0x02, 0xF1, 0x1C, 0x3C, 0x93, 0x3D, 0xA2, 0xE8,
    0xF4, 0xB5, 0xD6, 0x7E, 0xDE, 0x8E, 0x5E, 0x02, 0xEE, 0xF6};

template <std::size_t tSize>
std::vector<std::byte> Bytes(const std::array<std::uint8_t, tSize>& source) {
    std::vector<std::byte> out(tSize);
    for (std::size_t i = 0; i < tSize; ++i) {
        out[i] = static_cast<std::byte>(source[i]);
    }
    return out;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

Key KeyFrom(const std::array<std::uint8_t, KEY_SIZE>& source) {
    KeyBytes bytes{};
    for (std::size_t i = 0; i < KEY_SIZE; ++i) {
        bytes[i] = static_cast<std::byte>(source[i]);
    }
    return Key{bytes};
}

struct SodiumFixture {
    SodiumFixture() { REQUIRE(Init()); }
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture, "AEAD matches the recorded vector") {
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const auto associatedData = Bytes(KAT_AD);
    const auto plaintext = Bytes(KAT_PLAINTEXT);

    std::vector<std::byte> out(plaintext.size() + TAG_SIZE);
    const auto sealed =
        AeadSeal(MutableByteSpan{out}, ByteSpan{plaintext},
                 ByteSpan{associatedData}, key, ByteSpan{nonce});

    REQUIRE(sealed.has_value());
    CHECK(*sealed == KAT_CIPHERTEXT.size());
    CHECK(out == Bytes(KAT_CIPHERTEXT));
}

TEST_CASE_FIXTURE(SodiumFixture, "sealed text opens back to the plaintext") {
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const auto associatedData = Bytes(KAT_AD);
    const auto ciphertext = Bytes(KAT_CIPHERTEXT);

    std::vector<std::byte> out(ciphertext.size() - TAG_SIZE);
    const auto opened =
        AeadOpen(MutableByteSpan{out}, ByteSpan{ciphertext},
                 ByteSpan{associatedData}, key, ByteSpan{nonce});

    REQUIRE(opened.has_value());
    CHECK(*opened == KAT_PLAINTEXT.size());
    CHECK(out == Bytes(KAT_PLAINTEXT));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "flipping any bit of the ciphertext fails the tag") {
    // Exhaustive rather than random: random flips give a test whose failures
    // cannot be reproduced, and the vector is short enough to walk in full.
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const auto associatedData = Bytes(KAT_AD);
    const auto original = Bytes(KAT_CIPHERTEXT);

    std::vector<std::byte> out(original.size());
    std::size_t rejected = 0;

    for (std::size_t byteIndex = 0; byteIndex < original.size(); ++byteIndex) {
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            auto damaged = original;
            damaged[byteIndex] ^= static_cast<std::byte>(1U << bit);

            const auto opened =
                AeadOpen(MutableByteSpan{out}, ByteSpan{damaged},
                         ByteSpan{associatedData}, key, ByteSpan{nonce});
            REQUIRE_FALSE(opened.has_value());
            CHECK(opened.error() == EProtoError::AuthenticationFailed);
            ++rejected;
        }
    }

    CHECK(rejected == original.size() * 8);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "flipping any bit of the header fails the tag") {
    // This is why the whole header goes into the AD: tampering with the
    // declared length or the key generation has to fail the tag rather than
    // steer the parse. In EternalTerminal the message type sat outside the MAC,
    // and a bit flip bought a guaranteed remote abort.
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const auto ciphertext = Bytes(KAT_CIPHERTEXT);
    const auto original = Bytes(KAT_AD);

    std::vector<std::byte> out(ciphertext.size() - TAG_SIZE);

    for (std::size_t byteIndex = 0; byteIndex < original.size(); ++byteIndex) {
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            auto damaged = original;
            damaged[byteIndex] ^= static_cast<std::byte>(1U << bit);

            const auto opened =
                AeadOpen(MutableByteSpan{out}, ByteSpan{ciphertext},
                         ByteSpan{damaged}, key, ByteSpan{nonce});
            REQUIRE_FALSE(opened.has_value());
            CHECK(opened.error() == EProtoError::AuthenticationFailed);
        }
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "a wrong nonce fails the tag") {
    const auto key = KeyFrom(KAT_KEY);
    auto nonce = Bytes(KAT_NONCE);
    nonce[NONCE_SIZE - 1] ^= std::byte{1};

    const auto associatedData = Bytes(KAT_AD);
    const auto ciphertext = Bytes(KAT_CIPHERTEXT);
    std::vector<std::byte> out(ciphertext.size() - TAG_SIZE);

    const auto opened =
        AeadOpen(MutableByteSpan{out}, ByteSpan{ciphertext},
                 ByteSpan{associatedData}, key, ByteSpan{nonce});
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == EProtoError::AuthenticationFailed);
}

TEST_CASE_FIXTURE(SodiumFixture, "a failed open is an error, never a crash") {
    // Garbage in the stream must bring nothing down: the disposition is to
    // close the connection, and the session survives. ET had an STFATAL here,
    // i.e. the process died along with everyone else's sessions.
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const std::vector<std::byte> garbage(64, std::byte{0xAB});
    std::vector<std::byte> out(garbage.size());

    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto opened = AeadOpen(MutableByteSpan{out}, ByteSpan{garbage},
                                     ByteSpan{}, key, ByteSpan{nonce});
        REQUIRE_FALSE(opened.has_value());
        CHECK(DispositionOf(opened.error()) == EDisposition::CloseConnection);
    }

    // And after a hundred rejections a valid frame still parses.
    const auto associatedData = Bytes(KAT_AD);
    const auto ciphertext = Bytes(KAT_CIPHERTEXT);
    std::vector<std::byte> good(ciphertext.size() - TAG_SIZE);
    CHECK(AeadOpen(MutableByteSpan{good}, ByteSpan{ciphertext},
                   ByteSpan{associatedData}, key, ByteSpan{nonce})
              .has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "ciphertext shorter than the tag cannot underflow") {
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    std::vector<std::byte> out(64);

    for (std::size_t length = 0; length < TAG_SIZE; ++length) {
        const std::vector<std::byte> tooShort(length, std::byte{0});
        const auto opened = AeadOpen(MutableByteSpan{out}, ByteSpan{tooShort},
                                     ByteSpan{}, key, ByteSpan{nonce});
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == EProtoError::AuthenticationFailed);
    }
}

TEST_CASE_FIXTURE(SodiumFixture, "a nonce of the wrong size is refused") {
    const auto key = KeyFrom(KAT_KEY);
    const std::vector<std::byte> shortNonce(NONCE_SIZE - 1, std::byte{0});
    const auto plaintext = Bytes(KAT_PLAINTEXT);
    std::vector<std::byte> out(plaintext.size() + TAG_SIZE);

    CHECK(AeadSeal(MutableByteSpan{out}, ByteSpan{plaintext}, ByteSpan{}, key,
                   ByteSpan{shortNonce})
              .error() == EProtoError::MalformedField);
}

TEST_CASE_FIXTURE(SodiumFixture, "sealing into too small a buffer is refused") {
    const auto key = KeyFrom(KAT_KEY);
    const auto nonce = Bytes(KAT_NONCE);
    const auto plaintext = Bytes(KAT_PLAINTEXT);
    std::vector<std::byte> out(plaintext.size() + TAG_SIZE - 1);

    CHECK(AeadSeal(MutableByteSpan{out}, ByteSpan{plaintext}, ByteSpan{}, key,
                   ByteSpan{nonce})
              .error() == EProtoError::BufferTooSmall);
}

TEST_CASE_FIXTURE(SodiumFixture, "KDF matches the recorded vector") {
    const auto master = KeyFrom(KAT_KEY);
    const auto subkey = DeriveSubkey(master, 1, "zet-conn");

    KeyBytes expected{};
    for (std::size_t i = 0; i < KEY_SIZE; ++i) {
        expected[i] = static_cast<std::byte>(KAT_SUBKEY[i]);
    }
    CHECK(subkey.Expose() == expected);
}

TEST_CASE_FIXTURE(SodiumFixture, "KDF separates by both id and context") {
    const auto master = KeyFrom(KAT_KEY);

    const auto first = DeriveSubkey(master, 1, "zet-conn");
    const auto sameAgain = DeriveSubkey(master, 1, "zet-conn");
    const auto otherId = DeriveSubkey(master, 2, "zet-conn");
    const auto otherContext = DeriveSubkey(master, 1, "zet-auth");

    // Determinism — otherwise the two sides derive different keys after a
    // reconnect.
    CHECK(first.Expose() == sameAgain.Expose());
    CHECK(first.Expose() != otherId.Expose());
    CHECK(first.Expose() != otherContext.Expose());
}

// A context of the wrong length is not tested: the signature takes a reference
// to a literal of exactly CONTEXT_SIZE characters, so "short" or "much too
// long" is refused by the compiler and never reaches a test.
TEST_CASE_FIXTURE(SodiumFixture,
                  "derived bytes fill a buffer wider than a key") {
    const auto master = KeyFrom(KAT_KEY);

    std::array<std::byte, 16> salt{};
    DeriveBytes(salt, master, 1, "zet-conn");

    // Same master, same label, different subkey number: independent output.
    std::array<std::byte, 16> other{};
    DeriveBytes(other, master, 2, "zet-conn");

    CHECK(salt != other);
    CHECK(salt != std::array<std::byte, 16>{});
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "MAC matches the recorded vector and verifies") {
    const auto key = KeyFrom(KAT_KEY);
    const auto message = Bytes(KAT_PLAINTEXT);

    const auto mac = Authenticate(ByteSpan{message}, key);
    Mac expected{};
    for (std::size_t i = 0; i < MAC_SIZE; ++i) {
        expected[i] = static_cast<std::byte>(KAT_MAC[i]);
    }

    CHECK(mac == expected);
    CHECK(VerifyMac(mac, ByteSpan{message}, key));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a MAC does not verify against a changed message") {
    const auto key = KeyFrom(KAT_KEY);
    auto message = Bytes(KAT_PLAINTEXT);
    const auto mac = Authenticate(ByteSpan{message}, key);

    for (std::size_t byteIndex = 0; byteIndex < message.size(); ++byteIndex) {
        auto damaged = message;
        damaged[byteIndex] ^= std::byte{1};
        CHECK_FALSE(VerifyMac(mac, ByteSpan{damaged}, key));
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "key exchange gives both sides mirrored keys") {
    const auto client = GenerateKeyPair();
    const auto server = GenerateKeyPair();

    const auto clientKeys = DeriveClientKeys(client, server.Public);
    const auto serverKeys = DeriveServerKeys(server, client.Public);

    REQUIRE(clientKeys.has_value());
    REQUIRE(serverKeys.has_value());

    // The mirroring is what lets the sides talk at all: what the client
    // encrypts with is what the server decrypts with.
    CHECK(clientKeys->Transmit.Expose() == serverKeys->Receive.Expose());
    CHECK(clientKeys->Receive.Expose() == serverKeys->Transmit.Expose());

    // And the two directions differ, otherwise a reflected frame would pass as
    // one of our own.
    CHECK(clientKeys->Transmit.Expose() != clientKeys->Receive.Expose());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a different peer gives different session keys") {
    const auto client = GenerateKeyPair();
    const auto server = GenerateKeyPair();
    const auto impostor = GenerateKeyPair();

    const auto real = DeriveClientKeys(client, server.Public);
    const auto other = DeriveClientKeys(client, impostor.Public);

    REQUIRE(real.has_value());
    REQUIRE(other.has_value());
    CHECK(real->Transmit.Expose() != other->Transmit.Expose());
}

TEST_CASE_FIXTURE(SodiumFixture, "generated key pairs differ") {
    const auto first = GenerateKeyPair();
    const auto second = GenerateKeyPair();
    CHECK(first.Public != second.Public);
}

TEST_CASE_FIXTURE(SodiumFixture, "random bytes fill the whole span") {
    // Not a test of entropy quality — of the buffer being filled at all and the
    // bounds not being mixed up.
    std::vector<std::byte> buffer(64, std::byte{0});
    RandomBytes(MutableByteSpan{buffer});

    std::size_t zeroes = 0;
    for (const auto byte : buffer) {
        zeroes += (byte == std::byte{0}) ? 1 : 0;
    }
    CHECK(zeroes < buffer.size() / 2);
}
