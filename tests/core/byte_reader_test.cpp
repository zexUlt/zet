#include "zet/core/byte_reader.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using zet::ByteReader;
using zet::ByteSpan;
using zet::EProtoError;

ByteSpan SpanOf(const std::vector<std::uint8_t>& v) {
    return ByteSpan{reinterpret_cast<const std::byte*>(v.data()), v.size()};
}

}  // namespace

TEST_CASE("reads big-endian integers independently of host byte order") {
    const std::vector<std::uint8_t> bytes{0x12, 0x34, 0x56, 0x78,
                                          0x9A, 0xBC, 0xDE, 0xF0};
    ByteReader r{SpanOf(bytes)};

    SUBCASE("u8") {
        CHECK(r.ReadU8().value() == 0x12);
        CHECK(r.Offset() == 1);
    }
    SUBCASE("u16") {
        CHECK(r.ReadU16BE().value() == 0x1234);
        CHECK(r.Offset() == 2);
    }
    SUBCASE("u32") {
        CHECK(r.ReadU32BE().value() == 0x12345678U);
        CHECK(r.Offset() == 4);
    }
    SUBCASE("u64") {
        CHECK(r.ReadU64BE().value() == 0x123456789ABCDEF0ULL);
        CHECK(r.Offset() == 8);
    }
}

TEST_CASE("an empty buffer yields truncated, never undefined behaviour") {
    ByteReader r{ByteSpan{}};

    CHECK(r.ReadU8().error() == EProtoError::Truncated);
    CHECK(r.ReadU32BE().error() == EProtoError::Truncated);
    CHECK(r.ReadBytes(1).error() == EProtoError::Truncated);
    CHECK(r.Exhausted());
    CHECK(r.Remaining() == 0);
}

TEST_CASE("a one-byte buffer cannot be read as a wider field") {
    // This is the EternalTerminal crash in miniature: its Packet constructor
    // indexed [0] and [1] and then called substr(2) on a length-1 string,
    // throwing out_of_range past a catch(runtime_error) and terminating the
    // root daemon. Here the same input is an ordinary error value.
    const std::vector<std::uint8_t> one{0x41};
    ByteReader r{SpanOf(one)};

    CHECK(r.ReadU16BE().error() == EProtoError::Truncated);
    CHECK(r.ReadBytes(2).error() == EProtoError::Truncated);
    CHECK(r.Offset() == 0);
    CHECK(r.ReadU8().value() == 0x41);
}

TEST_CASE("a failed read consumes nothing") {
    const std::vector<std::uint8_t> bytes{0xAA, 0xBB, 0xCC};
    ByteReader r{SpanOf(bytes)};

    REQUIRE(r.ReadU8().has_value());
    const std::size_t before = r.Offset();

    CHECK_FALSE(r.ReadU32BE().has_value());
    CHECK(r.Offset() == before);
    CHECK_FALSE(r.ReadBytes(99).has_value());
    CHECK(r.Offset() == before);
    CHECK_FALSE(r.Skip(99).has_value());
    CHECK(r.Offset() == before);

    // Still usable: the rest decodes as if nothing had been attempted.
    CHECK(r.ReadU16BE().value() == 0xBBCC);
    CHECK(r.Exhausted());
}

TEST_CASE("ReadBytes borrows a view of the original buffer") {
    const std::vector<std::uint8_t> bytes{1, 2, 3, 4, 5};
    ByteReader r{SpanOf(bytes)};

    REQUIRE(r.Skip(1).has_value());
    const auto taken = r.ReadBytes(3);
    REQUIRE(taken.has_value());
    CHECK(taken->size() == 3);
    CHECK(taken->data() ==
          reinterpret_cast<const std::byte*>(bytes.data()) + 1);
    CHECK(r.Remaining() == 1);
    CHECK(r.Rest().size() == 1);
}

TEST_CASE("reading exactly to the end succeeds and exhausts the reader") {
    const std::vector<std::uint8_t> bytes{0, 0, 0, 7};
    ByteReader r{SpanOf(bytes)};

    CHECK(r.ReadU32BE().value() == 7U);
    CHECK(r.Exhausted());
    CHECK(r.ReadU8().error() == EProtoError::Truncated);
}

TEST_CASE("a zero-length read is legal at any position, including the end") {
    const std::vector<std::uint8_t> bytes{9};
    ByteReader r{SpanOf(bytes)};

    CHECK(r.ReadBytes(0).value().empty());
    REQUIRE(r.ReadU8().has_value());
    CHECK(r.ReadBytes(0).value().empty());
    CHECK(r.Skip(0).has_value());
}

TEST_CASE("every truncation point of a well-formed message is rejected") {
    // Property: a decoder fed any strict prefix of a valid message must fail
    // cleanly rather than read past the end. Cheap to state here, and the same
    // shape will carry over to whole frames later.
    const std::vector<std::uint8_t> full{0xDE, 0xAD, 0xBE, 0xEF,
                                         0x01, 0x02, 0x03, 0x04};

    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        const std::vector<std::uint8_t> prefix(
            full.begin(), full.begin() + static_cast<long>(cut));
        ByteReader r{SpanOf(prefix)};
        CAPTURE(cut);
        CHECK_FALSE(r.ReadU64BE().has_value());
        CHECK(r.Offset() == 0);
    }

    ByteReader whole{SpanOf(full)};
    CHECK(whole.ReadU64BE().value() == 0xDEADBEEF01020304ULL);
}
