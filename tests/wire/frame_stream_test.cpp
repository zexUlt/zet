#include "zet/wire/frame_stream.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "zet/core/byte_writer.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

/// A frame taken off the stream, kept by value: the runs being compared have
/// long moved the buffer on by the time the comparison happens.
struct DecodedFrame {
    std::uint32_t Length{0};
    std::uint8_t Epoch{0};
    std::vector<std::byte> Body;

    bool operator==(const DecodedFrame&) const = default;
};

void EncodeHeader(std::vector<std::byte>& out, std::uint32_t declaredLength,
                  std::uint8_t epoch) {
    std::array<std::byte, HEADER_SIZE> header{};
    ByteWriter writer{MutableByteSpan{header}};
    REQUIRE(WriteFrameHeader(
                writer, FrameHeader{.Length = declaredLength, .Epoch = epoch})
                .has_value());
    out.insert(out.end(), header.begin(), header.end());
}

/// Appends a whole frame whose body starts at `seed`, so that two frames of
/// the same length still differ.
void EncodeFrame(std::vector<std::byte>& out, std::uint32_t bodyLength,
                 std::uint8_t epoch, std::uint8_t seed) {
    EncodeHeader(out, bodyLength, epoch);
    for (std::uint32_t i = 0; i < bodyLength; ++i) {
        out.push_back(static_cast<std::byte>(seed + i));
    }
}

void CollectFrames(FrameStream& stream, std::vector<DecodedFrame>& out) {
    while (true) {
        const auto next = stream.Next();
        REQUIRE(next.has_value());
        if (!next->has_value()) {
            return;
        }
        out.push_back(DecodedFrame{
            .Length = (*next)->Header.Length,
            .Epoch = (*next)->Header.Epoch,
            .Body = std::vector<std::byte>{(*next)->Body.begin(),
                                           (*next)->Body.end()},
        });
    }
}

/// Feeds `bytes` in the pieces `nextChunk` asks for and returns every frame
/// that came out. A chunk is always clipped to what the stream still has room
/// for, which is what a caller reading off a socket would do — the largest
/// single piece anyone can hand over is one capacity.
template <typename TChunker>
std::vector<DecodedFrame> DrainChunked(ByteSpan bytes,
                                       std::uint32_t maxBodyLength,
                                       TChunker nextChunk) {
    std::vector<std::byte> storage(HEADER_SIZE + maxBodyLength);
    FrameStream stream{MutableByteSpan{storage}, maxBodyLength};

    std::vector<DecodedFrame> out;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t take =
            std::min({std::max<std::size_t>(nextChunk(), 1),
                      bytes.size() - offset, stream.Room()});
        REQUIRE(take > 0);
        REQUIRE(stream.Append(bytes.subspan(offset, take)).has_value());
        offset += take;
        CollectFrames(stream, out);
    }
    return out;
}

}  // namespace

TEST_CASE("every chunking of a stream yields the same frames") {
    constexpr std::uint32_t limit = MAX_HANDSHAKE_FRAME;

    // The shortest body a frame may carry, a few middling ones, and two that
    // sit right against the stage limit — the last of them fills the buffer
    // exactly, header included.
    const std::array<std::uint32_t, 6> lengths{1, 2, 3, 300, limit - 1, limit};

    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        EncodeFrame(bytes, lengths.at(i), static_cast<std::uint8_t>(i),
                    static_cast<std::uint8_t>(0x10 + i));
    }

    const auto reference =
        DrainChunked(ByteSpan{bytes}, limit, [&] { return bytes.size(); });

    REQUIRE(reference.size() == lengths.size());
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        CHECK(reference.at(i).Length == lengths.at(i));
        CHECK(reference.at(i).Epoch == i);
        CHECK(reference.at(i).Body.size() == lengths.at(i));
    }

    for (const std::size_t chunkSize :
         std::array<std::size_t, 6>{1, 2, 3, 7, 64, 1000}) {
        CAPTURE(chunkSize);
        CHECK(DrainChunked(ByteSpan{bytes}, limit,
                           [chunkSize] { return chunkSize; }) == reference);
    }

    // A fixed seed, because a chunking that breaks the parser is worth nothing
    // if the next run picks different pieces.
    std::mt19937 rng{20250809};
    std::uniform_int_distribution<std::size_t> pieces{1, 97};
    CHECK(DrainChunked(ByteSpan{bytes}, limit, [&] { return pieces(rng); }) ==
          reference);
}

TEST_CASE("a header arriving one byte at a time decides nothing early") {
    std::vector<std::byte> bytes;
    EncodeFrame(bytes, 3, 7, 0x40);

    std::vector<std::byte> storage(HEADER_SIZE + MAX_HANDSHAKE_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};

    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        CAPTURE(i);
        REQUIRE(stream.Append(ByteSpan{bytes}.subspan(i, 1)).has_value());
        const auto partial = stream.Next();
        REQUIRE(partial.has_value());
        CHECK_FALSE(partial->has_value());
    }

    REQUIRE(stream.Append(ByteSpan{bytes}.last(1)).has_value());
    const auto next = stream.Next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((*next)->Header.Length == 3);
    CHECK((*next)->Header.Epoch == 7);
    CHECK((*next)->Body.size() == 3);
}

TEST_CASE("a frame is held until the next call and no longer") {
    std::vector<std::byte> bytes;
    EncodeFrame(bytes, 2, 1, 0x10);
    EncodeFrame(bytes, 3, 2, 0x20);

    std::vector<std::byte> storage(HEADER_SIZE + MAX_HANDSHAKE_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};
    REQUIRE(stream.Append(ByteSpan{bytes}).has_value());
    CHECK(stream.Buffered() == bytes.size());

    const auto first = stream.Next();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->Header.Epoch == 1);
    // The frame counts as gone the moment it is handed over, even though its
    // bytes are still there for the caller to read.
    CHECK(stream.Buffered() == bytes.size() - (*first)->Consumed);

    const auto second = stream.Next();
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->Header.Epoch == 2);
    CHECK(stream.Buffered() == 0);
    // What was left slid to the front when the first frame retired.
    CHECK((*second)->Body.data() == storage.data() + HEADER_SIZE);
}

TEST_CASE("a length above the stage limit is refused before its bytes exist") {
    std::vector<std::byte> header;
    EncodeHeader(header, MAX_HANDSHAKE_FRAME + 1, 0);

    std::vector<std::byte> storage(HEADER_SIZE + MAX_HANDSHAKE_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};
    REQUIRE(stream.Append(ByteSpan{header}).has_value());

    const auto next = stream.Next();
    REQUIRE_FALSE(next.has_value());
    CHECK(next.error() == EProtoError::LengthLimitExceeded);
    CHECK(DispositionOf(next.error()) == EDisposition::CloseConnection);
    // Five bytes were ever held: the refusal did not wait for the body.
    CHECK(stream.Buffered() == HEADER_SIZE);

    // The offending bytes stay at the front, so the verdict does not change on
    // a second look — there is no resyncing past a frame we never understood.
    const auto again = stream.Next();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == EProtoError::LengthLimitExceeded);
}

TEST_CASE("a frame declaring an empty body is refused") {
    std::vector<std::byte> header;
    EncodeHeader(header, 0, 0);

    std::vector<std::byte> storage(HEADER_SIZE + MAX_HANDSHAKE_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};
    REQUIRE(stream.Append(ByteSpan{header}).has_value());

    const auto next = stream.Next();
    REQUIRE_FALSE(next.has_value());
    CHECK(next.error() == EProtoError::MalformedField);
}

TEST_CASE("bytes past the capacity are an error, not a bigger buffer") {
    constexpr std::uint32_t limit = 16;

    std::vector<std::byte> storage(HEADER_SIZE + limit);
    FrameStream stream{MutableByteSpan{storage}, limit};
    CHECK(stream.Capacity() == HEADER_SIZE + limit);
    CHECK(stream.Room() == stream.Capacity());

    const std::vector<std::byte> filler(stream.Capacity());
    REQUIRE(stream.Append(ByteSpan{filler}).has_value());
    CHECK(stream.Buffered() == stream.Capacity());
    CHECK(stream.Room() == 0);

    const std::array<std::byte, 1> more{};
    const auto appended = stream.Append(ByteSpan{more});
    REQUIRE_FALSE(appended.has_value());
    CHECK(appended.error() == EProtoError::LengthLimitExceeded);
    CHECK(stream.Buffered() == stream.Capacity());
}

TEST_CASE("garbage does not make the buffer grow") {
    constexpr std::uint32_t limit = 64;

    std::vector<std::byte> storage(HEADER_SIZE + limit);
    FrameStream stream{MutableByteSpan{storage}, limit};

    const std::vector<std::byte> garbage(limit, std::byte{0xFF});
    REQUIRE(stream.Append(ByteSpan{garbage}).has_value());

    // 0xFFFFFFFF read as a length: turned down on the header alone, with
    // nothing put aside for the body it claims.
    const auto next = stream.Next();
    REQUIRE_FALSE(next.has_value());
    CHECK(next.error() == EProtoError::LengthLimitExceeded);
    CHECK(stream.Buffered() == garbage.size());

    const auto appended = stream.Append(ByteSpan{garbage});
    REQUIRE_FALSE(appended.has_value());
    CHECK(appended.error() == EProtoError::LengthLimitExceeded);
    CHECK(stream.Buffered() <= stream.Capacity());
}

TEST_CASE("the post-auth limit lets through what the handshake limit refuses") {
    constexpr std::uint32_t bodyLength = 200U * 1024U;

    std::vector<std::byte> bytes;
    EncodeFrame(bytes, bodyLength, 3, 0x5A);

    std::vector<std::byte> storage(HEADER_SIZE + MAX_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};
    CHECK(stream.MaxBodyLength() == MAX_HANDSHAKE_FRAME);

    REQUIRE(stream.Append(ByteSpan{bytes}.first(HEADER_SIZE)).has_value());
    const auto refused = stream.Next();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == EProtoError::LengthLimitExceeded);

    stream.RaiseLimit(MAX_FRAME);
    CHECK(stream.MaxBodyLength() == MAX_FRAME);

    REQUIRE(stream.Append(ByteSpan{bytes}.subspan(HEADER_SIZE)).has_value());
    const auto next = stream.Next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((*next)->Header.Length == bodyLength);
    CHECK((*next)->Header.Epoch == 3);
    CHECK((*next)->Body.size() == bodyLength);
}

TEST_CASE("the limit only ever widens") {
    std::vector<std::byte> storage(HEADER_SIZE + MAX_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_FRAME};

    stream.RaiseLimit(MAX_HANDSHAKE_FRAME);
    CHECK(stream.MaxBodyLength() == MAX_FRAME);
    CHECK(stream.Capacity() == HEADER_SIZE + MAX_FRAME);
}

TEST_CASE("the limit is capped by the storage on hand") {
    std::vector<std::byte> storage(100);
    FrameStream stream{MutableByteSpan{storage}, MAX_FRAME};

    CHECK(stream.Capacity() == storage.size());
    CHECK(stream.MaxBodyLength() == storage.size() - HEADER_SIZE);

    stream.RaiseLimit(MAX_FRAME);
    CHECK(stream.MaxBodyLength() == storage.size() - HEADER_SIZE);
}

TEST_CASE("storage too small for a header leaves room for no body at all") {
    std::vector<std::byte> storage(HEADER_SIZE - 2);
    FrameStream stream{MutableByteSpan{storage}, MAX_FRAME};

    CHECK(stream.Capacity() == storage.size());
    CHECK(stream.MaxBodyLength() == 0);

    const std::vector<std::byte> chunk(storage.size());
    REQUIRE(stream.Append(ByteSpan{chunk}).has_value());
    const auto next = stream.Next();
    REQUIRE(next.has_value());
    CHECK_FALSE(next->has_value());
}

TEST_CASE("an append of nothing changes nothing") {
    std::vector<std::byte> storage(HEADER_SIZE + MAX_HANDSHAKE_FRAME);
    FrameStream stream{MutableByteSpan{storage}, MAX_HANDSHAKE_FRAME};

    REQUIRE(stream.Append(ByteSpan{}).has_value());
    CHECK(stream.Buffered() == 0);
    const auto empty = stream.Next();
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->has_value());

    std::vector<std::byte> bytes;
    EncodeFrame(bytes, 2, 0, 1);
    REQUIRE(stream.Append(ByteSpan{bytes}).has_value());
    REQUIRE(stream.Append(ByteSpan{}).has_value());
    CHECK(stream.Buffered() == bytes.size());

    const auto next = stream.Next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((*next)->Body.size() == 2);
}
