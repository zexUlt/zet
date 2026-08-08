#include "zet/wire/frame.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/byte_writer.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

std::vector<std::byte> Frame(std::uint32_t declaredLength, std::uint8_t epoch,
                             std::size_t bodyBytes) {
    std::vector<std::byte> out(HEADER_SIZE + bodyBytes);
    ByteWriter writer{MutableByteSpan{out}};
    REQUIRE(writer.WriteU32BE(declaredLength).has_value());
    REQUIRE(writer.WriteU8(epoch).has_value());
    for (std::size_t i = 0; i < bodyBytes; ++i) {
        REQUIRE(writer.WriteU8(static_cast<std::uint8_t>(i)).has_value());
    }
    return out;
}

}  // namespace

TEST_CASE("header survives a round trip") {
    std::array<std::byte, HEADER_SIZE> buffer{};
    ByteWriter writer{MutableByteSpan{buffer}};

    const FrameHeader written{.Length = 0x0001'0203, .Epoch = 0x7F};
    REQUIRE(WriteFrameHeader(writer, written).has_value());

    ByteReader reader{ByteSpan{buffer}};
    const auto read = ReadFrameHeader(reader);
    REQUIRE(read.has_value());
    CHECK(read->Length == written.Length);
    CHECK(read->Epoch == written.Epoch);
    CHECK(reader.Exhausted());
}

TEST_CASE("length is big endian on the wire") {
    std::array<std::byte, HEADER_SIZE> buffer{};
    ByteWriter writer{MutableByteSpan{buffer}};
    REQUIRE(WriteFrameHeader(writer, FrameHeader{.Length = 1, .Epoch = 0})
                .has_value());

    CHECK(buffer[0] == std::byte{0});
    CHECK(buffer[3] == std::byte{1});
}

TEST_CASE("a whole frame is read and its body is a view on the input") {
    const auto bytes = Frame(4, 9, 4);
    const auto frame = ReadFrame(ByteSpan{bytes}, MAX_FRAME);

    REQUIRE(frame.has_value());
    CHECK(frame->Header.Length == 4);
    CHECK(frame->Header.Epoch == 9);
    CHECK(frame->Body.size() == 4);
    CHECK(frame->Consumed == HEADER_SIZE + 4);
    CHECK(frame->Body.data() == bytes.data() + HEADER_SIZE);
}

TEST_CASE("trailing bytes after a frame are left alone") {
    auto bytes = Frame(2, 0, 2);
    bytes.push_back(std::byte{0xAA});

    const auto frame = ReadFrame(ByteSpan{bytes}, MAX_FRAME);
    REQUIRE(frame.has_value());
    CHECK(frame->Consumed == HEADER_SIZE + 2);
    CHECK(frame->Consumed < bytes.size());
}

TEST_CASE(
    "a frame arriving one byte at a time reports truncation until complete") {
    const auto bytes = Frame(3, 1, 3);

    for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
        const auto partial =
            ReadFrame(ByteSpan{bytes}.first(prefix), MAX_FRAME);
        REQUIRE_FALSE(partial.has_value());
        CHECK(partial.error() == EProtoError::Truncated);
        CHECK(DispositionOf(partial.error()) == EDisposition::CloseConnection);
    }

    CHECK(ReadFrame(ByteSpan{bytes}, MAX_FRAME).has_value());
}

TEST_CASE("declared length above the stage limit is refused") {
    // Тело не выделяется: в буфере только заголовок, и его достаточно, чтобы
    // отказать. Проверка длины обязана идти раньше чтения тела.
    const auto bytes = Frame(MAX_FRAME + 1, 0, 0);

    const auto frame = ReadFrame(ByteSpan{bytes}, MAX_FRAME);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == EProtoError::LengthLimitExceeded);
}

TEST_CASE("the handshake limit is tighter than the post-auth one") {
    const auto bytes = Frame(MAX_HANDSHAKE_FRAME + 1, 0, 0);

    CHECK(ReadFrame(ByteSpan{bytes}, MAX_HANDSHAKE_FRAME).error() ==
          EProtoError::LengthLimitExceeded);
    CHECK(ReadFrame(ByteSpan{bytes}, MAX_FRAME).error() ==
          EProtoError::Truncated);
}

TEST_CASE("a frame with an empty body is refused") {
    const auto bytes = Frame(0, 0, 0);

    const auto frame = ReadFrame(ByteSpan{bytes}, MAX_FRAME);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == EProtoError::MalformedField);
}

TEST_CASE("a length of 0xFFFFFFFF does not wrap the size computation") {
    const auto bytes = Frame(0xFFFF'FFFF, 0, 0);

    const auto frame = ReadFrame(ByteSpan{bytes}, MAX_FRAME);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == EProtoError::LengthLimitExceeded);
}

TEST_CASE("an empty buffer is truncation, not a crash") {
    const auto frame = ReadFrame(ByteSpan{}, MAX_FRAME);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == EProtoError::Truncated);
}

TEST_CASE(
    "writing a header into too small a buffer fails instead of overflowing") {
    std::array<std::byte, HEADER_SIZE - 1> buffer{};
    ByteWriter writer{MutableByteSpan{buffer}};

    const auto written =
        WriteFrameHeader(writer, FrameHeader{.Length = 1, .Epoch = 0});
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == EProtoError::BufferTooSmall);
}
