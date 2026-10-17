/// The M3 criterion of docs/design.md §17: 64 MiB both ways through a real
/// pty without a deadlock and with memory bounded.
///
/// One loop on the Poller, as the agent will run it: write to the master
/// while it has room, read the echo while it has some, never block. The
/// stream goes out of a generator and is checked against a second copy of
/// it, so nothing here holds more than one chunk each way.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>

#include "pty_under_test.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/poller.hpp"

namespace {

using namespace zet;
using namespace zet::io;
using namespace zet::io::test;

constexpr std::uint64_t BURST_BYTES = 64ULL * 1024 * 1024;

/// Read and write at most this much per call, and hold no more than that each
/// way: the process's share of what is in flight.
constexpr std::size_t LOOP_CHUNK = 64 * 1024;

/// What may be written but not yet echoed back: the kernel's two pty buffers
/// and zet_pty_echo's one, with room to spare. A pty that buffered without
/// bound, or a loop that let the backlog grow, would pass it.
constexpr std::uint64_t IN_FLIGHT_BOUND = 1024 * 1024;

constexpr std::uint64_t MASTER_TOKEN = 1;
constexpr std::size_t EVENTS_PER_WAIT = 4;
constexpr std::uint64_t STREAM_SEED = 0x7a65745f62757273;  // "zet_burs"

/// The burst as a stream: the same seed gives the same bytes, so the reader
/// can check what the writer sent without anyone keeping 64 MiB.
class Stream {
public:
    explicit Stream(std::uint64_t seed) : Random_(seed) {}

    void Fill(MutableByteSpan into) {
        for (auto& byte : into) {
            byte = static_cast<std::byte>(Random_() & 0xFFU);
        }
    }

private:
    std::mt19937_64 Random_;
};

}  // namespace

TEST_CASE("64 MiB echo through a real pty on one poller loop, no deadlock") {
    RealPty pty{std::array<const char*, 3>{ECHO_CHILD_PATH, "echo", nullptr}};
    REQUIRE(ReadPast(pty, READY_MARKER).empty());
    const int master = pty.Master();

    auto poller = Poller::Create();
    REQUIRE(poller.has_value());
    REQUIRE(poller->Watch(master, EInterest::ReadWrite, MASTER_TOKEN));

    Stream outgoing{STREAM_SEED};
    Stream expected{STREAM_SEED};
    std::array<std::byte, LOOP_CHUNK> sendChunk{};
    std::size_t sendFrom = 0;
    std::size_t sendTo = 0;
    std::array<std::byte, LOOP_CHUNK> received{};
    std::array<std::byte, LOOP_CHUNK> check{};

    std::uint64_t written = 0;
    std::uint64_t echoed = 0;
    std::uint64_t mostInFlight = 0;
    bool writing = true;
    std::array<PollEvent, EVENTS_PER_WAIT> events{};

    while (echoed < BURST_BYTES) {
        const Timestamp now = MonotonicNow();
        const auto ready =
            poller->Wait(events, Deadline{now + PTY_WAIT_TIMEOUT}, now);
        REQUIRE(ready.has_value());
        REQUIRE_MESSAGE(*ready > 0, "no progress for "
                                        << PTY_WAIT_TIMEOUT.count() << " ms at "
                                        << written << " written, " << echoed
                                        << " echoed");

        for (std::size_t e = 0; e < *ready; ++e) {
            const PollEvent& event = events[e];
            REQUIRE(event.Token == MASTER_TOKEN);
            REQUIRE_FALSE(event.Error);

            if (event.Writable && writing) {
                if (sendFrom == sendTo) {
                    const auto length =
                        static_cast<std::size_t>(std::min<std::uint64_t>(
                            LOOP_CHUNK, BURST_BYTES - written));
                    outgoing.Fill(MutableByteSpan{sendChunk}.first(length));
                    sendFrom = 0;
                    sendTo = length;
                }
                const auto n = WriteSome(
                    master,
                    ByteSpan{sendChunk}.subspan(sendFrom, sendTo - sendFrom));
                if (n) {
                    sendFrom += *n;
                    written += *n;
                } else {
                    REQUIRE(n.error().WouldBlock());
                }
                if (written == BURST_BYTES) {
                    writing = false;
                    REQUIRE(
                        poller->Modify(master, EInterest::Read, MASTER_TOKEN));
                }
            }

            if (event.Readable || event.Hangup) {
                const auto n = ReadPty(master, MutableByteSpan{received});
                if (!n) {
                    REQUIRE(n.error().WouldBlock());
                    continue;
                }
                REQUIRE_MESSAGE(*n > 0, "the child went away at " << echoed);
                REQUIRE(echoed + *n <= written);
                const auto got = MutableByteSpan{received}.first(*n);
                const auto want = MutableByteSpan{check}.first(*n);
                expected.Fill(want);
                REQUIRE_MESSAGE(
                    std::equal(got.begin(), got.end(), want.begin()),
                    "the echo differs within " << *n << " bytes of offset "
                                               << echoed);
                echoed += *n;
            }
        }
        mostInFlight = std::max(mostInFlight, written - echoed);
    }

    CHECK(written == BURST_BYTES);
    CHECK(echoed == BURST_BYTES);
    CHECK(mostInFlight <= IN_FLIGHT_BOUND);
    MESSAGE("most in flight: " << mostInFlight << " bytes");
}
