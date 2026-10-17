#include "zet/session/send_buffer.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {

using namespace zet;
using namespace zet::session;

// Helpers -----------------------------------------------------------------

/// Fill a span with a deterministic byte pattern starting at `seed`.
void Fill(std::vector<std::byte>& v, std::size_t n, std::uint8_t seed = 0) {
    v.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
}

/// Drain up to `maxBytes` from `buf` starting at `fromOffset` and return them
/// by value.
std::vector<std::byte> DrainCopyOut(const SendBuffer& buf,
                                    std::uint64_t fromOffset,
                                    std::size_t maxBytes) {
    std::vector<std::byte> dest(maxBytes);
    const auto written = buf.CopyOut(fromOffset, MutableByteSpan{dest});
    dest.resize(written.size());
    return dest;
}

}  // namespace

// Basic state ---------------------------------------------------------------

TEST_CASE("fresh buffer is empty and reports full room") {
    std::array<std::byte, 64> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    CHECK(buf.Base() == 0);
    CHECK(buf.End() == 0);
    CHECK(buf.SendNext() == 0);
    CHECK(buf.Size() == 0);
    CHECK(buf.Room() == 64);
    CHECK(buf.Capacity() == 64);
    CHECK(buf.Unsent() == 0);
}

TEST_CASE("non-zero initial base shifts all offsets") {
    std::array<std::byte, 32> storage{};
    constexpr std::uint64_t base = 1'000'000;
    SendBuffer buf{MutableByteSpan{storage}, base};

    CHECK(buf.Base() == base);
    CHECK(buf.End() == base);
    CHECK(buf.SendNext() == base);
    CHECK(buf.Size() == 0);
    CHECK(buf.Room() == 32);
}

// Append ------------------------------------------------------------------

TEST_CASE("append takes all bytes when room is enough") {
    std::array<std::byte, 64> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 10, 0xAA);
    const std::size_t taken = buf.Append(ByteSpan{data});

    CHECK(taken == 10);
    CHECK(buf.Size() == 10);
    CHECK(buf.End() == 10);
    CHECK(buf.Room() == 54);
}

TEST_CASE("append to full ring returns 0") {
    std::array<std::byte, 8> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 8);
    CHECK(buf.Append(ByteSpan{data}) == 8);
    CHECK(buf.Room() == 0);
    CHECK(buf.Append(ByteSpan{data}) == 0);
}

TEST_CASE("append truncates to room when data exceeds room") {
    std::array<std::byte, 8> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 6);
    CHECK(buf.Append(ByteSpan{data}) == 6);

    Fill(data, 10, 0x20);
    const std::size_t taken = buf.Append(ByteSpan{data});
    CHECK(taken == 2);  // only 2 slots remain
    CHECK(buf.Room() == 0);
}

TEST_CASE("empty append is a no-op") {
    std::array<std::byte, 8> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    const ByteSpan empty{};
    CHECK(buf.Append(empty) == 0);
    CHECK(buf.Size() == 0);
}

// Acknowledge -------------------------------------------------------------

TEST_CASE("ack at base is a no-op") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 8);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto result = buf.Acknowledge(0);
    REQUIRE(result.has_value());
    CHECK(buf.Base() == 0);
    CHECK(buf.Size() == 8);
}

TEST_CASE("ack below base is ignored") {
    std::array<std::byte, 16> storage{};
    constexpr std::uint64_t base = 100;
    SendBuffer buf{MutableByteSpan{storage}, base};
    std::vector<std::byte> data;
    Fill(data, 8);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    // Ack older than base: harmless.
    const auto result = buf.Acknowledge(base - 1);
    REQUIRE(result.has_value());
    CHECK(buf.Base() == base);
}

TEST_CASE("ack at end drains the ring") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 10);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto result = buf.Acknowledge(10);
    REQUIRE(result.has_value());
    CHECK(buf.Base() == 10);
    CHECK(buf.End() == 10);
    CHECK(buf.Size() == 0);
    CHECK(buf.Room() == 16);
}

TEST_CASE("ack in the middle trims and frees room") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 10);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto result = buf.Acknowledge(6);
    REQUIRE(result.has_value());
    CHECK(buf.Base() == 6);
    CHECK(buf.Size() == 4);
    CHECK(buf.Room() == 12);
}

TEST_CASE("ack beyond end returns AckBeyondSent") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 5);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto result = buf.Acknowledge(6);  // 6 > End (5)
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::AckBeyondSent);
}

TEST_CASE("ack beyond end on empty buffer returns AckBeyondSent") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    const auto result = buf.Acknowledge(1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::AckBeyondSent);
}

// CopyOut -----------------------------------------------------------------

TEST_CASE("CopyOut at base returns correct bytes") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 8, 0x10);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto got = DrainCopyOut(buf, 0, 8);
    CHECK(got == data);
}

TEST_CASE("CopyOut from the middle returns the tail") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 8, 0x00);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    const auto got = DrainCopyOut(buf, 3, 5);
    REQUIRE(got.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(got[i] == static_cast<std::byte>(3 + i));
    }
}

TEST_CASE("CopyOut clips to what is available") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};
    std::vector<std::byte> data;
    Fill(data, 5);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    // Ask for 10, only 5 available starting at base.
    const auto got = DrainCopyOut(buf, 0, 10);
    CHECK(got.size() == 5);
}

TEST_CASE("CopyOut on empty returns empty") {
    std::array<std::byte, 16> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::array<std::byte, 8> dest{};
    const auto written = buf.CopyOut(0, MutableByteSpan{dest});
    CHECK(written.empty());
}

// Ring wrap-around --------------------------------------------------------

TEST_CASE("ring wrap-around: append, ack, fill across the seam") {
    // Storage of 8 bytes: positions 0..7.
    std::array<std::byte, 8> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    // Fill with 0..5 (6 bytes, positions 0..5 in the ring).
    std::vector<std::byte> first;
    Fill(first, 6, 0x00);
    CHECK(buf.Append(ByteSpan{first}) == 6);

    // Acknowledge 4 bytes: ring now holds [4, 6) at physical positions 4..5.
    REQUIRE(buf.Acknowledge(4).has_value());
    CHECK(buf.Base() == 4);
    CHECK(buf.Size() == 2);
    CHECK(buf.Room() == 6);

    // Append 6 more bytes: End goes from 6 to 12.  WritePos = 6 % 8 = 6, then
    // wraps: bytes at physical 6, 7, 0, 1, 2, 3.
    std::vector<std::byte> second;
    Fill(second, 6, 0x10);
    CHECK(buf.Append(ByteSpan{second}) == 6);
    CHECK(buf.End() == 12);
    CHECK(buf.Size() == 8);

    // CopyOut across the wrap: start at offset 4 (physical 4), read 8 bytes.
    const auto got = DrainCopyOut(buf, 4, 8);
    REQUIRE(got.size() == 8);
    // Bytes [4,6) came from `first`: values 0x04, 0x05.
    CHECK(got[0] == static_cast<std::byte>(0x04));
    CHECK(got[1] == static_cast<std::byte>(0x05));
    // Bytes [6,12) came from `second`: values 0x10..0x15.
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(got[2 + i] == static_cast<std::byte>(0x10 + i));
    }
}

TEST_CASE("ring wrap-around at every starting position") {
    // Rotate the write head to every position 0..cap-1 and verify CopyOut.
    constexpr std::size_t cap = 7;  // odd to stress non-power-of-two wrapping
    std::array<std::byte, cap> storage{};

    for (std::size_t startPos = 0; startPos < cap; ++startPos) {
        SendBuffer buf{MutableByteSpan{storage}};

        // Wind the write head to `startPos` by filling and acking startPos
        // bytes.
        if (startPos > 0) {
            std::vector<std::byte> pad(startPos, std::byte{0xFF});
            CHECK(buf.Append(ByteSpan{pad}) == startPos);
            REQUIRE(buf.Acknowledge(startPos).has_value());
        }
        CHECK(buf.Base() == startPos);
        CHECK(buf.Size() == 0);

        // Append cap bytes so the ring is full, wrapping at startPos.
        std::vector<std::byte> data(cap);
        for (std::size_t i = 0; i < cap; ++i) {
            data[i] = static_cast<std::byte>(i + 1);  // 1..cap
        }
        CHECK(buf.Append(ByteSpan{data}) == cap);

        // CopyOut the whole ring in one shot.
        const auto got = DrainCopyOut(buf, buf.Base(), cap);
        REQUIRE(got.size() == cap);
        for (std::size_t i = 0; i < cap; ++i) {
            CHECK(got[i] == static_cast<std::byte>(i + 1));
        }
    }
}

// SendNext ----------------------------------------------------------------

TEST_CASE("SendNext starts at End and tracks appends") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 10);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);
    // SendNext was at 0 before the append; it does not move automatically.
    CHECK(buf.SendNext() == 0);
    CHECK(buf.Unsent() == 10);
}

TEST_CASE("AdvanceSendNext steps through the ring") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 10);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    buf.AdvanceSendNext(5);
    CHECK(buf.SendNext() == 5);
    CHECK(buf.Unsent() == 5);

    buf.AdvanceSendNext(5);
    CHECK(buf.SendNext() == 10);
    CHECK(buf.Unsent() == 0);
}

TEST_CASE("AdvanceSendNext clamps to End") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 5);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);

    buf.AdvanceSendNext(100);  // overshoot
    CHECK(buf.SendNext() == 5);
    CHECK(buf.SendNext() == buf.End());
}

TEST_CASE("ResetSendNext after reconnect resumes from peer offset") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 20);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);
    buf.AdvanceSendNext(20);
    CHECK(buf.SendNext() == 20);

    // Ack 10, reconnect: peer has received up to offset 5.
    REQUIRE(buf.Acknowledge(10).has_value());
    buf.ResetSendNext(5);
    // Peer_recv_offset 5 < Base 10 → clamped to Base.
    CHECK(buf.SendNext() == 10);

    buf.ResetSendNext(15);
    CHECK(buf.SendNext() == 15);

    buf.ResetSendNext(100);  // above End → clamped to End
    CHECK(buf.SendNext() == 20);
}

TEST_CASE("Acknowledge clamps SendNext to new Base") {
    std::array<std::byte, 32> storage{};
    SendBuffer buf{MutableByteSpan{storage}};

    std::vector<std::byte> data;
    Fill(data, 20);
    REQUIRE(buf.Append(ByteSpan{data}) > 0);
    // Advance send cursor to 5.
    buf.AdvanceSendNext(5);

    // Ack past SendNext: SendNext must move up to base.
    REQUIRE(buf.Acknowledge(15).has_value());
    CHECK(buf.SendNext() == 15);
    CHECK(buf.SendNext() >= buf.Base());
}

// Disconnect-buffer-overflow path -----------------------------------------

TEST_CASE("caller can detect overflow past DISCONNECT_BUFFER_MAX") {
    // Simulate the session-layer overflow check that §9 requires without
    // putting policy in the buffer.  The buffer returns partial takes and the
    // caller decides when enough is enough.
    constexpr std::size_t limit = 16;  // stand-in for DISCONNECT_BUFFER_MAX
    std::array<std::byte, limit> storage{};
    SendBuffer buf{MutableByteSpan{storage}, 0};

    // Drive the buffer to the limit without acks (disconnected scenario).
    std::vector<std::byte> data(limit, std::byte{0xBB});
    CHECK(buf.Append(ByteSpan{data}) == limit);
    CHECK(buf.Room() == 0);

    // One more byte arrives: take == 0, so the caller can raise
    // DisconnectBufferOverflow.  Silently dropping is impossible: Room() == 0
    // means there is no slot to quietly overwrite old bytes.
    std::array<std::byte, 1> one{std::byte{0x01}};
    CHECK(buf.Append(ByteSpan{one}) == 0);

    // The buffer itself is unchanged — only the caller decides disposition.
    CHECK(buf.Size() == limit);
}

// Property test -----------------------------------------------------------

TEST_CASE("property: SendBuffer byte contents match std::vector reference") {
    // Fixed seed: print it in the failure message so any regression is
    // reproducible.
    constexpr std::uint32_t seed = 0xDEADBEEFU;
    INFO("seed=" << seed);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> lenDist(1, 32);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> opDist(0, 2);

    constexpr std::size_t storageSize = 128;
    constexpr std::uint64_t initialBase = 9'007'199'254'740'992ULL;  // 2^53
    std::array<std::byte, storageSize> storage{};
    SendBuffer buf{MutableByteSpan{storage}, initialBase};

    // Reference model: a flat vector of all bytes ever accepted, indexed from
    // initialBase.
    std::vector<std::byte> reference;
    std::uint64_t refBase = initialBase;

    for (int iter = 0; iter < 10'000; ++iter) {
        const int op = opDist(rng);

        if (op == 0) {
            // Append random bytes.
            std::vector<std::byte> data(lenDist(rng));
            for (auto& b : data) {
                b = static_cast<std::byte>(byteDist(rng));
            }
            const std::size_t taken = buf.Append(ByteSpan{data});
            reference.insert(reference.end(), data.begin(),
                             data.begin() + static_cast<std::ptrdiff_t>(taken));

            // Invariant: buf.End() == initialBase + reference.size()
            CHECK(buf.End() ==
                  refBase + static_cast<std::uint64_t>(reference.size()));

        } else if (op == 1) {
            // Acknowledge a random prefix.
            if (buf.Base() < buf.End()) {
                std::uniform_int_distribution<std::uint64_t> ackDist(buf.Base(),
                                                                     buf.End());
                const std::uint64_t ackOffset = ackDist(rng);
                REQUIRE(buf.Acknowledge(ackOffset).has_value());

                const std::size_t trimmed =
                    static_cast<std::size_t>(ackOffset - refBase);
                reference.erase(
                    reference.begin(),
                    reference.begin() + static_cast<std::ptrdiff_t>(trimmed));
                refBase = ackOffset;
            }

        } else {
            // CopyOut and verify byte-for-byte.
            if (buf.Base() < buf.End()) {
                std::uniform_int_distribution<std::uint64_t> offsetDist(
                    buf.Base(), buf.End() - 1);
                const std::uint64_t fromOffset = offsetDist(rng);

                std::array<std::byte, 64> dest{};
                const auto written =
                    buf.CopyOut(fromOffset, MutableByteSpan{dest});

                const std::size_t refIndex =
                    static_cast<std::size_t>(fromOffset - refBase);
                const std::size_t available = reference.size() - refIndex;
                const std::size_t expected = std::min(dest.size(), available);

                CHECK(written.size() == expected);
                for (std::size_t i = 0; i < expected; ++i) {
                    CHECK(written[i] == reference[refIndex + i]);
                }
            }
        }
    }
}

TEST_CASE("property: sender-receiver pipeline with random retransmits") {
    constexpr std::uint32_t seed = 0xCAFEBABEU;
    INFO("seed=" << seed);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> lenDist(1, 16);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> coinDist(0, 1);

    constexpr std::size_t storageSize = 128;
    constexpr std::uint64_t initialOffset = 0;
    std::array<std::byte, storageSize> sendStorage{};
    SendBuffer sender{MutableByteSpan{sendStorage}, initialOffset};

    // Accumulate all bytes ever appended for comparison at the end.
    std::vector<std::byte> sent;
    std::vector<std::byte> received;

    // Simulated peer recv_offset.
    std::uint64_t peerRecv = initialOffset;

    for (int iter = 0; iter < 5'000; ++iter) {
        if (coinDist(rng)) {
            // Append random data.
            std::vector<std::byte> data(lenDist(rng));
            for (auto& b : data) {
                b = static_cast<std::byte>(byteDist(rng));
            }
            const std::size_t taken = sender.Append(ByteSpan{data});
            sent.insert(sent.end(), data.begin(),
                        data.begin() + static_cast<std::ptrdiff_t>(taken));
        }

        if (sender.Base() < sender.End()) {
            // Pick a random start offset in [Base, End) to simulate
            // duplicate/partial retransmit.
            std::uniform_int_distribution<std::uint64_t> offDist(
                sender.Base(), sender.End() - 1);
            const std::uint64_t fromOffset = offDist(rng);

            const std::size_t maxLen =
                static_cast<std::size_t>(sender.End() - fromOffset);
            const std::size_t chunkLen = std::min(lenDist(rng), maxLen);

            std::array<std::byte, 32> chunkBuf{};
            const auto chunk = sender.CopyOut(
                fromOffset, MutableByteSpan{chunkBuf}.first(chunkLen));
            REQUIRE(chunk.size() == chunkLen);

            // Deliver to simulated receiver (inline ReceiveCursor logic so
            // this test is self-contained).
            if (fromOffset <= peerRecv) {
                const std::uint64_t skip = peerRecv - fromOffset;
                if (skip < static_cast<std::uint64_t>(chunk.size())) {
                    const ByteSpan newBytes =
                        ByteSpan{chunk}.subspan(static_cast<std::size_t>(skip));
                    for (auto b : newBytes) {
                        received.push_back(b);
                    }
                    peerRecv += static_cast<std::uint64_t>(newBytes.size());
                }
            }

            // Ack what the peer has.
            const auto ackResult = sender.Acknowledge(peerRecv);
            REQUIRE(ackResult.has_value());
        }
    }

    // Drain the remaining buffered bytes in one pass.
    if (sender.Base() < sender.End()) {
        std::array<std::byte, storageSize> drain{};
        const auto remaining =
            sender.CopyOut(sender.Base(), MutableByteSpan{drain});
        const ByteSpan remSpan{remaining};
        const std::uint64_t skip =
            (peerRecv > sender.Base()) ? peerRecv - sender.Base() : 0;
        if (skip < static_cast<std::uint64_t>(remSpan.size())) {
            for (auto b : remSpan.subspan(static_cast<std::size_t>(skip))) {
                received.push_back(b);
            }
        }
    }

    // received[0..n) must equal sent[0..n)
    REQUIRE(received.size() <= sent.size());
    for (std::size_t i = 0; i < received.size(); ++i) {
        CHECK(received[i] == sent[i]);
    }
}
