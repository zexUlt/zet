/// EchoAck and the ECHO_ACK option (§7, §9 ADR-036): the codec, and what the
/// client puts in Hello.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

constexpr std::size_t ECHO_ACK_BODY = 20;

std::vector<std::byte> Encode(const EchoAckMessage& message) {
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(WriteEchoAck(writer, message).has_value());
    out.resize(writer.Size());
    return out;
}

/// The parse of one whole message body: type byte first, as the channel reads.
ProtoResult<EchoAckMessage> ParseBody(ByteSpan body) {
    ByteReader reader{body};
    const auto type = ParseMessageType(reader);
    if (!type) {
        return std::unexpected(type.error());
    }
    REQUIRE(*type == EMessageType::EchoAck);
    return ParseEchoAck(reader);
}

TEST_CASE("EchoAck is type 0x1A with a body of exactly 20 big-endian bytes") {
    const EchoAckMessage message{.StreamGen = 0x0102'0304,
                                 .OutputOffset = 0x1112'1314'1516'1718,
                                 .InputOffset = 0x2122'2324'2526'2728};
    const auto bytes = Encode(message);

    const std::vector<std::uint8_t> expected{
        0x1A, 0x01, 0x02, 0x03, 0x04, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x17, 0x18, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    REQUIRE(bytes.size() == 1 + ECHO_ACK_BODY);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        CAPTURE(i);
        CHECK(std::to_integer<std::uint8_t>(bytes[i]) == expected[i]);
    }
    CHECK(ParseBody(ByteSpan{bytes}) == message);
}

TEST_CASE("EchoAck round-trips at the ends of every field") {
    const EchoAckMessage cases[] = {
        {},
        {.StreamGen = UINT32_MAX,
         .OutputOffset = UINT64_MAX,
         .InputOffset = UINT64_MAX},
        {.StreamGen = 1, .OutputOffset = 0, .InputOffset = UINT64_MAX},
    };
    for (const auto& message : cases) {
        CHECK(ParseBody(ByteSpan{Encode(message)}) == message);
    }
}

TEST_CASE("an EchoAck body that is not 20 bytes is refused") {
    const auto bytes = Encode(
        EchoAckMessage{.StreamGen = 7, .OutputOffset = 8, .InputOffset = 9});

    // Short: the reader runs out before the last field.
    for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
        CAPTURE(prefix);
        const auto parsed = ParseBody(ByteSpan{bytes}.first(prefix));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::Truncated);
        CHECK(DispositionOf(parsed.error()) == EDisposition::CloseConnection);
    }

    // Long: bytes parked behind the last field.
    auto longer = bytes;
    longer.push_back(std::byte{0});
    const auto parsed = ParseBody(ByteSpan{longer});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::MalformedField);
    CHECK(DispositionOf(parsed.error()) == EDisposition::CloseConnection);
}

TEST_CASE("a writer with too little room for EchoAck refuses") {
    std::vector<std::byte> out(1 + ECHO_ACK_BODY);
    for (std::size_t room = 0; room < out.size(); ++room) {
        CAPTURE(room);
        ByteWriter writer{MutableByteSpan{out}.first(room)};
        CHECK_FALSE(WriteEchoAck(writer, EchoAckMessage{}).has_value());
    }
}

/// The options of the Hello a ClientHandshake writes.
struct HelloOptions {
    bool HasEchoAck{false};
    std::uint16_t EchoAckValue{0};
    bool HasGridCapable{false};
};

HelloOptions HelloOf(bool gridCapable, bool echoAck, std::uint16_t ceiling) {
    const auto secrets = DeriveSessionSecrets(crypto::Key{});
    NonceMemory seen;
    ClientHandshake client{secrets.Id,
                           crypto::Key{secrets.Auth.Expose()},
                           crypto::Key{secrets.ConnectionSeed.Expose()},
                           HandshakeNonce{},
                           seen,
                           ceiling,
                           gridCapable,
                           echoAck};
    std::vector<std::byte> out(MAX_HANDSHAKE_FRAME);
    const auto written = client.Start(MutableByteSpan{out});
    REQUIRE(written.has_value());

    ByteReader reader{ByteSpan{out}.first(*written)};
    REQUIRE(ParseMessageType(reader) == EMessageType::Hello);
    const auto hello = ParseHello(reader);
    REQUIRE(hello.has_value());

    HelloOptions found;
    for (const auto& option : hello->Options.Options()) {
        CHECK_FALSE(option.IsCritical());
        if (option.Type == TLV_ECHO_ACK) {
            REQUIRE(option.Value.size() == 2);
            found.HasEchoAck = true;
            found.EchoAckValue = static_cast<std::uint16_t>(
                std::to_integer<unsigned>(option.Value[0]) << 8U |
                std::to_integer<unsigned>(option.Value[1]));
        } else if (option.Type == TLV_GRID_CAPABLE) {
            found.HasGridCapable = true;
        }
    }
    return found;
}

TEST_CASE_FIXTURE(SodiumFixture, "ECHO_ACK is TLV 0x0004 with u16be value 1") {
    CHECK(TLV_ECHO_ACK == 0x0004);
    const auto found = HelloOf(false, true, PROTOCOL_VERSION_MAX);
    CHECK(found.HasEchoAck);
    CHECK(found.EchoAckValue == 1);
    CHECK_FALSE(found.HasGridCapable);
}

TEST_CASE_FIXTURE(SodiumFixture, "Hello carries ECHO_ACK beside GRID_CAPABLE") {
    const auto found = HelloOf(true, true, PROTOCOL_VERSION_MAX);
    CHECK(found.HasEchoAck);
    CHECK(found.HasGridCapable);
}

TEST_CASE_FIXTURE(SodiumFixture, "Hello has no ECHO_ACK unless asked for") {
    CHECK_FALSE(HelloOf(true, false, PROTOCOL_VERSION_MAX).HasEchoAck);
    CHECK_FALSE(HelloOf(false, false, PROTOCOL_VERSION_MAX).HasEchoAck);
}

TEST_CASE_FIXTURE(SodiumFixture, "a ceiling below 2 does not offer ECHO_ACK") {
    CHECK_FALSE(HelloOf(false, true, PROTOCOL_VERSION_MIN).HasEchoAck);
}

}  // namespace
