// GridBegin / GridChunk* / GridEnd fragmentation (design.md §16, "Тест
// фрагментации"): snapshots of about 1 KiB, 256 KiB and 1 MiB go from the
// agent's channel to the client's snapshot assembly and come out whole, and a
// connection cut after the first GridChunk leaves nothing behind: the next
// connection gets a new snapshot of the then-current state.
//
// Mutants these tests were checked against (applied by hand):
//   channel.cpp: GridChunk carries Snapshot.subspan(0, n) instead of
//     subspan(BytesSent, n)                         -> both tests
//   connection.cpp: a Resume with a stale stream_gen does not force path B
//                                                   -> "cut after the first
//                                                      GridChunk"

#include "grid_path_helpers.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "zet/wire/message.hpp"

namespace {

std::vector<std::byte> Pattern(std::size_t size, std::uint8_t salt) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31 + salt) & 0xFF);
    }
    return bytes;
}

constexpr std::size_t ONE_KIB = 1024;
constexpr std::size_t QUARTER_MIB = 256 * 1024;
constexpr std::size_t ONE_MIB = 1024 * 1024;

/// A session that has been grid-capable and live once, dropped, and then
/// overflowed in Resync mode: its next connection takes path B.
void PrimeForPathB(Rig& r) {
    {
        AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{r.Client,
                            MakeNonce(++r.Nonces),
                            MutableByteSpan{r.Sp.ClientFrames},
                            START,
                            /*capable=*/true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{r.Sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
        ac.Close(START);
    }
    const std::vector<std::byte> fill(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{fill});
    }
    REQUIRE(r.Agent.State() == ESessionState::Detached);
}

/// What the client saw of one snapshot.
struct Applied {
    bool Seen{false};
    std::vector<std::byte> Bytes;
    std::uint32_t ScrollbackLen{0};
    std::uint8_t Flags{0};
    std::size_t FramesFromAgent{0};
};

/// One connection of the session, both ends, in one thread.
class Link {
public:
    explicit Link(Rig& r)
        : R_(r),
          Ac_{MutableByteSpan{r.Sp.AgentFrames}, START,
              wire::PROTOCOL_VERSION_GRID},
          Cc_{r.Client,
              MakeNonce(++r.Nonces),
              MutableByteSpan{r.Sp.ClientFrames},
              START,
              /*capable=*/true,
              wire::PROTOCOL_VERSION_GRID,
              MutableByteSpan{r.Sp.SnapshotBuf}} {
        // The agent waits for the post-Resume Resize before it asks for a
        // snapshot (design.md §9, path B, step 1).
        Cc_.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        REQUIRE(DriveToLive(Ac_, Cc_, r.Agent, r.Sp, START, r.Nonces));
    }

    /// True once the agent asks for a snapshot.
    [[nodiscard]] bool UntilNeedsSnapshot() {
        for (int i = 0; i < 200; ++i) {
            auto ea = PumpAgent(Ac_, R_.Sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                return true;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(Cc_, R_.Sp, sf->Written));
            }
            auto ec = PumpClient(Cc_, R_.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(Ac_, R_.Sp, sf->Written));
            }
        }
        return false;
    }

    /// Supplies `snap` and carries the frames across until the client applies
    /// it, or, with `cutAfterFrames` > 0, until the agent has sent that many
    /// frames (GridBegin, then GridChunks) and the connection is dropped.
    Applied Deliver(ByteSpan snap, std::uint32_t scrollbackLen,
                    std::uint8_t flags, std::size_t cutAfterFrames = 0) {
        Ac_.SupplySnapshot(START, snap, scrollbackLen, flags);
        Applied out;
        for (int i = 0; i < 4000; ++i) {
            auto ea = PumpAgent(Ac_, R_.Sp, START);
            REQUIRE(ea.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                ++out.FramesFromAgent;
                REQUIRE(Feed(Cc_, R_.Sp, sf->Written));
            }
            auto ec = PumpClient(Cc_, R_.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                out.Seen = true;
                out.Bytes.assign(as->Bytes.begin(), as->Bytes.end());
                out.ScrollbackLen = as->ScrollbackLen;
                out.Flags = as->Flags;
                Cc_.ConfirmApply(as->StreamGen);
                return out;
            }
            if (cutAfterFrames != 0 && out.FramesFromAgent >= cutAfterFrames) {
                return out;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(Ac_, R_.Sp, sf->Written));
            }
        }
        return out;
    }

    /// Pumps both ends until the client delivers output, and returns it.
    std::vector<std::byte> NextOutput() {
        std::vector<std::byte> got;
        for (int i = 0; i < 400 && got.empty(); ++i) {
            auto ea = PumpAgent(Ac_, R_.Sp, START);
            REQUIRE(ea.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(Cc_, R_.Sp, sf->Written));
            }
            auto ec = PumpClient(Cc_, R_.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* d = std::get_if<zet::session::Deliver>(&*ec)) {
                got.assign(d->Bytes.begin(), d->Bytes.end());
            }
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(Ac_, R_.Sp, sf->Written));
            }
        }
        return got;
    }

    void Drop() { Ac_.Close(START); }

private:
    Rig& R_;
    AgentConnection Ac_;
    ClientConnection Cc_;
};

std::size_t ChunksFor(std::size_t size) {
    return (size + wire::GRID_CHUNK_MAX_DATA - 1) / wire::GRID_CHUNK_MAX_DATA;
}

}  // namespace

// Kills: a GridChunk that carries the wrong slice of the snapshot (the bytes
// differ past the first chunk), a lost or repeated chunk (a different frame
// count), a scrollback length or flags that do not survive the assembly.

TEST_CASE_FIXTURE(SodiumFixture,
                  "fragmentation: snapshots of 1 KiB, 256 KiB and 1 MiB reach "
                  "the client whole") {
    for (const std::size_t size : {ONE_KIB, QUARTER_MIB, ONE_MIB}) {
        INFO("snapshot of " << size << " bytes");
        Rig r;
        PrimeForPathB(r);
        Link link{r};
        REQUIRE(link.UntilNeedsSnapshot());

        const auto snap = Pattern(size, 7);
        const auto scrollback = static_cast<std::uint32_t>(size / 4);
        const auto got = link.Deliver(ByteSpan{snap}, scrollback, 1);
        REQUIRE(got.Seen);
        CHECK(got.Bytes == snap);
        CHECK(got.ScrollbackLen == scrollback);
        CHECK(got.Flags == 1);
        // GridBegin, the chunks, GridEnd.
        CHECK(got.FramesFromAgent == ChunksFor(size) + 2);

        // The stream after the snapshot starts clean.
        const std::vector<std::byte> after(5, std::byte{0xCC});
        std::ignore = r.Agent.Write(ByteSpan{after});
        CHECK(link.NextOutput() == after);
    }
}

// Kills: a client session that counts a half-received snapshot as applied
// (the next connection takes path A and never asks for a snapshot), a stale
// partial assembly leaking into the next snapshot (the bytes differ).

TEST_CASE_FIXTURE(SodiumFixture,
                  "fragmentation: a cut after the first GridChunk is followed "
                  "by a fresh snapshot of the current state") {
    for (const std::size_t size : {ONE_KIB, QUARTER_MIB, ONE_MIB}) {
        INFO("snapshot of " << size << " bytes");
        Rig r;
        PrimeForPathB(r);

        {
            Link first{r};
            REQUIRE(first.UntilNeedsSnapshot());
            const auto stale = Pattern(size, 1);
            // GridBegin and the first GridChunk, then the link goes.
            const auto cut = first.Deliver(ByteSpan{stale}, 0, 0, 2);
            CHECK_FALSE(cut.Seen);
            CHECK(cut.FramesFromAgent == 2);
            first.Drop();
        }

        // What the shell printed meanwhile is in the new snapshot, not the
        // old one: same length, different bytes.
        Link second{r};
        REQUIRE(second.UntilNeedsSnapshot());
        const auto fresh = Pattern(size, 2);
        const auto got = second.Deliver(ByteSpan{fresh}, 0, 0);
        REQUIRE(got.Seen);
        CHECK(got.Bytes == fresh);
        CHECK(got.Bytes != Pattern(size, 1));
        CHECK(got.FramesFromAgent == ChunksFor(size) + 2);
    }
}
