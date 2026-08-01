#include "zet/core/byte_writer.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "zet/core/byte_reader.hpp"

namespace {

using zet::ByteReader;
using zet::ByteSpan;
using zet::ByteWriter;
using zet::MutableByteSpan;
using zet::EProtoError;

}  // namespace

TEST_CASE("writes big-endian integers") {
    std::array<std::byte, 8> buf{};
    ByteWriter w{MutableByteSpan{buf}};

    REQUIRE(w.WriteU16BE(0x1234).has_value());
    REQUIRE(w.WriteU32BE(0xAABBCCDDU).has_value());

    CHECK(std::to_integer<int>(buf[0]) == 0x12);
    CHECK(std::to_integer<int>(buf[1]) == 0x34);
    CHECK(std::to_integer<int>(buf[2]) == 0xAA);
    CHECK(std::to_integer<int>(buf[5]) == 0xDD);
    CHECK(w.Size() == 6);
    CHECK(w.Remaining() == 2);
}

TEST_CASE("a write that does not fit is rejected and changes nothing") {
    std::array<std::byte, 3> buf{};
    ByteWriter w{MutableByteSpan{buf}};

    REQUIRE(w.WriteU8(0xFF).has_value());
    const std::size_t before = w.Size();

    CHECK(w.WriteU32BE(1).error() == EProtoError::BufferTooSmall);
    CHECK(w.Size() == before);
    CHECK(std::to_integer<int>(buf[1]) == 0);
    CHECK(std::to_integer<int>(buf[2]) == 0);

    // The writer stays usable for something that does fit.
    CHECK(w.WriteU16BE(0x0102).has_value());
    CHECK(w.Size() == 3);
    CHECK(w.Remaining() == 0);
}

TEST_CASE("a zero-capacity writer accepts nothing but does not misbehave") {
    ByteWriter w{MutableByteSpan{}};

    CHECK(w.Capacity() == 0);
    CHECK(w.WriteU8(1).error() == EProtoError::BufferTooSmall);
    CHECK(w.WriteBytes(ByteSpan{}).has_value());
    CHECK(w.Written().empty());
}

TEST_CASE("reserve hands out a slot to be filled in place") {
    std::array<std::byte, 4> buf{};
    ByteWriter w{MutableByteSpan{buf}};

    auto slot = w.Reserve(2);
    REQUIRE(slot.has_value());
    CHECK(slot->size() == 2);
    (*slot)[0] = std::byte{0x77};
    (*slot)[1] = std::byte{0x88};

    REQUIRE(w.WriteU16BE(0x9900).has_value());
    CHECK(w.Size() == 4);
    CHECK(std::to_integer<int>(buf[0]) == 0x77);
    CHECK(std::to_integer<int>(buf[2]) == 0x99);
    CHECK(w.Reserve(1).error() == EProtoError::BufferTooSmall);
}

TEST_CASE("what the writer produces the reader gets back unchanged") {
    std::array<std::byte, 15> buf{};
    ByteWriter w{MutableByteSpan{buf}};

    const std::array<std::byte, 3> payload{std::byte{0xCA}, std::byte{0xFE},
                                           std::byte{0xBA}};

    REQUIRE(w.WriteU8(0x2A).has_value());
    REQUIRE(w.WriteU16BE(0xBEEF).has_value());
    REQUIRE(w.WriteU32BE(0x01020304U).has_value());
    REQUIRE(w.WriteBytes(ByteSpan{payload}).has_value());
    CHECK(w.Size() == 10);

    ByteReader r{w.Written()};
    CHECK(r.ReadU8().value() == 0x2A);
    CHECK(r.ReadU16BE().value() == 0xBEEF);
    CHECK(r.ReadU32BE().value() == 0x01020304U);

    const auto tail = r.ReadBytes(3);
    REQUIRE(tail.has_value());
    CHECK(std::to_integer<int>((*tail)[0]) == 0xCA);
    CHECK(std::to_integer<int>((*tail)[2]) == 0xBA);
    CHECK(r.Exhausted());
}

TEST_CASE("written() reports only the bytes actually produced") {
    std::array<std::byte, 16> buf{};
    ByteWriter w{MutableByteSpan{buf}};

    CHECK(w.Written().empty());
    REQUIRE(w.WriteU64BE(0).has_value());
    CHECK(w.Written().size() == 8);
    CHECK(w.Capacity() == 16);
}
