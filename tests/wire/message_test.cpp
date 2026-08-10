#include "zet/wire/message.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace zet;
using namespace zet::wire;

ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const HelloMessage& message) {
    return WriteHello(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const ChallengeMessage& message) {
    return WriteChallenge(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer, const AuthMessage& message) {
    return WriteAuth(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const AuthOkMessage& message) {
    return WriteAuthOk(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const ResumeMessage& message) {
    return WriteResume(writer, message);
}

template <typename TMessage>
std::vector<std::byte> Encode(const TMessage& message) {
    std::vector<std::byte> out(MAX_HANDSHAKE_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(WriteMessage(writer, message).has_value());
    out.resize(writer.Size());
    return out;
}

template <typename TParsed>
ProtoResult<void> Discard(const ProtoResult<TParsed>& parsed) {
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return {};
}

/// Parses whatever the type byte says it is, keeping only the outcome. This is
/// what a truncation sweep needs: the same call for every message and every
/// prefix of it.
ProtoResult<void> ParseAny(ByteSpan bytes) {
    ByteReader reader{bytes};
    const auto type = ParseMessageType(reader);
    if (!type) {
        return std::unexpected(type.error());
    }
    switch (*type) {
        case EMessageType::Hello:
            return Discard(ParseHello(reader));
        case EMessageType::Challenge:
            return Discard(ParseChallenge(reader));
        case EMessageType::Auth:
            return Discard(ParseAuth(reader));
        case EMessageType::AuthOk:
            return Discard(ParseAuthOk(reader));
        case EMessageType::Resume:
            return Discard(ParseResume(reader));
    }
    return {};
}

template <std::size_t tSize>
std::array<std::byte, tSize> Pattern(std::uint8_t seed) {
    std::array<std::byte, tSize> out{};
    std::uint8_t value = seed;
    for (auto& byte : out) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 7);
    }
    return out;
}

HelloMessage MakeHello() {
    HelloMessage message{};
    // Deliberately not the constants: this fixture must keep meaning what it
    // means on the day PROTOCOL_VERSION_MAX moves.
    message.VersionMin = 1;
    message.VersionMax = 0x0102;
    message.Id = Pattern<SESSION_ID_SIZE>(0x11);
    message.ClientNonce = Pattern<HANDSHAKE_NONCE_SIZE>(0x40);
    return message;
}

ChallengeMessage MakeChallenge() {
    ChallengeMessage message{};
    message.Version = 0x0102;
    message.ServerNonce = Pattern<HANDSHAKE_NONCE_SIZE>(0x80);
    return message;
}

AuthMessage MakeAuth() {
    return AuthMessage{.ClientTag = Pattern<crypto::MAC_SIZE>(0x21)};
}

AuthOkMessage MakeAuthOk() {
    return AuthOkMessage{.ServerTag = Pattern<crypto::MAC_SIZE>(0x31),
                         .ServerReceiveOffset = 0x0102'0304'0506'0708};
}

ResumeMessage MakeResume() {
    return ResumeMessage{.ClientReceiveOffset = 0xFFFF'FFFF'FFFF'FFFF};
}

void AppendOption(std::vector<std::byte>& out, std::uint16_t type,
                  ByteSpan value) {
    out.push_back(std::byte{static_cast<std::uint8_t>(type >> 8U)});
    out.push_back(std::byte{static_cast<std::uint8_t>(type & 0xFFU)});
    const auto length = static_cast<std::uint16_t>(value.size());
    out.push_back(std::byte{static_cast<std::uint8_t>(length >> 8U)});
    out.push_back(std::byte{static_cast<std::uint8_t>(length & 0xFFU)});
    out.insert(out.end(), value.begin(), value.end());
}

/// A Hello whose option list is handed over as raw bytes, so that lists no
/// encoder of ours would produce can still be fed to the parser.
std::vector<std::byte> HelloWithRawOptions(ByteSpan rawOptions) {
    auto bytes = Encode(MakeHello());
    bytes.insert(bytes.end(), rawOptions.begin(), rawOptions.end());
    return bytes;
}

}  // namespace

TEST_CASE("a hello survives a round trip") {
    const std::vector<std::byte> optionValue{std::byte{0xDE}, std::byte{0xAD}};
    auto hello = MakeHello();
    REQUIRE(
        hello.Options
            .Append(TlvOption{.Type = 0x0001, .Value = ByteSpan{optionValue}})
            .has_value());

    const auto bytes = Encode(hello);
    ByteReader reader{ByteSpan{bytes}};
    const auto type = ParseMessageType(reader);
    REQUIRE(type.has_value());
    CHECK(*type == EMessageType::Hello);

    const auto parsed = ParseHello(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->VersionMin == hello.VersionMin);
    CHECK(parsed->VersionMax == hello.VersionMax);
    CHECK(parsed->Id == hello.Id);
    CHECK(parsed->ClientNonce == hello.ClientNonce);
    REQUIRE(parsed->Options.Options().size() == 1);
    CHECK(parsed->Options.Options().front().Type == 0x0001);
    CHECK(std::ranges::equal(parsed->Options.Options().front().Value,
                             optionValue));
    CHECK(reader.Exhausted());
}

TEST_CASE("a challenge survives a round trip") {
    const auto challenge = MakeChallenge();
    const auto bytes = Encode(challenge);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Challenge);
    const auto parsed = ParseChallenge(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Version == challenge.Version);
    CHECK(parsed->ServerNonce == challenge.ServerNonce);
    CHECK(parsed->Options.Options().empty());
}

TEST_CASE("an auth survives a round trip") {
    const auto auth = MakeAuth();
    const auto bytes = Encode(auth);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Auth);
    const auto parsed = ParseAuth(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ClientTag == auth.ClientTag);
}

TEST_CASE("an auth_ok survives a round trip") {
    const auto authOk = MakeAuthOk();
    const auto bytes = Encode(authOk);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::AuthOk);
    const auto parsed = ParseAuthOk(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ServerTag == authOk.ServerTag);
    CHECK(parsed->ServerReceiveOffset == authOk.ServerReceiveOffset);
}

TEST_CASE("a resume survives a round trip") {
    const auto resume = MakeResume();
    const auto bytes = Encode(resume);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Resume);
    const auto parsed = ParseResume(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ClientReceiveOffset == resume.ClientReceiveOffset);
}

TEST_CASE("the first nine bytes of a hello are the prefix pinned by §7") {
    auto hello = MakeHello();
    hello.VersionMin = 1;
    hello.VersionMax = 0x0102;

    const auto bytes = Encode(hello);
    // Spelled out as literals on purpose. Reordering the fields, widening a
    // version or renaming the magic all have to break this line: a peer that
    // agrees with us on nothing else still has to be able to read these nine
    // bytes and find out which versions we speak.
    const std::vector<std::byte> prefix{
        std::byte{0x01},                                   // type = Hello
        std::byte{'Z'},  std::byte{'E'},  std::byte{'T'},  // "ZET1"
        std::byte{'1'},                                    //
        std::byte{0x00}, std::byte{0x01},                  // ver_min = 1
        std::byte{0x01}, std::byte{0x02},                  // ver_max = 0x0102
    };

    REQUIRE(bytes.size() > prefix.size());
    CHECK(prefix.size() == HELLO_PREFIX_SIZE);
    CHECK(std::ranges::equal(ByteSpan{bytes}.first(prefix.size()), prefix));
}

TEST_CASE("cutting a message at any offset is truncation, never a parse") {
    // Option-free messages: a list of options is variable length by
    // construction, so a prefix ending on an option boundary is a shorter but
    // entirely valid message. That boundary gets its own case below.
    const std::vector<std::vector<std::byte>> messages{
        Encode(MakeHello()), Encode(MakeChallenge()), Encode(MakeAuth()),
        Encode(MakeAuthOk()), Encode(MakeResume())};

    for (const auto& bytes : messages) {
        const std::size_t size = bytes.size();
        CAPTURE(size);
        for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
            CAPTURE(prefix);
            const auto parsed = ParseAny(ByteSpan{bytes}.first(prefix));
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error() == EProtoError::Truncated);
        }
        CHECK(ParseAny(ByteSpan{bytes}).has_value());
    }
}

TEST_CASE("a writer with too little room refuses instead of overflowing") {
    const std::vector<std::byte> optionValue{std::byte{0x01}, std::byte{0x02}};
    auto hello = MakeHello();
    REQUIRE(
        hello.Options
            .Append(TlvOption{.Type = 0x0001, .Value = ByteSpan{optionValue}})
            .has_value());

    const std::vector<std::vector<std::byte>> encoded{
        Encode(hello), Encode(MakeChallenge()), Encode(MakeAuth()),
        Encode(MakeAuthOk()), Encode(MakeResume())};

    const auto refuses = [](const auto& message, std::size_t capacity) {
        std::vector<std::byte> out(capacity);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteMessage(writer, message);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::BufferTooSmall);
    };

    for (std::size_t capacity = 0; capacity < encoded.front().size();
         ++capacity) {
        CAPTURE(capacity);
        refuses(hello, capacity);
    }
    for (std::size_t capacity = 0; capacity < encoded[1].size(); ++capacity) {
        refuses(MakeChallenge(), capacity);
    }
    for (std::size_t capacity = 0; capacity < encoded[2].size(); ++capacity) {
        refuses(MakeAuth(), capacity);
    }
    for (std::size_t capacity = 0; capacity < encoded[3].size(); ++capacity) {
        refuses(MakeAuthOk(), capacity);
    }
    for (std::size_t capacity = 0; capacity < encoded[4].size(); ++capacity) {
        refuses(MakeResume(), capacity);
    }
}

TEST_CASE("an unknown option that is not critical is carried past") {
    const std::vector<std::byte> value{std::byte{0x99}};
    std::vector<std::byte> options;
    AppendOption(options, 0x4321, ByteSpan{value});

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Hello);

    const auto parsed = ParseHello(reader);
    REQUIRE(parsed.has_value());
    // The point is the message around the option: an extension we never heard
    // of must not shift a single field of what we do understand.
    CHECK(parsed->Id == MakeHello().Id);
    CHECK(parsed->ClientNonce == MakeHello().ClientNonce);
    REQUIRE(parsed->Options.Options().size() == 1);
    CHECK_FALSE(parsed->Options.Options().front().IsCritical());
}

TEST_CASE("an unknown critical option is refused") {
    std::vector<std::byte> options;
    AppendOption(options, static_cast<std::uint16_t>(TLV_CRITICAL_BIT | 0x0001),
                 ByteSpan{});

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::UnsupportedCriticalOption);
    // A retry will not teach us the option, but the connection is still all
    // that may go: Hello is cleartext and unauthenticated, so a disposition of
    // KillSession here would let anyone who read a sid off the wire end that
    // session.
    CHECK(DispositionOf(parsed.error()) == EDisposition::CloseConnection);
}

TEST_CASE("a cut inside an option is an error, not the end of the list") {
    const std::vector<std::byte> value{std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> options;
    AppendOption(options, 0x0001, ByteSpan{value});

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    const std::size_t listStart = bytes.size() - options.size();

    // A whole option list may end where it ends; one byte further in it may
    // not.
    CHECK(ParseAny(ByteSpan{bytes}.first(listStart)).has_value());
    for (std::size_t prefix = listStart + 1; prefix < bytes.size(); ++prefix) {
        CAPTURE(prefix);
        const auto parsed = ParseAny(ByteSpan{bytes}.first(prefix));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::Truncated);
    }
    CHECK(ParseAny(ByteSpan{bytes}).has_value());
}

TEST_CASE("an option claiming more than the buffer holds is refused") {
    // Length says 0xFFFF, one byte follows. Read past the end and this is where
    // it would happen.
    std::vector<std::byte> options{std::byte{0x00}, std::byte{0x01},
                                   std::byte{0xFF}, std::byte{0xFF},
                                   std::byte{0x7E}};

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::Truncated);
}

TEST_CASE("a repeated option type is malformed") {
    const std::vector<std::byte> value{std::byte{0x01}};
    std::vector<std::byte> options;
    AppendOption(options, 0x0007, ByteSpan{value});
    AppendOption(options, 0x0007, ByteSpan{});

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::MalformedField);
}

TEST_CASE("more options than the list can hold are refused") {
    std::vector<std::byte> options;
    for (std::size_t i = 0; i <= MAX_TLV_OPTIONS; ++i) {
        AppendOption(options, static_cast<std::uint16_t>(i), ByteSpan{});
    }

    const auto bytes = HelloWithRawOptions(ByteSpan{options});
    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
}

TEST_CASE("an option value too wide for the length field cannot be built") {
    const std::vector<std::byte> value(MAX_TLV_VALUE_SIZE + 1);
    TlvList options;

    const auto appended =
        options.Append(TlvOption{.Type = 0x0001, .Value = ByteSpan{value}});
    REQUIRE_FALSE(appended.has_value());
    CHECK(appended.error() == EProtoError::LengthLimitExceeded);
    CHECK(options.Options().empty());
}

TEST_CASE("bytes past the last field of a fixed message are malformed") {
    for (auto bytes :
         {Encode(MakeAuth()), Encode(MakeAuthOk()), Encode(MakeResume())}) {
        bytes.push_back(std::byte{0x00});
        const auto parsed = ParseAny(ByteSpan{bytes});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
}

TEST_CASE("a type byte outside the registry is unknown") {
    // 0x00 is not assigned, 0x06 and 0x10 open the reserved ranges of §7, 0xFF
    // is past everything.
    for (const std::uint8_t type :
         std::array<std::uint8_t, 4>{0x00, 0x06, 0x10, 0xFF}) {
        CAPTURE(type);
        const std::vector<std::byte> bytes{std::byte{type}};
        ByteReader reader{ByteSpan{bytes}};
        const auto parsed = ParseMessageType(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::UnknownMessageType);
    }
}

TEST_CASE("a hello with the wrong magic is refused") {
    auto bytes = Encode(MakeHello());
    bytes.at(1) = std::byte{'X'};

    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::MalformedField);
}

TEST_CASE("a version range that runs backwards is refused") {
    auto hello = MakeHello();
    hello.VersionMin = 4;
    hello.VersionMax = 3;

    const auto bytes = Encode(hello);
    const auto parsed = ParseAny(ByteSpan{bytes});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::MalformedField);
}

TEST_CASE("nothing a handshake message can say reaps the session") {
    // These messages are cleartext and prove nothing: a sid is a routing label
    // anyone on the path can read (§5). If any way of malforming one came back
    // with KillSession, that label would become a way to end the session it
    // names. The sweep is over outcomes rather than a list of error codes so
    // that a new one added to the parser is covered the day it appears.
    std::vector<std::vector<std::byte>> corpus;

    for (const auto& whole :
         {Encode(MakeHello()), Encode(MakeChallenge()), Encode(MakeAuth()),
          Encode(MakeAuthOk()), Encode(MakeResume())}) {
        for (std::size_t prefix = 0; prefix < whole.size(); ++prefix) {
            corpus.emplace_back(
                whole.begin(),
                whole.begin() + static_cast<std::ptrdiff_t>(prefix));
        }
        auto trailing = whole;
        trailing.push_back(std::byte{0x00});
        corpus.push_back(std::move(trailing));
    }

    for (const std::uint8_t type :
         std::array<std::uint8_t, 4>{0x00, 0x06, 0x10, 0xFF}) {
        corpus.push_back({std::byte{type}});
    }

    const std::vector<std::byte> value{std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> critical;
    AppendOption(critical, static_cast<std::uint16_t>(TLV_CRITICAL_BIT | 1),
                 ByteSpan{});
    std::vector<std::byte> repeated;
    AppendOption(repeated, 1, ByteSpan{value});
    AppendOption(repeated, 1, ByteSpan{value});
    std::vector<std::byte> overrun;
    AppendOption(overrun, 1, ByteSpan{value});
    overrun.at(3) = std::byte{0xFF};
    std::vector<std::byte> crowded;
    for (std::uint16_t i = 0; i <= MAX_TLV_OPTIONS; ++i) {
        AppendOption(crowded, static_cast<std::uint16_t>(i + 1), ByteSpan{});
    }

    for (const auto& options : {critical, repeated, overrun, crowded}) {
        corpus.push_back(HelloWithRawOptions(ByteSpan{options}));
    }

    std::size_t refused = 0;
    for (const auto& bytes : corpus) {
        const auto parsed = ParseAny(ByteSpan{bytes});
        if (parsed) {
            continue;
        }
        ++refused;
        INFO("error: " << Describe(parsed.error()));
        CHECK(DispositionOf(parsed.error()) != EDisposition::KillSession);
    }

    // Guards the sweep itself: a corpus the parser happens to accept whole
    // would pass the check above without testing anything.
    CHECK(refused > 0);
}
