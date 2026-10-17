/// The pty contract of docs/design.md §10 and §16: what the agent may rely
/// on from a pty, checked on a real one and on FakePty by the same code. If
/// the two part ways, this fails, and the fake is wrong.
///
/// The assertions are about behaviour, never about buffer sizes, which are
/// the kernel's to choose.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pty_under_test.hpp"

namespace {

using namespace zet;
using namespace zet::io;
using namespace zet::io::test;

/// How long a writer that stopped getting room is given before it counts as
/// stalled. Only the real pty waits: the child may still be draining.
constexpr std::chrono::milliseconds STALL_WAIT{200};

/// More than a pty and zet_pty_echo can hold between them. A writer that got
/// this far without stalling has a pty under it that buffers without bound.
constexpr std::size_t STALL_BOUND = 8 * 1024 * 1024;

/// What the stall test pushes per write: bigger than any one buffer.
constexpr std::size_t BURST_CHUNK = 64 * 1024;

/// Total pushed through by the test that reads as it writes.
constexpr std::size_t ROUND_TRIP_BYTES = 1024 * 1024;

/// Small enough to sit in the kernel's buffer whole, so the emitter exits
/// before anything reads it.
constexpr std::size_t LAST_WORDS_BYTES = 1000;

std::vector<std::byte> Pattern(std::size_t from, std::size_t length) {
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::byte>(PatternByte(from + i));
    }
    return bytes;
}

/// Writes bursts without reading until the pty stops taking them for
/// STALL_WAIT. Returns how much it took; fails if that passes STALL_BOUND.
std::size_t WriteUntilStalled(PtyUnderTest& pty) {
    std::size_t written = 0;
    for (;;) {
        const auto burst = Pattern(written, BURST_CHUNK);
        const auto n = pty.Write(ByteSpan{burst});
        if (n) {
            written += *n;
            REQUIRE_MESSAGE(written <= STALL_BOUND,
                            "the pty took " << written << " bytes unread");
            continue;
        }
        REQUIRE(n.error().WouldBlock());
        if (!pty.WaitWritable(STALL_WAIT)) {
            return written;
        }
    }
}

}  // namespace

TEST_CASE_TEMPLATE("pty contract: an empty master would block", TFactory,
                   RealFactory, FakeFactory) {
    auto pty = TFactory::Echo();
    REQUIRE(ReadPast(*pty, READY_MARKER).empty());
    std::array<std::byte, 16> buffer{};
    const auto n = pty->Read(MutableByteSpan{buffer});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().WouldBlock());
}

TEST_CASE_TEMPLATE(
    "pty contract: a burst nobody reads stalls the writer, never blocks it",
    TFactory, RealFactory, FakeFactory) {
    // 243078a in ET: the write to the pty blocked forever, because the child
    // echoed into a buffer nobody read. Non-blocking, it has to end in
    // EAGAIN instead, after a bounded amount.
    auto pty = TFactory::Echo();
    REQUIRE(ReadPast(*pty, READY_MARKER).empty());
    const std::size_t taken = WriteUntilStalled(*pty);
    CHECK(taken > 0);
}

TEST_CASE_TEMPLATE(
    "pty contract: reading the echo lets a stalled writer go on, losing "
    "nothing",
    TFactory, RealFactory, FakeFactory) {
    auto pty = TFactory::Echo();
    std::vector<std::byte> echoed = ReadPast(*pty, READY_MARKER);
    std::size_t written = WriteUntilStalled(*pty);

    while (written < ROUND_TRIP_BYTES || echoed.size() < written) {
        bool moved = false;
        const std::size_t before = echoed.size();
        REQUIRE(Drain(*pty, echoed));
        moved = echoed.size() != before;
        if (written < ROUND_TRIP_BYTES) {
            const auto chunk = Pattern(
                written, std::min(BURST_CHUNK, ROUND_TRIP_BYTES - written));
            const auto n = pty->Write(ByteSpan{chunk});
            if (n) {
                written += *n;
                moved = true;
            } else {
                REQUIRE(n.error().WouldBlock());
            }
        }
        if (!moved) {
            REQUIRE_MESSAGE(pty->WaitReadable(PTY_WAIT_TIMEOUT),
                            "stuck with " << written << " written, "
                                          << echoed.size() << " echoed");
        }
    }
    REQUIRE(echoed.size() == ROUND_TRIP_BYTES);
    CHECK(echoed == Pattern(0, ROUND_TRIP_BYTES));
}

TEST_CASE_TEMPLATE(
    "pty contract: a child's output outlives it, then the stream ends",
    TFactory, RealFactory, FakeFactory) {
    auto pty = TFactory::Emitter(LAST_WORDS_BYTES);
    REQUIRE(pty->WaitHangup(PTY_WAIT_TIMEOUT));
    std::vector<std::byte> output;
    CHECK_FALSE(Drain(*pty, output));
    const std::string_view text = AsText(output);
    REQUIRE(text.starts_with(READY_MARKER));
    const std::vector<std::byte> rest(
        output.begin() + static_cast<std::ptrdiff_t>(READY_MARKER.size()),
        output.end());
    CHECK(rest == Pattern(0, LAST_WORDS_BYTES));
}

TEST_CASE_TEMPLATE("pty contract: a resize reaches the child", TFactory,
                   RealFactory, FakeFactory) {
    auto pty = TFactory::Echo();
    REQUIRE(ReadPast(*pty, READY_MARKER).empty());
    REQUIRE(pty->Resize(WindowSize{.Rows = 30, .Cols = 100}));
    CHECK(ReadPast(*pty, "<winsize 30 100>").empty());
}
