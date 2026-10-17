#include "zet/session/receive_cursor.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {

using namespace zet;
using namespace zet::session;

/// Build a small byte vector with a deterministic pattern.
std::vector<std::byte> MakeBytes(std::size_t n, std::uint8_t seed = 0) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return v;
}

}  // namespace

// Basic delivery ----------------------------------------------------------

TEST_CASE("fresh cursor starts at zero") {
    ReceiveCursor cur;
    CHECK(cur.RecvOffset() == 0);
}

TEST_CASE("non-zero initial offset") {
    constexpr std::uint64_t init = 1'234'567;
    ReceiveCursor cur{init};
    CHECK(cur.RecvOffset() == init);
}

TEST_CASE("exact match at offset: delivers all bytes") {
    ReceiveCursor cur;
    const auto data = MakeBytes(5, 0x10);
    const auto result = cur.Receive(0, ByteSpan{data});

    REQUIRE(result.has_value());
    CHECK(result->size() == 5);
    CHECK(result->data() == data.data());  // sub-span, no copy
    CHECK(cur.RecvOffset() == 5);
}

TEST_CASE("recv_offset advances by delivered byte count") {
    ReceiveCursor cur;
    const auto a = MakeBytes(3, 0x01);
    const auto b = MakeBytes(7, 0x10);

    REQUIRE(cur.Receive(0, ByteSpan{a}).has_value());
    CHECK(cur.RecvOffset() == 3);

    REQUIRE(cur.Receive(3, ByteSpan{b}).has_value());
    CHECK(cur.RecvOffset() == 10);
}

// Duplicate frames --------------------------------------------------------

TEST_CASE("complete duplicate: delivers empty span") {
    ReceiveCursor cur;
    const auto data = MakeBytes(8, 0xAA);
    REQUIRE(cur.Receive(0, ByteSpan{data}).has_value());
    CHECK(cur.RecvOffset() == 8);

    // Repeat the same frame.
    const auto dup = cur.Receive(0, ByteSpan{data});
    REQUIRE(dup.has_value());
    CHECK(dup->empty());
    CHECK(cur.RecvOffset() == 8);  // did not move
}

TEST_CASE("partial duplicate: only the new tail is delivered") {
    ReceiveCursor cur;
    const auto first = MakeBytes(6, 0x00);
    REQUIRE(cur.Receive(0, ByteSpan{first}).has_value());
    CHECK(cur.RecvOffset() == 6);

    // Frame that overlaps [3, 10): bytes 0..2 are old, 3..9 are new.
    const auto overlap = MakeBytes(7, 0x03);
    const auto result = cur.Receive(3, ByteSpan{overlap});
    REQUIRE(result.has_value());
    CHECK(result->size() == 4);  // new portion: bytes at offset 6..9
    // Sub-span points into `overlap`: the first 3 bytes were already received.
    CHECK(result->data() == overlap.data() + 3);
    CHECK(cur.RecvOffset() == 10);
}

TEST_CASE("frame ending exactly at recv_offset is a complete duplicate") {
    ReceiveCursor cur;
    const auto data = MakeBytes(4, 0x00);
    REQUIRE(cur.Receive(0, ByteSpan{data}).has_value());

    // Frame [0, 4): entirely below recv_offset 4.
    const auto dup = cur.Receive(0, ByteSpan{data});
    REQUIRE(dup.has_value());
    CHECK(dup->empty());
    CHECK(cur.RecvOffset() == 4);
}

// Gap errors --------------------------------------------------------------

TEST_CASE("data starting above recv_offset returns StreamGap") {
    ReceiveCursor cur;
    const auto data = MakeBytes(4, 0x00);
    // Gap: nothing received yet but offset is 5.
    const auto result = cur.Receive(5, ByteSpan{data});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::StreamGap);
    CHECK(cur.RecvOffset() == 0);  // state unchanged
}

TEST_CASE("StreamGap leaves recv_offset unchanged") {
    ReceiveCursor cur;
    const auto first = MakeBytes(4);
    REQUIRE(cur.Receive(0, ByteSpan{first}).has_value());
    CHECK(cur.RecvOffset() == 4);

    const auto gap = MakeBytes(3);
    const auto result = cur.Receive(8, ByteSpan{gap});  // skip offsets 4..7
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::StreamGap);
    CHECK(cur.RecvOffset() == 4);
}

// uint64 overflow ---------------------------------------------------------

TEST_CASE("offset + length overflow returns StreamGap without UB") {
    // Start near the top of the uint64 address space.
    constexpr std::uint64_t high =
        std::numeric_limits<std::uint64_t>::max() - 4;
    ReceiveCursor cur{high};

    // First, advance past the 2^64-5 mark with a short in-range frame.
    const auto valid = MakeBytes(3, 0x01);
    REQUIRE(cur.Receive(high, ByteSpan{valid}).has_value());
    CHECK(cur.RecvOffset() == high + 3);

    // Now try a frame whose end would wrap: offset = high+3, length = 10
    // → offset + 10 > UINT64_MAX.  Must return StreamGap, not UB.
    const auto big = MakeBytes(10, 0x20);
    const auto result = cur.Receive(high + 3, ByteSpan{big});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::StreamGap);
    CHECK(cur.RecvOffset() == high + 3);  // unchanged
}

TEST_CASE("maximal offset itself does not overflow") {
    constexpr std::uint64_t maxVal = std::numeric_limits<std::uint64_t>::max();
    ReceiveCursor cur{maxVal};

    // A zero-length span at MAX: offset + 0 does not overflow.
    const ByteSpan empty{};
    const auto result = cur.Receive(maxVal, empty);
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

// recv_offset monotonicity ------------------------------------------------

TEST_CASE("recv_offset never decreases across any sequence") {
    ReceiveCursor cur;
    std::uint64_t prev = 0;

    // Normal delivery.
    const auto a = MakeBytes(10, 0x01);
    REQUIRE(cur.Receive(0, ByteSpan{a}).has_value());
    CHECK(cur.RecvOffset() >= prev);
    prev = cur.RecvOffset();

    // Duplicate → no change.
    REQUIRE(cur.Receive(0, ByteSpan{a}).has_value());
    CHECK(cur.RecvOffset() == prev);

    // Partial duplicate.
    const auto b = MakeBytes(5, 0x05);
    REQUIRE(cur.Receive(5, ByteSpan{b}).has_value());
    CHECK(cur.RecvOffset() >= prev);
    prev = cur.RecvOffset();

    // Gap → error, but offset must not decrease.
    const auto c = MakeBytes(3, 0x00);
    const auto res = cur.Receive(cur.RecvOffset() + 100, ByteSpan{c});
    REQUIRE_FALSE(res.has_value());
    CHECK(cur.RecvOffset() == prev);
}

// Reconnect with initial offset -------------------------------------------

TEST_CASE("reconnect scenario: initial offset mirrors acked position") {
    // After reconnect the cursor starts at the last recv_offset the peer
    // confirmed in the handshake (s_recv_offset / c_recv_offset).  §9.
    constexpr std::uint64_t resume = 1'000'000;
    ReceiveCursor cur{resume};

    // The sender resends from resume.
    const auto data = MakeBytes(6, 0xCC);
    const auto result = cur.Receive(resume, ByteSpan{data});
    REQUIRE(result.has_value());
    CHECK(result->size() == 6);
    CHECK(cur.RecvOffset() == resume + 6);

    // Earlier frame already covered before the reconnect: duplicate, no data.
    const auto earlier = MakeBytes(4, 0xAA);
    const auto dup = cur.Receive(resume - 4, ByteSpan{earlier});
    REQUIRE(dup.has_value());
    CHECK(dup->empty());
}

// Property test -----------------------------------------------------------

TEST_CASE("property: ReceiveCursor handles random duplicates and gaps") {
    constexpr std::uint32_t seed = 0xBAADF00DU;
    INFO("seed=" << seed);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<std::size_t> lenDist(1, 20);
    std::uniform_int_distribution<int> opDist(0, 2);

    // Reference: a flat vector of every byte delivered to the cursor.
    std::vector<std::byte> reference;
    ReceiveCursor cur;

    // Ground-truth stream: build a long stream of bytes.
    constexpr std::size_t streamLen = 4096;
    std::vector<std::byte> stream(streamLen);
    for (auto& b : stream) {
        b = static_cast<std::byte>(byteDist(rng));
    }

    // Feed the cursor through random windows.
    std::uint64_t highWater = 0;  // furthest end position sent so far

    for (int iter = 0; iter < 5'000 && highWater <= streamLen; ++iter) {
        const int op = opDist(rng);

        if (op == 0 || highWater == 0) {
            // Fresh window: extend from highWater.
            const std::size_t len = std::min(
                lenDist(rng), streamLen - static_cast<std::size_t>(highWater));
            if (len == 0) {
                continue;
            }
            const ByteSpan chunk{stream.data() + highWater, len};
            const auto result = cur.Receive(highWater, chunk);
            REQUIRE(result.has_value());
            for (auto b : *result) {
                reference.push_back(b);
            }
            highWater += static_cast<std::uint64_t>(len);

        } else if (op == 1 && highWater > 0) {
            // Retransmit: pick a random start in [0, highWater) and a length
            // that does not extend past highWater (no gap).
            std::uniform_int_distribution<std::uint64_t> startDist(
                0, highWater - 1);
            const std::uint64_t start = startDist(rng);
            const std::size_t maxLen =
                static_cast<std::size_t>(highWater - start);
            const std::size_t len = std::min(lenDist(rng), maxLen);

            const ByteSpan chunk{stream.data() + start, len};
            const auto result = cur.Receive(start, chunk);
            REQUIRE(result.has_value());  // no gap, must succeed
            // Bytes delivered must match the stream.
            const std::uint64_t deliveredStart =
                std::max(start, cur.RecvOffset() -
                                    static_cast<std::uint64_t>(result->size()));
            for (std::size_t i = 0; i < result->size(); ++i) {
                CHECK((*result)[i] == stream[deliveredStart + i]);
            }

        } else {
            // Duplicate of already-received data.
            if (cur.RecvOffset() == 0) {
                continue;
            }
            std::uniform_int_distribution<std::uint64_t> dupDist(
                0, cur.RecvOffset() - 1);
            const std::uint64_t start = dupDist(rng);
            const std::size_t maxLen =
                static_cast<std::size_t>(cur.RecvOffset() - start);
            const std::size_t len = std::min(lenDist(rng), maxLen);

            const ByteSpan chunk{stream.data() + start, len};
            const auto result = cur.Receive(start, chunk);
            REQUIRE(result.has_value());
            CHECK(result->empty());  // complete duplicate
        }
    }

    // Everything reference reports must match the stream from offset 0.
    REQUIRE(reference.size() == static_cast<std::size_t>(cur.RecvOffset()));
    for (std::size_t i = 0; i < reference.size(); ++i) {
        CHECK(reference[i] == stream[i]);
    }
}
