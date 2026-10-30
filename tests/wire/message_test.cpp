#include "zet/wire/message.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
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
// The test helpers use the version as a tag so callers stay explicit.
ProtoResult<void> WriteMessageV1(ByteWriter& writer,
                                 const ResumeMessage& message) {
    return WriteResume(writer, message, PROTOCOL_VERSION_MIN);
}
ProtoResult<void> WriteMessageV2(ByteWriter& writer,
                                 const ResumeMessage& message) {
    return WriteResume(writer, message, PROTOCOL_VERSION_GRID);
}
// Default overload used by Encode<>: v1 keeps pre-M6 behaviour.
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const ResumeMessage& message) {
    return WriteResume(writer, message, PROTOCOL_VERSION_MIN);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const BootstrapOfferMessage& message) {
    return WriteBootstrapOffer(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const BootstrapAcceptMessage& message) {
    return WriteBootstrapAccept(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer, const DataMessage& message) {
    return WriteData(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer, const AckMessage& message) {
    return WriteAck(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const ResizeMessage& message) {
    return WriteResize(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer, const PingMessage& message) {
    return WritePing(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer, const PongMessage& message) {
    return WritePong(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const SessionEndMessage& message) {
    return WriteSessionEnd(writer, message, PROTOCOL_VERSION_MIN);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const GridBeginMessage& message) {
    return WriteGridBegin(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const GridChunkMessage& message) {
    return WriteGridChunk(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const GridEndMessage& message) {
    return WriteGridEnd(writer, message);
}
ProtoResult<void> WriteMessage(ByteWriter& writer,
                               const GridAppliedMessage& message) {
    return WriteGridApplied(writer, message);
}

template <typename TMessage>
std::vector<std::byte> Encode(const TMessage& message) {
    // MAX_FRAME is enough for any message the codecs in this file produce.
    std::vector<std::byte> out(MAX_FRAME);
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
            return Discard(ParseResume(reader, PROTOCOL_VERSION_MIN));
        case EMessageType::BootstrapOffer:
            return Discard(ParseBootstrapOffer(reader));
        case EMessageType::BootstrapAccept:
            return Discard(ParseBootstrapAccept(reader));
        case EMessageType::Data:
            return Discard(ParseData(reader));
        case EMessageType::Ack:
            return Discard(ParseAck(reader));
        case EMessageType::Resize:
            return Discard(ParseResize(reader));
        case EMessageType::Ping:
            return Discard(ParsePing(reader));
        case EMessageType::Pong:
            return Discard(ParsePong(reader));
        case EMessageType::SessionEnd:
            return Discard(ParseSessionEnd(reader, PROTOCOL_VERSION_MIN));
        case EMessageType::GridBegin:
            return Discard(ParseGridBegin(reader));
        case EMessageType::GridChunk:
            return Discard(ParseGridChunk(reader));
        case EMessageType::GridEnd:
            return Discard(ParseGridEnd(reader));
        case EMessageType::GridApplied:
            return Discard(ParseGridApplied(reader));
        case EMessageType::MuxRegister:
            return Discard(ParseMuxRegister(reader));
        case EMessageType::MuxRegistered:
            return Discard(ParseMuxRegistered(reader));
        case EMessageType::MuxRefused:
            return Discard(ParseMuxRefused(reader));
        case EMessageType::MuxHandoff:
            return Discard(ParseMuxHandoff(reader));
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

// v1 Resume (no StreamGen on the wire)
ResumeMessage MakeResume() {
    return ResumeMessage{.StreamGen = 0,
                         .ClientReceiveOffset = 0xFFFF'FFFF'FFFF'FFFF};
}

// v2 Resume (StreamGen precedes c_recv_offset on the wire)
ResumeMessage MakeResumeV2() {
    return ResumeMessage{.StreamGen = 0x0000'0007,
                         .ClientReceiveOffset = 0xFFFF'FFFF'FFFF'FFFF};
}

/// What IsForwardedVariable admits, as design.md §4 lists it.
constexpr std::array<std::string_view, MAX_BOOTSTRAP_ENV> FORWARDED_FOR_TEST{
    "TERM",           "COLORTERM",   "LANG",
    "LANGUAGE",       "LC_ALL",      "LC_ADDRESS",
    "LC_COLLATE",     "LC_CTYPE",    "LC_IDENTIFICATION",
    "LC_MEASUREMENT", "LC_MESSAGES", "LC_MONETARY",
    "LC_NAME",        "LC_NUMERIC",  "LC_PAPER",
    "LC_TELEPHONE",   "LC_TIME"};

BootstrapOfferMessage MakeOffer() {
    BootstrapOfferMessage offer{
        .VersionMin = 1,
        .VersionMax = 0x0102,
        .ClientKey = Pattern<crypto::PUBLIC_KEY_SIZE>(0x51),
        .ClientNonce = Pattern<BOOTSTRAP_NONCE_SIZE>(0x61),
        .Environment = {}};
    REQUIRE(offer.Environment.Add("TERM", "xterm-256color"));
    REQUIRE(offer.Environment.Add("LANG", "en_US.UTF-8"));
    return offer;
}

BootstrapAcceptMessage MakeAccept() {
    return BootstrapAcceptMessage{
        .Version = 0x0102,
        .AgentKey = Pattern<crypto::PUBLIC_KEY_SIZE>(0x71),
        .ClientNonce = Pattern<BOOTSTRAP_NONCE_SIZE>(0x61),
        .AgentNonce = Pattern<BOOTSTRAP_NONCE_SIZE>(0x81),
        .Port = 0xC350,
        .OrphanTimeoutSeconds = 0x0000'0708};
}

// For Data, the payload is a ByteSpan: keep the bytes alongside the message.
std::pair<std::vector<std::byte>, DataMessage> MakeData() {
    std::vector<std::byte> payload{std::byte{0xDE}, std::byte{0xAD},
                                   std::byte{0xBE}};
    DataMessage msg{.Ack = 0x0102'0304'0506'0708,
                    .Offset = 0x0807'0605'0403'0201,
                    .Payload = ByteSpan{payload}};
    return {std::move(payload), msg};
}

AckMessage MakeAck() { return AckMessage{.Ack = 0xCAFE'BABE'0000'0001}; }

ResizeMessage MakeResize() {
    return ResizeMessage{.Rows = 24, .Cols = 80, .XPixel = 960, .YPixel = 384};
}

PingMessage MakePing() { return PingMessage{.Token = 0xDEAD'BEEF'0102'0304}; }

PongMessage MakePong() { return PongMessage{.Token = 0xDEAD'BEEF'0102'0304}; }

SessionEndMessage MakeEnd() {
    return SessionEndMessage{.Kind = ESessionEndKind::Signaled, .Value = 15};
}

SessionEndMessage MakeOverflow() {
    return SessionEndMessage{.Kind = ESessionEndKind::Overflow, .Value = 0};
}

// For GridChunk the payload is a ByteSpan: keep the storage alongside.
// Usage:
//   auto [storage, msg] = MakeGridChunkStorage();
//   msg.Data = ByteSpan{storage};   // pointer still valid
std::pair<std::array<std::byte, 4>, GridChunkMessage> MakeGridChunkStorage() {
    std::array<std::byte, 4> data{std::byte{0xCA}, std::byte{0xFE},
                                  std::byte{0xBA}, std::byte{0xBE}};
    GridChunkMessage msg{.ChunkOffset = 0x0000'1234, .Data = {}};
    return {data, msg};
}

GridBeginMessage MakeGridBegin() {
    return GridBeginMessage{.Flags = 0x01,
                            .StreamGen = 0x0000'0001,
                            .ScrollbackLen = 1024,
                            .TotalLen = 2048};
}

GridEndMessage MakeGridEnd() { return GridEndMessage{}; }

GridAppliedMessage MakeGridApplied() {
    return GridAppliedMessage{.StreamGen = 0x0000'0001};
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
    const auto parsed = ParseResume(reader, PROTOCOL_VERSION_MIN);
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
    //
    // Data is excluded: its payload is variable length, so any prefix at or
    // beyond the header (type+ack+offset = 17 bytes) is either a valid shorter
    // Data or MalformedField (empty payload). It has its own test below.
    // GridChunk is excluded like Data: its body is header+variable-tail, so a
    // prefix that ends right after the header is MalformedField (empty
    // payload), not Truncated. GridChunk has its own truncation test below.
    const std::vector<std::vector<std::byte>> messages{
        Encode(MakeHello()),      Encode(MakeChallenge()),
        Encode(MakeAuth()),       Encode(MakeAuthOk()),
        Encode(MakeResume()),     Encode(MakeAck()),
        Encode(MakeResize()),     Encode(MakePing()),
        Encode(MakePong()),       Encode(MakeOffer()),
        Encode(MakeAccept()),     Encode(MakeEnd()),
        Encode(MakeGridBegin()),  Encode(MakeGridEnd()),
        Encode(MakeGridApplied())};

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

    auto [dataPayload, dataMsg] = MakeData();

    const auto refuses = [](const auto& message, std::size_t capacity) {
        std::vector<std::byte> out(capacity);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteMessage(writer, message);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::BufferTooSmall);
    };

    const auto refusesUpTo = [&refuses](const auto& message) {
        const auto full = Encode(message);
        for (std::size_t capacity = 0; capacity < full.size(); ++capacity) {
            CAPTURE(capacity);
            refuses(message, capacity);
        }
    };

    refusesUpTo(hello);
    refusesUpTo(MakeChallenge());
    refusesUpTo(MakeAuth());
    refusesUpTo(MakeAuthOk());
    refusesUpTo(MakeResume());
    refusesUpTo(dataMsg);
    refusesUpTo(MakeAck());
    refusesUpTo(MakeResize());
    refusesUpTo(MakePing());
    refusesUpTo(MakePong());
    refusesUpTo(MakeOffer());
    refusesUpTo(MakeAccept());
    refusesUpTo(MakeEnd());
    refusesUpTo(MakeGridBegin());
    refusesUpTo(MakeGridEnd());
    refusesUpTo(MakeGridApplied());
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
    // Data is excluded: its "last field" is the variable payload, so a trailing
    // byte would just extend the payload and remain valid.
    for (auto bytes :
         {Encode(MakeAuth()), Encode(MakeAuthOk()), Encode(MakeResume()),
          Encode(MakeAck()), Encode(MakeResize()), Encode(MakePing()),
          Encode(MakePong()), Encode(MakeOffer()), Encode(MakeAccept()),
          Encode(MakeEnd()), Encode(MakeGridBegin()), Encode(MakeGridEnd()),
          Encode(MakeGridApplied())}) {
        bytes.push_back(std::byte{0x00});
        const auto parsed = ParseAny(ByteSpan{bytes});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
}

TEST_CASE("a type byte outside the registry is unknown") {
    // 0x00 is not assigned, 0x08 is the first free one of the bootstrap
    // range (§7), 0x1A the first unassigned session type after the Grid
    // block (0x16–0x19), 0xFF is past everything.
    for (const std::uint8_t type :
         std::array<std::uint8_t, 4>{0x00, 0x08, 0x1A, 0xFF}) {
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
         std::array<std::uint8_t, 4>{0x00, 0x08, 0x1A, 0xFF}) {
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

// ─── Bootstrap messages ──────────────────────────────────────────────────────

TEST_CASE("a bootstrap offer survives a round trip") {
    const auto offer = MakeOffer();
    const auto bytes = Encode(offer);
    CHECK(bytes.front() == std::byte{0x06});
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::BootstrapOffer);
    const auto parsed = ParseBootstrapOffer(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->VersionMin == offer.VersionMin);
    CHECK(parsed->VersionMax == offer.VersionMax);
    CHECK(parsed->ClientKey == offer.ClientKey);
    CHECK(parsed->ClientNonce == offer.ClientNonce);
    CHECK(parsed->Environment == offer.Environment);
    REQUIRE(parsed->Environment.Variables().size() == 2);
    CHECK(parsed->Environment.Variables()[0].Name() == "TERM");
    CHECK(parsed->Environment.Variables()[0].Value() == "xterm-256color");
}

TEST_CASE("a bootstrap accept survives a round trip") {
    const auto accept = MakeAccept();
    const auto bytes = Encode(accept);
    CHECK(bytes.front() == std::byte{0x07});
    // The port, then the orphan timeout, both big-endian, at the end.
    CHECK(bytes.at(bytes.size() - 6) == std::byte{0xC3});
    CHECK(bytes.at(bytes.size() - 5) == std::byte{0x50});
    CHECK(bytes.at(bytes.size() - 2) == std::byte{0x07});
    CHECK(bytes.back() == std::byte{0x08});
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::BootstrapAccept);
    const auto parsed = ParseBootstrapAccept(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Version == accept.Version);
    CHECK(parsed->AgentKey == accept.AgentKey);
    CHECK(parsed->ClientNonce == accept.ClientNonce);
    CHECK(parsed->AgentNonce == accept.AgentNonce);
    CHECK(parsed->Port == accept.Port);
    CHECK(parsed->OrphanTimeoutSeconds == accept.OrphanTimeoutSeconds);
}

TEST_CASE("only the terminal type and the locale are forwarded") {
    for (const std::string_view name :
         {"TERM", "COLORTERM", "LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE",
          "LC_IDENTIFICATION"}) {
        CAPTURE(name);
        CHECK(IsForwardedVariable(name));
    }
    for (const std::string_view name :
         {"", "PATH", "LD_PRELOAD", "HOME", "LC_", "LC__", "LC_all", "LC_X1",
          "LC_MODIFIERS", "term", "TERMINFO", "LANGX"}) {
        CAPTURE(name);
        CHECK_FALSE(IsForwardedVariable(name));
    }
}

TEST_CASE("an environment refuses what it could not forward") {
    BootstrapEnvironment environment;
    CHECK_FALSE(environment.Add("PATH", "/bin"));
    CHECK_FALSE(environment.Add("TERM", "xterm\n"));
    CHECK_FALSE(environment.Add("TERM", std::string_view{"x\0y", 3}));
    CHECK_FALSE(environment.Add("TERM", std::string(MAX_ENV_VALUE + 1, 'x')));
    CHECK_FALSE(environment.Add("LC_" + std::string(MAX_ENV_NAME, 'A'), "C"));
    REQUIRE(environment.Add("TERM", ""));
    CHECK_FALSE(environment.Add("TERM", "xterm"));
    CHECK(environment.Variables().size() == 1);

    // Every forwarded name fits at once, and that is the whole list.
    BootstrapEnvironment full;
    for (const std::string_view name : FORWARDED_FOR_TEST) {
        REQUIRE(full.Add(name, "C"));
    }
    CHECK(full.Variables().size() == MAX_BOOTSTRAP_ENV);
    CHECK_FALSE(full.Add("TERM", "xterm"));
    CHECK_FALSE(full.Add("LANG", "C"));
}

TEST_CASE("an offer at the most it may carry survives a round trip") {
    BootstrapOfferMessage offer = MakeOffer();
    offer.Environment = {};
    for (const std::string_view name : FORWARDED_FOR_TEST) {
        REQUIRE(offer.Environment.Add(name, std::string(MAX_ENV_VALUE, '~')));
    }
    const auto bytes = Encode(offer);
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::BootstrapOffer);
    const auto parsed = ParseBootstrapOffer(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Environment == offer.Environment);
}

TEST_CASE("an offer whose environment the agent would not set is refused") {
    // Header fields as MakeOffer writes them, then the environment by hand.
    auto bytes = Encode(MakeOffer());
    const std::size_t fixed = 1 + 2 * sizeof(std::uint16_t) +
                              crypto::PUBLIC_KEY_SIZE + BOOTSTRAP_NONCE_SIZE;
    bytes.resize(fixed);
    const auto refused = [&](std::vector<std::byte> environment,
                             EProtoError expected) {
        auto message = bytes;
        message.insert(message.end(), environment.begin(), environment.end());
        ByteReader reader{ByteSpan{message}};
        REQUIRE(ParseMessageType(reader) == EMessageType::BootstrapOffer);
        const auto parsed = ParseBootstrapOffer(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == expected);
    };
    const auto text = [](std::string_view chars) {
        std::vector<std::byte> out;
        out.push_back(static_cast<std::byte>(chars.size()));
        for (const char c : chars) {
            out.push_back(static_cast<std::byte>(c));
        }
        return out;
    };
    const auto entry = [&](std::string_view name, std::string_view value) {
        auto out = text(name);
        const auto rest = text(value);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    };
    const auto withCount = [](std::uint8_t count, std::vector<std::byte> rest) {
        rest.insert(rest.begin(), static_cast<std::byte>(count));
        return rest;
    };

    SUBCASE("no count at all") { refused({}, EProtoError::Truncated); }
    SUBCASE("bytes after an empty environment") {
        refused(withCount(0, {std::byte{0}}), EProtoError::MalformedField);
    }
    SUBCASE("more variables than an Offer carries") {
        refused(withCount(MAX_BOOTSTRAP_ENV + 1, {}),
                EProtoError::LengthLimitExceeded);
    }
    SUBCASE("a name that is not forwarded") {
        refused(withCount(1, entry("LD_PRELOAD", "/tmp/x.so")),
                EProtoError::MalformedField);
    }
    SUBCASE("a value with a control character") {
        refused(withCount(1, entry("TERM", "xterm\x1b")),
                EProtoError::MalformedField);
    }
    SUBCASE("the same name twice") {
        auto two = entry("TERM", "a");
        const auto again = entry("TERM", "b");
        two.insert(two.end(), again.begin(), again.end());
        refused(withCount(2, two), EProtoError::MalformedField);
    }
    SUBCASE("fewer variables than counted") {
        refused(withCount(2, entry("TERM", "a")), EProtoError::Truncated);
    }
    SUBCASE("a value cut short") {
        auto cut = entry("TERM", "xterm");
        cut.pop_back();
        refused(withCount(1, cut), EProtoError::Truncated);
    }
}

TEST_CASE("an accept without its orphan timeout is truncated") {
    auto bytes = Encode(MakeAccept());
    bytes.resize(bytes.size() - sizeof(std::uint32_t));
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::BootstrapAccept);
    const auto parsed = ParseBootstrapAccept(reader);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::Truncated);
}

TEST_CASE("an offer with a backwards version range is refused both ways") {
    auto offer = MakeOffer();
    offer.VersionMin = 3;
    offer.VersionMax = 2;
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    CHECK(WriteBootstrapOffer(writer, offer).error() ==
          EProtoError::MalformedField);

    auto bytes = Encode(MakeOffer());
    // ver_min:u16 sits right after the type byte.
    bytes.at(1) = std::byte{0xFF};
    CHECK(ParseAny(ByteSpan{bytes}).error() == EProtoError::MalformedField);
}

TEST_CASE("an accept naming port zero is refused both ways") {
    auto accept = MakeAccept();
    accept.Port = 0;
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    CHECK(WriteBootstrapAccept(writer, accept).error() ==
          EProtoError::MalformedField);

    auto bytes = Encode(MakeAccept());
    // The port sits before the four bytes of the orphan timeout.
    bytes.at(bytes.size() - 6) = std::byte{0};
    bytes.at(bytes.size() - 5) = std::byte{0};
    CHECK(ParseAny(ByteSpan{bytes}).error() == EProtoError::MalformedField);
}

// ─── Session messages ────────────────────────────────────────────────────────

TEST_CASE("a data message survives a round trip") {
    auto [payload, data] = MakeData();
    const auto bytes = Encode(data);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Data);
    const auto parsed = ParseData(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Ack == data.Ack);
    CHECK(parsed->Offset == data.Offset);
    CHECK(std::ranges::equal(parsed->Payload, data.Payload));
    CHECK(reader.Exhausted());
}

TEST_CASE("an ack message survives a round trip") {
    const auto ack = MakeAck();
    const auto bytes = Encode(ack);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Ack);
    const auto parsed = ParseAck(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Ack == ack.Ack);
}

TEST_CASE("a resize message survives a round trip") {
    const auto resize = MakeResize();
    const auto bytes = Encode(resize);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Resize);
    const auto parsed = ParseResize(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Rows == resize.Rows);
    CHECK(parsed->Cols == resize.Cols);
    CHECK(parsed->XPixel == resize.XPixel);
    CHECK(parsed->YPixel == resize.YPixel);
}

TEST_CASE("a ping message survives a round trip") {
    const auto ping = MakePing();
    const auto bytes = Encode(ping);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Ping);
    const auto parsed = ParsePing(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Token == ping.Token);
}

TEST_CASE("a pong message survives a round trip") {
    const auto pong = MakePong();
    const auto bytes = Encode(pong);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Pong);
    const auto parsed = ParsePong(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Token == pong.Token);
}

TEST_CASE("a session end survives a round trip") {
    for (const auto end :
         {MakeEnd(),
          SessionEndMessage{.Kind = ESessionEndKind::Exited, .Value = 0},
          SessionEndMessage{.Kind = ESessionEndKind::Exited, .Value = 255}}) {
        const auto bytes = Encode(end);
        REQUIRE(bytes.size() == 3);
        CHECK(bytes[1] == std::byte{static_cast<std::uint8_t>(end.Kind)});
        CHECK(bytes[2] == std::byte{end.Value});

        ByteReader reader{ByteSpan{bytes}};
        REQUIRE(ParseMessageType(reader) == EMessageType::SessionEnd);
        const auto parsed = ParseSessionEnd(reader, PROTOCOL_VERSION_MIN);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == end);
    }
}

TEST_CASE("a session end of a kind nobody defined is refused both ways") {
    auto bytes = Encode(MakeEnd());
    bytes.at(1) = std::byte{2};
    CHECK(ParseAny(ByteSpan{bytes}).error() == EProtoError::MalformedField);

    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    // bit_cast rather than static_cast: the point is a value outside the
    // enumeration, which is what the analyzer rightly flags a cast for.
    const SessionEndMessage odd{
        .Kind = std::bit_cast<ESessionEndKind>(std::uint8_t{7}), .Value = 0};
    CHECK(WriteSessionEnd(writer, odd, PROTOCOL_VERSION_MIN).error() ==
          EProtoError::MalformedField);
    CHECK(writer.Size() == 0);
}

TEST_CASE("big-endian layout of session messages is pinned by explicit bytes") {
    // Resize: rows=0x0018 cols=0x0050 xpixel=0x03C0 ypixel=0x0180.
    {
        const auto bytes = Encode(ResizeMessage{.Rows = 0x0018,
                                                .Cols = 0x0050,
                                                .XPixel = 0x03C0,
                                                .YPixel = 0x0180});
        const std::vector<std::byte> expected{
            std::byte{0x12},                   // type = Resize
            std::byte{0x00}, std::byte{0x18},  // rows
            std::byte{0x00}, std::byte{0x50},  // cols
            std::byte{0x03}, std::byte{0xC0},  // xpixel
            std::byte{0x01}, std::byte{0x80},  // ypixel
        };
        CHECK(std::ranges::equal(bytes, expected));
    }

    // Ack: ack=0x0102030405060708.
    {
        const auto bytes = Encode(AckMessage{.Ack = 0x0102'0304'0506'0708});
        const std::vector<std::byte> expected{
            std::byte{0x11},                                    // type
            std::byte{0x01}, std::byte{0x02}, std::byte{0x03},  // ack
            std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
            std::byte{0x07}, std::byte{0x08},
        };
        CHECK(std::ranges::equal(bytes, expected));
    }

    // Data: ack=0x01, offset=0x02, payload={0xAB}.
    {
        const std::vector<std::byte> pl{std::byte{0xAB}};
        const auto bytes =
            Encode(DataMessage{.Ack = 1, .Offset = 2, .Payload = ByteSpan{pl}});
        const std::vector<std::byte> expected{
            std::byte{0x10},  // type
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x01},  // ack
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x02},  // offset
            std::byte{0xAB},                   // payload
        };
        CHECK(std::ranges::equal(bytes, expected));
    }
}

TEST_CASE("an empty data payload is malformed") {
    // Write side refuses before producing any bytes.
    {
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteData(writer, DataMessage{});
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::MalformedField);
    }

    // Parse side: type + ack + offset, payload absent.
    {
        const std::vector<std::byte> bytes{
            std::byte{0x10},  // type
            std::byte{0},    std::byte{0}, std::byte{0},
            std::byte{0},    std::byte{0}, std::byte{0},
            std::byte{0},    std::byte{0},  // ack
            std::byte{0},    std::byte{0}, std::byte{0},
            std::byte{0},    std::byte{0}, std::byte{0},
            std::byte{0},    std::byte{0},  // offset
        };
        const auto parsed = ParseAny(ByteSpan{bytes});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
}

TEST_CASE("a data payload larger than MAX_DATA_PAYLOAD is refused") {
    // Write side.
    {
        const std::vector<std::byte> big(MAX_DATA_PAYLOAD + 1);
        std::vector<std::byte> out(MAX_FRAME + MAX_DATA_PAYLOAD + 1);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written =
            WriteData(writer, DataMessage{.Payload = ByteSpan{big}});
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::LengthLimitExceeded);
    }

    // Parse side: craft a reader that lies about having MAX_DATA_PAYLOAD+1
    // bytes after the header. We test the size check, not actual byte
    // consumption, so a sub-span of a large buffer suffices.
    {
        std::vector<std::byte> big(1 + 8 + 8 + MAX_DATA_PAYLOAD + 1,
                                   std::byte{0x00});
        big.front() = std::byte{0x10};
        ByteReader reader{ByteSpan{big}.subspan(1)};
        const auto parsed = ParseData(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
    }
}

TEST_CASE("cutting a data message before the payload header is truncation") {
    // Bytes 0..17 (exclusive) always give Truncated.
    // Byte 17 (header complete, payload missing) gives MalformedField.
    // Byte 18+ is a valid Data with 1+ bytes of payload.
    const std::vector<std::byte> pl{std::byte{0xFF}};
    const auto full =
        Encode(DataMessage{.Ack = 0, .Offset = 0, .Payload = ByteSpan{pl}});
    REQUIRE(full.size() == 18);

    for (std::size_t prefix = 0; prefix < 17; ++prefix) {
        CAPTURE(prefix);
        const auto parsed = ParseAny(ByteSpan{full}.first(prefix));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::Truncated);
    }
    {
        const auto parsed = ParseAny(ByteSpan{full}.first(17));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
    CHECK(ParseAny(ByteSpan{full}).has_value());
}

TEST_CASE("cutting a GridChunk before the data is truncation") {
    // type(1) + chunk_offset(4) = 5 bytes header; at exactly 5 bytes the data
    // is absent, which is MalformedField. Beyond 5 bytes it is valid.
    auto [storage, msg] = MakeGridChunkStorage();
    msg.Data = ByteSpan{storage};
    const auto full = Encode(msg);
    REQUIRE(full.size() == 9);  // 1 + 4 + 4

    for (std::size_t prefix = 0; prefix < 5; ++prefix) {
        CAPTURE(prefix);
        const auto parsed = ParseAny(ByteSpan{full}.first(prefix));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::Truncated);
    }
    {
        const auto parsed = ParseAny(ByteSpan{full}.first(5));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
    CHECK(ParseAny(ByteSpan{full}).has_value());
}

// ─── Protocol v2: Resume ─────────────────────────────────────────────────────

TEST_CASE("a v2 resume carries stream_gen before c_recv_offset on the wire") {
    const auto msg = MakeResumeV2();
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(WriteMessageV2(writer, msg).has_value());
    out.resize(writer.Size());

    // Type byte + stream_gen (4) + c_recv_offset (8) = 13 bytes.
    REQUIRE(out.size() == 13);
    CHECK(out[0] == std::byte{0x05});  // Resume type
    // stream_gen = 0x00000007 big-endian
    CHECK(out[1] == std::byte{0x00});
    CHECK(out[2] == std::byte{0x00});
    CHECK(out[3] == std::byte{0x00});
    CHECK(out[4] == std::byte{0x07});
    // c_recv_offset = 0xFFFFFFFFFFFFFFFF big-endian
    for (std::size_t i = 5; i < 13; ++i) {
        CHECK(out[i] == std::byte{0xFF});
    }

    ByteReader reader{ByteSpan{out}.subspan(1)};
    const auto parsed = ParseResume(reader, PROTOCOL_VERSION_GRID);
    REQUIRE(parsed.has_value());
    CHECK(parsed->StreamGen == msg.StreamGen);
    CHECK(parsed->ClientReceiveOffset == msg.ClientReceiveOffset);
}

TEST_CASE("a v1 resume is rejected by a v2 parser and vice versa") {
    // v1 wire: type + c_recv_offset (8) = 9 bytes.
    {
        std::vector<std::byte> v1bytes(MAX_FRAME);
        ByteWriter w{MutableByteSpan{v1bytes}};
        REQUIRE(WriteMessageV1(w, MakeResume()).has_value());
        v1bytes.resize(w.Size());
        // Remove the type byte, feed body to a v2 parser.
        ByteReader reader{ByteSpan{v1bytes}.subspan(1)};
        // v2 tries to read stream_gen(4) + c_recv_offset(8) = 12 bytes;
        // v1 body is only 8 bytes, so it truncates.
        const auto parsed = ParseResume(reader, PROTOCOL_VERSION_GRID);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::Truncated);
    }

    // v2 wire: type + stream_gen(4) + c_recv_offset(8) = 13 bytes.
    {
        std::vector<std::byte> v2bytes(MAX_FRAME);
        ByteWriter w{MutableByteSpan{v2bytes}};
        REQUIRE(
            WriteResume(w, MakeResumeV2(), PROTOCOL_VERSION_GRID).has_value());
        v2bytes.resize(w.Size());
        // Remove the type byte, feed body to a v1 parser.
        ByteReader reader{ByteSpan{v2bytes}.subspan(1)};
        // v1 reads c_recv_offset(8), then RequireExhausted finds 4 leftover
        // bytes (the stream_gen that v1 never consumed).
        const auto parsed = ParseResume(reader, PROTOCOL_VERSION_MIN);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }
}

// ─── Protocol v2: SessionEnd Overflow ────────────────────────────────────────

TEST_CASE("SessionEnd Overflow is accepted at v2 and rejected at v1") {
    const auto overflow = MakeOverflow();

    // Write at v2 must succeed.
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(
        WriteSessionEnd(writer, overflow, PROTOCOL_VERSION_GRID).has_value());
    out.resize(writer.Size());
    REQUIRE(out.size() == 3);
    CHECK(out[0] == std::byte{0x15});  // SessionEnd type
    CHECK(out[1] == std::byte{0x02});  // kind = Overflow
    CHECK(out[2] == std::byte{0x00});  // value

    // Parse at v2 must round-trip.
    {
        ByteReader reader{ByteSpan{out}.subspan(1)};
        const auto parsed = ParseSessionEnd(reader, PROTOCOL_VERSION_GRID);
        REQUIRE(parsed.has_value());
        CHECK(parsed->Kind == ESessionEndKind::Overflow);
        CHECK(parsed->Value == 0);
    }

    // Parse at v1 must refuse.
    {
        ByteReader reader{ByteSpan{out}.subspan(1)};
        const auto parsed = ParseSessionEnd(reader, PROTOCOL_VERSION_MIN);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::MalformedField);
    }

    // Write at v1 must refuse.
    {
        std::vector<std::byte> tmp(MAX_FRAME);
        ByteWriter w{MutableByteSpan{tmp}};
        const auto written = WriteSessionEnd(w, overflow, PROTOCOL_VERSION_MIN);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::MalformedField);
    }
}

// ─── Grid messages ───────────────────────────────────────────────────────────

TEST_CASE("a GridBegin survives a round trip") {
    const auto begin = MakeGridBegin();
    const auto bytes = Encode(begin);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::GridBegin);
    const auto parsed = ParseGridBegin(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Flags == begin.Flags);
    CHECK(parsed->StreamGen == begin.StreamGen);
    CHECK(parsed->ScrollbackLen == begin.ScrollbackLen);
    CHECK(parsed->TotalLen == begin.TotalLen);
    CHECK(reader.Exhausted());
}

TEST_CASE("GridBegin wire layout is pinned by explicit bytes") {
    // flags=0x01 stream_gen=0x00000002 scrollback_len=0x00000100
    // total_len=0x00000200
    const GridBeginMessage msg{.Flags = 0x01,
                               .StreamGen = 0x00000002,
                               .ScrollbackLen = 0x0000'0100,
                               .TotalLen = 0x0000'0200};
    const auto bytes = Encode(msg);
    const std::vector<std::byte> expected{
        std::byte{0x16},  // type = GridBegin
        std::byte{0x01},  // flags
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x02},  // stream_gen
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00},  // scrollback_len
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x02}, std::byte{0x00},  // total_len
    };
    CHECK(std::ranges::equal(bytes, expected));
}

TEST_CASE("GridBegin rejects reserved flag bits both ways") {
    // Bit 0 = alt-screen (defined). Bits 1–7 are reserved.
    for (std::uint8_t bit = 1; bit <= 7; ++bit) {
        CAPTURE(bit);
        const auto badFlags = static_cast<std::uint8_t>(1U << bit);

        // Write side: refuses before producing any bytes.
        {
            auto begin = MakeGridBegin();
            begin.Flags = badFlags;
            std::vector<std::byte> out(MAX_FRAME);
            ByteWriter w{MutableByteSpan{out}};
            const auto written = WriteGridBegin(w, begin);
            REQUIRE_FALSE(written.has_value());
            CHECK(written.error() == EProtoError::MalformedField);
            CHECK(w.Size() == 0);
        }

        // Parse side: hand-crafted bytes with the bad flag byte.
        // type(1) already consumed; body = flags(1) + stream_gen(4) +
        // scrollback_len(4) + total_len(4) with valid values except flags.
        {
            const std::vector<std::byte> body{
                std::byte{badFlags},  // flags (reserved bit set)
                std::byte{0},        std::byte{0},
                std::byte{0},        std::byte{1},  // stream_gen
                std::byte{0},        std::byte{0},
                std::byte{0},        std::byte{0},  // scrollback_len
                std::byte{0},        std::byte{0},
                std::byte{0},        std::byte{1},  // total_len
            };
            ByteReader reader{ByteSpan{body}};
            const auto parsed = ParseGridBegin(reader);
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error() == EProtoError::MalformedField);
        }
    }
}

// Helper: a valid GridBegin body (13 bytes, no type prefix) with a
// configurable total_len field.
static std::vector<std::byte> GridBeginBody(std::uint32_t totalLen) {
    std::vector<std::byte> body;
    body.push_back(std::byte{0x01});  // flags = alt-screen
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x01});  // stream_gen=1
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});  // scrollback_len=0
    body.push_back(std::byte{static_cast<std::uint8_t>(totalLen >> 24U)});
    body.push_back(
        std::byte{static_cast<std::uint8_t>((totalLen >> 16U) & 0xFF)});
    body.push_back(
        std::byte{static_cast<std::uint8_t>((totalLen >> 8U) & 0xFF)});
    body.push_back(std::byte{static_cast<std::uint8_t>(totalLen & 0xFF)});
    return body;
}

TEST_CASE("GridBegin rejects zero total_len both ways") {
    // Write side.
    {
        GridBeginMessage begin = MakeGridBegin();
        begin.TotalLen = 0;
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteGridBegin(writer, begin);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::LengthLimitExceeded);
        CHECK(writer.Size() == 0);
    }

    // Parse side: hand-crafted body with total_len=0.
    {
        const auto body = GridBeginBody(0);
        ByteReader reader{ByteSpan{body}};
        const auto parsed = ParseGridBegin(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
    }
}

TEST_CASE("GridBegin rejects total_len above SNAPSHOT_MAX_BYTES both ways") {
    // Write side.
    {
        GridBeginMessage begin = MakeGridBegin();
        begin.TotalLen = SNAPSHOT_MAX_BYTES + 1;
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteGridBegin(writer, begin);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::LengthLimitExceeded);
        CHECK(writer.Size() == 0);
    }

    // Parse side: hand-crafted body with total_len = SNAPSHOT_MAX_BYTES + 1.
    {
        const auto body = GridBeginBody(SNAPSHOT_MAX_BYTES + 1);
        ByteReader reader{ByteSpan{body}};
        const auto parsed = ParseGridBegin(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
    }
}

TEST_CASE("GridBegin accepts total_len exactly at SNAPSHOT_MAX_BYTES") {
    const auto body = GridBeginBody(SNAPSHOT_MAX_BYTES);
    ByteReader reader{ByteSpan{body}};
    const auto parsed = ParseGridBegin(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->TotalLen == SNAPSHOT_MAX_BYTES);
}

TEST_CASE("a GridChunk survives a round trip") {
    auto [storage, msg] = MakeGridChunkStorage();
    msg.Data = ByteSpan{storage};
    const auto bytes = Encode(msg);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::GridChunk);
    const auto parsed = ParseGridChunk(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ChunkOffset == msg.ChunkOffset);
    REQUIRE(parsed->Data.size() == storage.size());
    CHECK(std::ranges::equal(parsed->Data, storage));
    CHECK(reader.Exhausted());
}

TEST_CASE("GridChunk rejects empty data") {
    GridChunkMessage msg{.ChunkOffset = 0, .Data = ByteSpan{}};
    std::vector<std::byte> out(MAX_FRAME);
    ByteWriter writer{MutableByteSpan{out}};
    const auto written = WriteGridChunk(writer, msg);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == EProtoError::MalformedField);
    CHECK(writer.Size() == 0);
}

TEST_CASE("GridChunk rejects data above GRID_CHUNK_MAX_DATA") {
    // On the write side: oversized data is refused before any bytes are
    // written.
    const std::vector<std::byte> big(GRID_CHUNK_MAX_DATA + 1);
    // Provide a large enough output buffer so the size check, not the buffer,
    // triggers the failure.
    std::vector<std::byte> out(MAX_FRAME + GRID_CHUNK_MAX_DATA + 1);
    ByteWriter writer{MutableByteSpan{out}};
    const auto written =
        WriteGridChunk(writer, GridChunkMessage{.Data = ByteSpan{big}});
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == EProtoError::LengthLimitExceeded);
    CHECK(writer.Size() == 0);

    // On the parse side: craft bytes with the oversized length field.
    // type(1) + chunk_offset(4) + data.  Body reader starts after type byte.
    std::vector<std::byte> raw;
    // chunk_offset = 0, then GRID_CHUNK_MAX_DATA+1 zero bytes as payload.
    raw.push_back(std::byte{0x17});  // type
    raw.push_back(std::byte{0x00});
    raw.push_back(std::byte{0x00});
    raw.push_back(std::byte{0x00});
    raw.push_back(std::byte{0x00});  // chunk_offset = 0
    raw.resize(raw.size() + GRID_CHUNK_MAX_DATA + 1, std::byte{0x00});
    ByteReader reader{ByteSpan{raw}.subspan(1)};
    const auto parsed = ParseGridChunk(reader);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
}

TEST_CASE("a GridEnd survives a round trip") {
    const auto end = MakeGridEnd();
    const auto bytes = Encode(end);

    // type byte only.
    REQUIRE(bytes.size() == 1);
    CHECK(bytes[0] == std::byte{0x18});

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::GridEnd);
    const auto parsed = ParseGridEnd(reader);
    REQUIRE(parsed.has_value());
    CHECK(reader.Exhausted());
}

TEST_CASE("a GridApplied survives a round trip") {
    const auto applied = MakeGridApplied();
    const auto bytes = Encode(applied);

    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::GridApplied);
    const auto parsed = ParseGridApplied(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->StreamGen == applied.StreamGen);
    CHECK(reader.Exhausted());
}

TEST_CASE("GridApplied wire layout is pinned by explicit bytes") {
    const GridAppliedMessage msg{.StreamGen = 0x0A0B'0C0D};
    const auto bytes = Encode(msg);
    const std::vector<std::byte> expected{
        std::byte{0x19},  // type = GridApplied
        std::byte{0x0A}, std::byte{0x0B},
        std::byte{0x0C}, std::byte{0x0D},  // stream_gen
    };
    CHECK(std::ranges::equal(bytes, expected));
}

// ─── Protocol v2: GRID_CAPABLE TLV ───────────────────────────────────────────

TEST_CASE("GRID_CAPABLE TLV type is 0x0003 and is non-critical") {
    // §7 TLV registry: type=0x0003, non-critical (high bit clear).
    CHECK(TLV_GRID_CAPABLE == 0x0003);
    CHECK((TLV_GRID_CAPABLE & TLV_CRITICAL_BIT) == 0);
}

TEST_CASE("GRID_CAPABLE option survives a Hello round trip") {
    auto hello = MakeHello();
    REQUIRE(
        hello.Options
            .Append(TlvOption{.Type = TLV_GRID_CAPABLE, .Value = ByteSpan{}})
            .has_value());
    const auto bytes = Encode(hello);
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == EMessageType::Hello);
    const auto parsed = ParseHello(reader);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Options.Options().size() == 1);
    CHECK(parsed->Options.Options().front().Type == TLV_GRID_CAPABLE);
    CHECK_FALSE(parsed->Options.Options().front().IsCritical());
}

TEST_CASE("AppendGridCapableTlv and HasGridCapableTlv round-trip") {
    // type(2) + length(2) + value(2) = 6 bytes.
    std::vector<std::byte> buf(6);
    const auto written = AppendGridCapableTlv(MutableByteSpan{buf});
    REQUIRE(written.has_value());
    CHECK(*written == 6);

    // HasGridCapableTlv must find it.
    const auto found = HasGridCapableTlv(ByteSpan{buf}.first(*written));
    REQUIRE(found.has_value());
    CHECK(*found == true);

    // An empty span has no TLV → false.
    const auto absent = HasGridCapableTlv(ByteSpan{});
    REQUIRE(absent.has_value());
    CHECK(*absent == false);

    // A buffer smaller than 6 bytes must fail.
    std::vector<std::byte> tiny(5);
    const auto refused = AppendGridCapableTlv(MutableByteSpan{tiny});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == EProtoError::BufferTooSmall);
}

TEST_CASE("GridBegin rejects scrollback_len greater than total_len") {
    // Parse path.
    {
        // Build a valid GridBegin body but with scrollback_len > total_len.
        // Layout (no type byte): flags(1) gen(4) scrollback(4) total(4) = 13 B
        const std::vector<std::byte> body{
            std::byte{0x01},  // flags
            std::byte{0},    std::byte{0},
            std::byte{0},    std::byte{1},  // gen=1
            std::byte{0},    std::byte{0},
            std::byte{0},    std::byte{5},  // scrollback=5
            std::byte{0},    std::byte{0},
            std::byte{0},    std::byte{4},  // total=4
        };
        ByteReader reader{ByteSpan{body}};
        const auto parsed = ParseGridBegin(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
    }
    // Write path.
    {
        GridBeginMessage msg = MakeGridBegin();
        msg.ScrollbackLen = msg.TotalLen + 1;
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const auto written = WriteGridBegin(writer, msg);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::LengthLimitExceeded);
    }
}

TEST_CASE("GRID_CHUNK_MAX_DATA is the largest payload GridChunk accepts") {
    // A payload of exactly GRID_CHUNK_MAX_DATA must succeed; one byte more
    // must be rejected by both WriteGridChunk and ParseGridChunk.
    const GridChunkMessage at_limit{
        .ChunkOffset = 0,
        .Data = ByteSpan{}.first(0),  // placeholder; resized below
    };

    std::vector<std::byte> payload_max(GRID_CHUNK_MAX_DATA, std::byte{0xAB});
    std::vector<std::byte> payload_over(GRID_CHUNK_MAX_DATA + 1,
                                        std::byte{0xAB});

    // Write at limit must succeed.
    {
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const GridChunkMessage msg{.ChunkOffset = 0,
                                   .Data = ByteSpan{payload_max}};
        REQUIRE(WriteGridChunk(writer, msg).has_value());
    }
    // Write over limit must fail.
    {
        std::vector<std::byte> out(MAX_FRAME);
        ByteWriter writer{MutableByteSpan{out}};
        const GridChunkMessage msg{.ChunkOffset = 0,
                                   .Data = ByteSpan{payload_over}};
        const auto written = WriteGridChunk(writer, msg);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error() == EProtoError::LengthLimitExceeded);
    }
    // Parse over limit: build a frame manually with an oversized payload.
    {
        // body = offset(4) + payload(GRID_CHUNK_MAX_DATA + 1)
        std::vector<std::byte> body(4 + GRID_CHUNK_MAX_DATA + 1, std::byte{0});
        ByteReader reader{ByteSpan{body}};
        const auto parsed = ParseGridChunk(reader);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == EProtoError::LengthLimitExceeded);
    }
}
