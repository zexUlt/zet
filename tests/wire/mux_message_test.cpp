/// The local socket between zet-muxd and the agents, and what the mux reads
/// off a TCP connection before it knows whose it is (design.md §6).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

/// Room for any message here: a handoff with the largest prefix.
constexpr std::size_t ENCODE_ROOM = 2 * MAX_HANDOFF_PREFIX;

template <typename TWrite>
std::vector<std::byte> Encode(TWrite&& write) {
    std::vector<std::byte> out(ENCODE_ROOM);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(write(writer).has_value());
    out.resize(writer.Size());
    return out;
}

/// The reader positioned after the type byte, which must be `type`.
ByteReader Body(const std::vector<std::byte>& bytes, EMessageType type) {
    ByteReader reader{ByteSpan{bytes}};
    REQUIRE(ParseMessageType(reader) == type);
    return reader;
}

crypto::Key Registration(std::uint8_t seed) {
    crypto::Key key;
    for (std::size_t i = 0; i < key.Expose().size(); ++i) {
        key.Expose()[i] = static_cast<std::byte>(seed + i);
    }
    return key;
}

/// A cleartext frame around `body`, as the client sends its Hello.
std::vector<std::byte> Frame(const std::vector<std::byte>& body,
                             std::uint8_t epoch = 0) {
    std::vector<std::byte> out(HEADER_SIZE + body.size());
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(WriteFrameHeader(
        writer, FrameHeader{.Length = static_cast<std::uint32_t>(body.size()),
                            .Epoch = epoch}));
    REQUIRE(writer.WriteBytes(ByteSpan{body}));
    return out;
}

HelloMessage MakeHello() {
    HelloMessage hello{};
    hello.VersionMin = 1;
    hello.VersionMax = 1;
    for (std::size_t i = 0; i < hello.Id.size(); ++i) {
        hello.Id[i] = static_cast<std::byte>(0xA0 + i);
    }
    return hello;
}

std::vector<std::byte> HelloFrame(const HelloMessage& hello) {
    return Frame(Encode([&](ByteWriter& w) { return WriteHello(w, hello); }));
}

}  // namespace

TEST_CASE("a registration survives a round trip and stays a secret") {
    const MuxRegisterMessage message{.Registration = Registration(3)};
    const auto bytes =
        Encode([&](ByteWriter& w) { return WriteMuxRegister(w, message); });
    CHECK(bytes.front() == std::byte{0x30});
    auto reader = Body(bytes, EMessageType::MuxRegister);
    const auto parsed = ParseMuxRegister(reader);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Registration.Expose() == message.Registration.Expose());
}

TEST_CASE("a registration with bytes past R or short of it is refused") {
    const MuxRegisterMessage message{.Registration = Registration(3)};
    auto bytes =
        Encode([&](ByteWriter& w) { return WriteMuxRegister(w, message); });
    auto longer = bytes;
    longer.push_back(std::byte{0});
    auto reader = Body(longer, EMessageType::MuxRegister);
    CHECK(ParseMuxRegister(reader).error() == EProtoError::MalformedField);

    bytes.pop_back();
    auto shorter = Body(bytes, EMessageType::MuxRegister);
    CHECK(ParseMuxRegister(shorter).error() == EProtoError::Truncated);
}

TEST_CASE("registered names the port; port zero is refused both ways") {
    const auto bytes = Encode([](ByteWriter& w) {
        return WriteMuxRegistered(w, MuxRegisteredMessage{.Port = 2022});
    });
    auto reader = Body(bytes, EMessageType::MuxRegistered);
    CHECK(ParseMuxRegistered(reader) == MuxRegisteredMessage{.Port = 2022});

    std::vector<std::byte> out(ENCODE_ROOM);
    ByteWriter writer{MutableByteSpan{out}};
    CHECK(WriteMuxRegistered(writer, MuxRegisteredMessage{.Port = 0}).error() ==
          EProtoError::MalformedField);

    auto zero = bytes;
    zero.at(1) = std::byte{0};
    zero.at(2) = std::byte{0};
    auto zeroReader = Body(zero, EMessageType::MuxRegistered);
    CHECK(ParseMuxRegistered(zeroReader).error() ==
          EProtoError::MalformedField);

    auto longer = bytes;
    longer.push_back(std::byte{0});
    auto longReader = Body(longer, EMessageType::MuxRegistered);
    CHECK(ParseMuxRegistered(longReader).error() ==
          EProtoError::MalformedField);
}

TEST_CASE("a refusal carries one of its reasons and nothing else") {
    for (const auto reason :
         {EMuxRefusal::Taken, EMuxRefusal::Limit, EMuxRefusal::Malformed}) {
        CAPTURE(static_cast<int>(reason));
        const auto bytes = Encode([&](ByteWriter& w) {
            return WriteMuxRefused(w, MuxRefusedMessage{.Reason = reason});
        });
        auto reader = Body(bytes, EMessageType::MuxRefused);
        CHECK(ParseMuxRefused(reader) == MuxRefusedMessage{.Reason = reason});
    }
    for (const std::uint8_t reason : std::array<std::uint8_t, 3>{0, 4, 0xFF}) {
        CAPTURE(reason);
        const std::vector<std::byte> bytes{std::byte{0x32},
                                           static_cast<std::byte>(reason)};
        auto reader = Body(bytes, EMessageType::MuxRefused);
        CHECK(ParseMuxRefused(reader).error() == EProtoError::MalformedField);

        std::vector<std::byte> out(ENCODE_ROOM);
        ByteWriter writer{MutableByteSpan{out}};
        CHECK(WriteMuxRefused(
                  writer,
                  MuxRefusedMessage{.Reason = static_cast<EMuxRefusal>(reason)})
                  .error() == EProtoError::MalformedField);
    }
    const std::vector<std::byte> empty{std::byte{0x32}};
    auto emptyReader = Body(empty, EMessageType::MuxRefused);
    CHECK(ParseMuxRefused(emptyReader).error() == EProtoError::Truncated);
    const std::vector<std::byte> longer{std::byte{0x32}, std::byte{1},
                                        std::byte{0}};
    auto longReader = Body(longer, EMessageType::MuxRefused);
    CHECK(ParseMuxRefused(longReader).error() == EProtoError::MalformedField);
}

TEST_CASE("a handoff carries its prefix whole") {
    const auto hello = HelloFrame(MakeHello());
    const auto bytes = Encode([&](ByteWriter& w) {
        return WriteMuxHandoff(w, MuxHandoffMessage{.Prefix = ByteSpan{hello}});
    });
    CHECK(bytes.size() == 1 + sizeof(std::uint16_t) + hello.size());
    auto reader = Body(bytes, EMessageType::MuxHandoff);
    const auto parsed = ParseMuxHandoff(reader);
    REQUIRE(parsed.has_value());
    CHECK(std::ranges::equal(parsed->Prefix, hello));

    // The largest a mux could have read, and one past it.
    const std::vector<std::byte> most(MAX_HANDOFF_PREFIX, std::byte{7});
    const auto full = Encode([&](ByteWriter& w) {
        return WriteMuxHandoff(w, MuxHandoffMessage{.Prefix = ByteSpan{most}});
    });
    auto fullReader = Body(full, EMessageType::MuxHandoff);
    CHECK(ParseMuxHandoff(fullReader).has_value());

    const std::vector<std::byte> past(MAX_HANDOFF_PREFIX + 1, std::byte{7});
    std::vector<std::byte> out(ENCODE_ROOM);
    ByteWriter writer{MutableByteSpan{out}};
    CHECK(WriteMuxHandoff(writer, MuxHandoffMessage{.Prefix = ByteSpan{past}})
              .error() == EProtoError::MalformedField);
    ByteWriter emptyWriter{MutableByteSpan{out}};
    CHECK(WriteMuxHandoff(emptyWriter, MuxHandoffMessage{}).error() ==
          EProtoError::MalformedField);
}

TEST_CASE("a handoff whose length lies is refused") {
    const auto hello = HelloFrame(MakeHello());
    const auto bytes = Encode([&](ByteWriter& w) {
        return WriteMuxHandoff(w, MuxHandoffMessage{.Prefix = ByteSpan{hello}});
    });
    SUBCASE("shorter than it says") {
        auto cut = bytes;
        cut.pop_back();
        auto reader = Body(cut, EMessageType::MuxHandoff);
        CHECK(ParseMuxHandoff(reader).error() == EProtoError::Truncated);
    }
    SUBCASE("longer than it says") {
        auto longer = bytes;
        longer.push_back(std::byte{0});
        auto reader = Body(longer, EMessageType::MuxHandoff);
        CHECK(ParseMuxHandoff(reader).error() == EProtoError::MalformedField);
    }
    SUBCASE("zero or past the limit") {
        for (const std::uint16_t length :
             {std::uint16_t{0},
              static_cast<std::uint16_t>(MAX_HANDOFF_PREFIX + 1)}) {
            CAPTURE(length);
            std::vector<std::byte> lying{
                std::byte{0x33}, static_cast<std::byte>(length >> 8U),
                static_cast<std::byte>(length & 0xFFU)};
            auto reader = Body(lying, EMessageType::MuxHandoff);
            CHECK(ParseMuxHandoff(reader).error() ==
                  EProtoError::MalformedField);
        }
    }
    SUBCASE("no length at all") {
        const std::vector<std::byte> bare{std::byte{0x33}, std::byte{0}};
        auto reader = Body(bare, EMessageType::MuxHandoff);
        CHECK(ParseMuxHandoff(reader).error() == EProtoError::Truncated);
    }
}

TEST_CASE("PeekHello reads the sid and the frame's size, and nothing else") {
    HelloMessage hello = MakeHello();
    const auto frame = HelloFrame(hello);
    const auto peek = PeekHello(ByteSpan{frame});
    REQUIRE(peek.has_value());
    CHECK(peek->Id == hello.Id);
    CHECK(peek->FrameSize == frame.size());

    // Bytes past the frame are not the Hello's.
    auto followed = frame;
    followed.push_back(std::byte{0x42});
    const auto more = PeekHello(ByteSpan{followed});
    REQUIRE(more.has_value());
    CHECK(more->FrameSize == frame.size());
}

TEST_CASE("PeekHello waits for the whole frame") {
    const auto frame = HelloFrame(MakeHello());
    for (std::size_t cut = 0; cut < frame.size(); ++cut) {
        CAPTURE(cut);
        CHECK(PeekHello(ByteSpan{frame}.first(cut)).error() ==
              EProtoError::Truncated);
    }
}

TEST_CASE("PeekHello refuses what is not a cleartext Hello") {
    const auto hello = MakeHello();
    const auto body =
        Encode([&](ByteWriter& w) { return WriteHello(w, hello); });

    SUBCASE("a sealed frame") {
        CHECK(PeekHello(ByteSpan{Frame(body, 1)}).error() ==
              EProtoError::UnexpectedMessage);
    }
    SUBCASE("another message") {
        const auto resize = Frame(Encode([](ByteWriter& w) {
            return WriteMuxRegistered(w, MuxRegisteredMessage{.Port = 1});
        }));
        CHECK(PeekHello(ByteSpan{resize}).error() ==
              EProtoError::UnexpectedMessage);
    }
    SUBCASE("an unknown type") {
        CHECK(PeekHello(ByteSpan{Frame({std::byte{0xEE}})}).error() ==
              EProtoError::UnknownMessageType);
    }
    SUBCASE("an empty body") {
        // A frame must carry a body at all (§7).
        CHECK(PeekHello(ByteSpan{Frame({})}).error() ==
              EProtoError::MalformedField);
    }
    SUBCASE("the wrong magic") {
        auto bad = body;
        bad.at(1) = std::byte{'Y'};
        CHECK(PeekHello(ByteSpan{Frame(bad)}).error() ==
              EProtoError::MalformedField);
    }
    SUBCASE("a range that runs backwards") {
        HelloMessage backwards = hello;
        backwards.VersionMin = 2;
        backwards.VersionMax = 2;
        auto bad =
            Encode([&](ByteWriter& w) { return WriteHello(w, backwards); });
        // ver_min is the two bytes after the magic.
        bad.at(HELLO_PREFIX_SIZE - 3) = std::byte{3};
        CHECK(PeekHello(ByteSpan{Frame(bad)}).error() ==
              EProtoError::MalformedField);
    }
    SUBCASE("cut before the sid ends") {
        const std::vector<std::byte> cut(
            body.begin(),
            body.begin() + static_cast<std::ptrdiff_t>(HELLO_PREFIX_SIZE + 3));
        CHECK(PeekHello(ByteSpan{Frame(cut)}).error() ==
              EProtoError::Truncated);
    }
    SUBCASE("cut inside the version range") {
        const std::vector<std::byte> cut(body.begin(), body.begin() + 6);
        CHECK(PeekHello(ByteSpan{Frame(cut)}).error() ==
              EProtoError::Truncated);
    }
    SUBCASE("cut inside the magic or the second version") {
        for (const std::size_t keep : {std::size_t{3}, std::size_t{8}}) {
            CAPTURE(keep);
            const std::vector<std::byte> cut(
                body.begin(), body.begin() + static_cast<std::ptrdiff_t>(keep));
            CHECK(PeekHello(ByteSpan{Frame(cut)}).error() ==
                  EProtoError::Truncated);
        }
    }
    SUBCASE("a frame longer than a handshake may be") {
        std::vector<std::byte> header(HEADER_SIZE);
        ByteWriter writer{MutableByteSpan{header}};
        REQUIRE(WriteFrameHeader(
            writer,
            FrameHeader{.Length = MAX_HANDSHAKE_FRAME + 1, .Epoch = 0}));
        CHECK(PeekHello(ByteSpan{header}).error() ==
              EProtoError::LengthLimitExceeded);
    }
}

TEST_CASE("the mux range is known; the type after it is not") {
    for (const std::uint8_t type :
         std::array<std::uint8_t, 4>{0x30, 0x31, 0x32, 0x33}) {
        CAPTURE(type);
        const std::vector<std::byte> bytes{static_cast<std::byte>(type)};
        ByteReader reader{ByteSpan{bytes}};
        CHECK(ParseMessageType(reader).has_value());
    }
    const std::vector<std::byte> next{std::byte{0x34}};
    ByteReader reader{ByteSpan{next}};
    CHECK(ParseMessageType(reader).error() == EProtoError::UnknownMessageType);
}
