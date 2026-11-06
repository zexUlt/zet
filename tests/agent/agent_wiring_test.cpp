/// Agent wiring tests: GridPump unit tests plus tests that run the real agent
/// pump loop (src/agent/agent.cpp) through pump_test_api.hpp.
///
/// The agent tests (prefix "Agent:") start RunAgentForTest on a TCP listener
/// with a real pty in place of the shell, connect real ClientConnections, and
/// look at what comes out: snapshots on the wire and
/// the grid the agent built (AgentTestConfig::Inspect).  Each one is aimed at
/// one line of agent.cpp's glue; the mutation it kills is named in its header
/// and was checked by applying the mutant by hand.
///
/// Agent glue (src/agent/agent.cpp) and its killing tests:
///   MovePtyOutput does not call FeedBytes     "Agent: pty bytes reach the grid
///                                              exactly once"
///   GridFed_ does not advance                 "Agent: bytes the session cannot
///                                              take are fed to the grid once"
///   PtyFrom_ = PtyTo_ missing, TrySupply...   "Agent: bytes fed while waiting
///                                              for Ground are not replayed"
///   PtyFrom_ = PtyTo_ missing, inline supply  "Agent: bytes fed while waiting
///                                              for the Resize are not
///                                              replayed"
///   GridEmissionDone never true / Release...  "Agent: a second snapshot cycle
///                                              succeeds and frees the first"
///   Resized not forwarded to GridPump         "Agent: Resized is forwarded to
///                                              the grid"
///   no clear of PtyInput_ once the child has  "Agent: input for a shell that
///   gone (RunLoop)                             has gone is dropped and
///                                              acknowledged"
///
/// GridPump units (tests 1-7 and 9-11) call GridPump
/// directly; they cover GridPump's own logic, not the agent glue.  Test 8 is a
/// session-level regression test of AgentConnection::SupplySnapshot, not of
/// agent.cpp.
///
/// PtyFrom_ = PtyTo_ is NOT redundant: while a snapshot is pending the session
/// is Live with a full ring, so AcceptsWrites() is false and pty bytes pile up
/// in PtyRead_ after the grid has seen them; the assignment drops them so the
/// new stream generation does not replay what the snapshot already holds.

#include "../../src/agent/grid_pump.hpp"
#include "../../src/agent/pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"

namespace {

using namespace zet;
using namespace zet::agent;
using namespace zet::session;
using namespace zet::grid;
using namespace zet::wire;
using namespace std::chrono_literals;

// ── Helpers ────────────────────────────────────────────────────────────────

/// Make a GridPump at 80×24; REQUIRE creation succeeds.
std::unique_ptr<GridPump> MakePump(std::uint16_t cols = 80,
                                   std::uint16_t rows = 24) {
    auto r = GridPump::Create(cols, rows);
    REQUIRE(r.has_value());
    return std::move(*r);
}

/// OnNeedsSnapshot, unwrapped: REQUIREs that a snapshot was built at once.
GridPump::SnapReady BuildNow(GridPump& p, std::uint16_t rows,
                             std::uint16_t cols) {
    auto r = p.OnNeedsSnapshot(rows, cols, START);
    REQUIRE(r.has_value());
    return *r;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out;
    for (char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// ESC byte — takes the VT parser out of Ground state.
constexpr std::byte ESC_BYTE{0x1B};

bool ConnectAndClose(Rig& r) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    if (!DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces)) {
        return false;
    }
    ac.Close(START);
    return true;
}

/// The snapshot a fresh GridPump produces after `feeds` are fed to it in
/// order, taken at rows × cols.
std::vector<std::byte> ReferenceSnapshot(
    const std::vector<std::vector<std::byte>>& feeds, std::uint16_t rows,
    std::uint16_t cols) {
    auto ref = MakePump(80, 24);
    for (const auto& f : feeds) {
        ref->FeedBytes(f.data(), f.size());
    }
    const auto ready = BuildNow(*ref, rows, cols);
    return {ready.Bytes.begin(), ready.Bytes.end()};
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

// ── Real-agent harness ─────────────────────────────────────────────────────

/// Connects a non-blocking TCP socket to 127.0.0.1:port; invalid fd on failure.
io::UniqueFd TcpConnect(std::uint16_t port) {
    auto addrs = io::Resolve("127.0.0.1", port);
    if (!addrs || addrs->Count == 0) {
        return io::UniqueFd{};
    }
    auto fd = io::StartConnect(addrs->Items[0]);
    if (!fd) {
        return io::UniqueFd{};
    }
    pollfd pf{.fd = fd->Get(), .events = POLLOUT, .revents = 0};
    if (::poll(&pf, 1, 3000) <= 0) {
        return io::UniqueFd{};
    }
    if (!io::FinishConnect(fd->Get())) {
        return io::UniqueFd{};
    }
    return std::move(*fd);
}

/// One ClientConnection over a TCP socket to the agent under test.
class TestClient {
public:
    TestClient(ClientSession& session, std::uint64_t nonce, io::UniqueFd sock)
        : Sock_(std::move(sock)),
          Cc_(session, MakeNonce(nonce), MutableByteSpan{Frames_},
              io::MonotonicNow(),
              /*capable=*/true, PROTOCOL_VERSION_GRID,
              MutableByteSpan{SnapBuf_}) {}

    void QueueResize(std::uint16_t rows, std::uint16_t cols) {
        Cc_.QueueResize(ResizeMessage{.Rows = rows, .Cols = cols});
    }

    [[nodiscard]] bool IsLive() const { return Cc_.IsLive(); }

    /// Pumps the connection for `ms`, recording what arrives.
    void Run(int ms) {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
        while (std::chrono::steady_clock::now() < end && !Failed && !Closed) {
            Step();
        }
    }

    /// Pumps until `done()` or `ms` elapse.
    template <typename TPred>
    bool RunUntil(int ms, TPred done) {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
        while (!done() && std::chrono::steady_clock::now() < end && !Failed &&
               !Closed) {
            Step();
        }
        return done();
    }

    std::vector<std::byte> Delivered;
    std::vector<std::vector<std::byte>> Snapshots;
    /// Frames sent since the client went Live: the Resume, then the Resize.
    int LiveFramesSent{0};
    bool Closed{false};
    bool Failed{false};

private:
    void Step() {
        pollfd pf{.fd = Sock_.Get(), .events = POLLIN, .revents = 0};
        ::poll(&pf, 1, 5);
        if (pf.revents & (POLLIN | POLLHUP)) {
            std::array<std::byte, 65536> buf{};
            auto n = io::ReadSome(Sock_.Get(), MutableByteSpan{buf});
            if (n && *n > 0) {
                std::ignore = Cc_.OnBytes(ByteSpan{buf}.first(*n));
            } else if (!n && !n.error().WouldBlock()) {
                Closed = true;
            } else if (n && *n == 0) {
                Closed = true;
            }
        }
        // Everything this turn produces goes out in one write, so a Resume and
        // the Resize queued behind it reach the agent in one read — as they do
        // when a real client's socket is not drained between its frames.
        std::vector<std::byte> outgoing;
        const auto flush = [&] {
            ByteSpan bytes{outgoing};
            while (!bytes.empty()) {
                auto w = io::WriteSome(Sock_.Get(), bytes);
                if (!w) {
                    if (w.error().WouldBlock()) {
                        pollfd wpf{
                            .fd = Sock_.Get(), .events = POLLOUT, .revents = 0};
                        ::poll(&wpf, 1, 10);
                        continue;
                    }
                    Closed = true;
                    return;
                }
                bytes = bytes.subspan(*w);
            }
        };
        for (;;) {
            auto r = Cc_.Pump(io::MonotonicNow(), MutableByteSpan{Out_},
                              MutableByteSpan{Scratch_});
            if (!r) {
                Failed = true;
                flush();
                return;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*r)) {
                if (Cc_.IsLive()) {
                    ++LiveFramesSent;
                }
                outgoing.insert(
                    outgoing.end(), Out_.begin(),
                    Out_.begin() + static_cast<std::ptrdiff_t>(sf->Written));
                continue;
            }
            if (const auto* d = std::get_if<Deliver>(&*r)) {
                Delivered.insert(Delivered.end(), d->Bytes.begin(),
                                 d->Bytes.end());
                continue;
            }
            if (const auto* as = std::get_if<ApplySnapshot>(&*r)) {
                Snapshots.emplace_back(as->Bytes.begin(), as->Bytes.end());
                Cc_.ConfirmApply(as->StreamGen);
                continue;
            }
            flush();
            return;  // Idle or an event the tests do not care about.
        }
    }

    io::UniqueFd Sock_;
    std::array<std::byte, CONNECTION_FRAME_STORAGE> Frames_{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch_{};
    std::vector<std::byte> SnapBuf_ =
        std::vector<std::byte>(wire::SNAPSHOT_MAX_BYTES);
    ClientConnection Cc_;
};

/// What the agent's GridPump looked like when the loop returned.
struct GridObservation {
    bool Seen{false};
    std::size_t GridBytes{0};
    std::uint16_t Cols{0};
    std::uint16_t Rows{0};
    char32_t Cell00{0};
    bool HasSnapshot{false};
    bool HasPendingSnap{false};
};

/// RunAgentForTest on a TCP listener and a real pty.  The test holds the pty's
/// slave and writes there what the "shell" prints.
class AgentRig {
public:
    explicit AgentRig(std::uint8_t key)
        : Lim_(TinyLimits()),
          Cs_(Lim_.DisconnectMax + 256),
          Sess_(DeriveSessionSecrets(MakeKey(key)),
                SessionStreams{.Storage = MutableByteSpan{Cs_},
                               .Now = io::MonotonicNow(),
                               .Limits = Lim_}) {
        auto pty = io::OpenPty();
        REQUIRE(pty.has_value());
        Slave_ = std::move(pty->Slave);
        // Raw mode: no echo and no newline translation between the test and
        // the agent.
        termios t{};
        REQUIRE(::tcgetattr(Slave_.Get(), &t) == 0);
        ::cfmakeraw(&t);
        REQUIRE(::tcsetattr(Slave_.Get(), TCSANOW, &t) == 0);

        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        Port = listener->Port;

        // A joined thread rather than std::async: the async state is ready
        // before its thread has freed its own bookkeeping, and a test that
        // ends in that window is reported as a leak.
        Thread_ = std::thread([this, key, l = std::move(*listener),
                               m = std::move(pty->Master)]() mutable {
            Result_ = RunAgentForTest(AgentTestConfig{
                .Secrets = DeriveSessionSecrets(MakeKey(key)),
                .Limits = Lim_,
                .Listen = std::move(l),
                .PtyReadFd = std::move(m),
                .Ceiling = PROTOCOL_VERSION_GRID,
                .GiveUpAfter = std::chrono::seconds{60},
                .Inspect =
                    [this](const GridPump& gp) {
                        Seen.Seen = true;
                        Seen.GridBytes = gp.TotalBytesProcessed();
                        Seen.Cols = gp.GetGrid().Cols();
                        Seen.Rows = gp.GetGrid().Rows();
                        Seen.Cell00 = gp.GetGrid().CellAt(0, 0).Codepoint;
                        Seen.HasSnapshot = gp.HasSnapshot();
                        Seen.HasPendingSnap = gp.HasPendingSnap();
                    },
                .Stop = &Stop_,
            });
        });
    }

    AgentRig(const AgentRig&) = delete;
    AgentRig& operator=(const AgentRig&) = delete;

    ~AgentRig() { Stop(); }

    /// Stops the agent loop and returns what its grid held.
    const GridObservation& Finish() {
        Stop();
        return Seen;
    }

    /// Waits for `client` to receive a snapshot.
    bool AwaitSnapshot(TestClient& client, int ms = 10000) {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
        while (client.Snapshots.empty() && !client.Failed && !client.Closed &&
               std::chrono::steady_clock::now() < end) {
            client.Run(200);
        }
        return !client.Snapshots.empty();
    }

    std::unique_ptr<TestClient> Connect() {
        auto sock = TcpConnect(Port);
        REQUIRE(sock.Get() != -1);
        return std::make_unique<TestClient>(Sess_, ++Nonces_, std::move(sock));
    }

    /// A client that sends `rows × cols`, goes Live, and is then dropped once
    /// the agent has taken the Resize.
    void ConnectAndDrop(std::uint16_t rows, std::uint16_t cols) {
        auto c = Connect();
        c->QueueResize(rows, cols);
        REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 2; }));
        AwaitPtySize(*c, rows, cols);
    }

    /// Returns once the agent has applied a Resize to the pty.  The agent
    /// takes frames in order, so the Resume and everything before the Resize
    /// are behind it by then.  Needs a size the pty does not have yet.
    void AwaitPtySize(TestClient& client, std::uint16_t rows,
                      std::uint16_t cols) {
        REQUIRE(client.RunUntil(8000, [&] {
            const auto ws = SlaveSize();
            return ws.ws_row == rows && ws.ws_col == cols;
        }));
    }

    /// Returns once the agent has taken `client`'s Resume: it sends the shell's
    /// output only to a connection it has resumed, and in order.  Prints one
    /// byte for it.
    void AwaitResumeTaken(TestClient& client) {
        const auto before = client.Delivered.size();
        PtyPrints(Bytes("."));
        REQUIRE(client.RunUntil(
            8000, [&] { return client.Delivered.size() > before; }));
    }

    /// The window size the pty reports to the shell.
    winsize SlaveSize() {
        winsize ws{};
        ::ioctl(Slave_.Get(), TIOCGWINSZ, &ws);
        return ws;
    }

    ClientSession& Session() { return Sess_; }

    /// What the shell prints.
    void PtyPrints(const std::vector<std::byte>& data) {
        ByteSpan remaining{data};
        while (!remaining.empty()) {
            auto w = io::WriteSome(Slave_.Get(), remaining);
            REQUIRE(w.has_value());
            remaining = remaining.subspan(*w);
        }
    }

    std::uint16_t Port{0};
    GridObservation Seen;

private:
    void Stop() {
        if (Stopped_) {
            return;
        }
        Stopped_ = true;
        Stop_ = true;
        std::ignore = TcpConnect(Port);
        Thread_.join();
    }

    bool Stopped_{false};
    std::atomic<bool> Stop_{false};
    SessionLimits Lim_;
    std::vector<std::byte> Cs_;
    ClientSession Sess_;
    io::UniqueFd Slave_;
    std::uint64_t Nonces_{0};
    std::thread Thread_;
    int Result_{0};
};

}  // namespace

// ── Test 1: FeedBytes writes into the grid ─────────────────────────────────
//
// Kills: FeedBytes does not parse (cells stay blank).

TEST_CASE_FIXTURE(SodiumFixture, "FeedBytes writes byte into grid cell") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('A')};
    p->FeedBytes(&b, 1);
    CHECK(p->GetGrid().CellAt(0, 0).Codepoint == static_cast<char32_t>('A'));
    CHECK(p->TotalBytesProcessed() == 1);
}

// ── Test 2: FeedBytes advances cursor exactly once ─────────────────────────
//
// Kills: FeedBytes feeds the parser twice (cursor at col 4, not 2).

TEST_CASE_FIXTURE(SodiumFixture,
                  "FeedBytes advances cursor by exactly the bytes fed") {
    auto p = MakePump();
    const std::array<std::byte, 2> bytes{std::byte{'A'}, std::byte{'B'}};
    p->FeedBytes(bytes.data(), 2);
    CHECK(p->GetGrid().CursorCol() == 2);
    CHECK(p->TotalBytesProcessed() == 2);
}

// ── Test 3: HandleResize updates grid dimensions ───────────────────────────
//
// Kills: HandleResize does not resize the grid.

TEST_CASE_FIXTURE(SodiumFixture,
                  "HandleResize resizes grid to requested dimensions") {
    auto p = MakePump();
    auto r = p->HandleResize(40, 10);
    REQUIRE(r.has_value());
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);
}

// ── Test 4: OnNeedsSnapshot builds at once from Ground ─────────────────────
//
// Kills: OnNeedsSnapshot never builds or arms the wait.

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot in Ground builds a snapshot at once") {
    auto p = MakePump();
    CHECK_FALSE(p->HasPendingSnap());

    const auto ready = BuildNow(*p, 24, 80);
    CHECK_FALSE(ready.Bytes.empty());
    CHECK(p->HasSnapshot());
    CHECK_FALSE(p->HasPendingSnap());  // cleared by DoBuild

    p->ReleaseSnapshot();
}

// ── Test 5: Snapshot bytes are non-empty ──────────────────────────────────
//
// Kills: DoBuild returns empty Bytes.

TEST_CASE_FIXTURE(SodiumFixture, "Snapshot bytes carry grid content") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('Z')};
    p->FeedBytes(&b, 1);

    const auto ready = BuildNow(*p, 24, 80);
    CHECK_FALSE(ready.Bytes.empty());

    p->ReleaseSnapshot();
}

// ── Test 6: Ground-wait timeout → PossiblyDiverged flag set ───────────────
//
// Kills: TryBuild never fires on the time limit, or fires without setting
// PossiblyDiverged.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Ground-wait timeout fires and sets PossiblyDiverged") {
    auto p = MakePump();

    // Take the parser out of Ground with ESC.
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    // Arm ground-wait; not in Ground so nothing is built yet.
    auto immediate = p->OnNeedsSnapshot(24, 80, START);
    CHECK_FALSE(immediate.has_value());
    CHECK(p->HasPendingSnap());

    // Before timeout: still waiting.
    {
        auto r = p->TryBuild(START);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->has_value());
    }

    // After timeout: snapshot is forced with PossiblyDiverged.
    const Timestamp past =
        START + GROUND_WAIT_TIMEOUT + std::chrono::milliseconds{1};
    auto r = p->TryBuild(past);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->Flags != 0);  // bit 0 = PossiblyDiverged

    p->ReleaseSnapshot();
}

// ── Test 7: Snapshot taken at requested size, not original grid size ───────
//
// Kills: DoBuild skips the grid resize (grid stays 80×24, not 40×10).

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot resizes grid to requested dimensions") {
    auto p = MakePump(80, 24);

    // Request a snapshot at 40×10 (different from grid's current 80×24).
    std::ignore = BuildNow(*p, /*rows=*/10, /*cols=*/40);
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);

    p->ReleaseSnapshot();
}

// ── Test 8: bytes dropped during overflow do not appear after snapshot ──────
//
// Session-level regression test of AgentConnection::SupplySnapshot (it drives
// AgentConnection directly, not agent.cpp).  Kills: SupplySnapshot does not
// start the new stream generation, so the ring content from before the
// overflow is replayed as Deliver 0xBB events.

TEST_CASE_FIXTURE(SodiumFixture,
                  "bytes dropped during overflow do not appear as Deliver "
                  "after snapshot is applied") {
    Rig r;
    REQUIRE(ConnectAndClose(r));

    // Overflow chunk: 0xBB (dropped in Resync).
    const std::vector<std::byte> overflow(300, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) std::ignore = r.Agent.Write(ByteSpan{overflow});

    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    // The agent waits for this before asking for a snapshot (design.md §9,
    // path B, step 1), and the clock here is frozen.
    cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));

    const std::vector<std::byte> snapData(16, std::byte{0x11});
    bool snapshotApplied = false;
    for (int i = 0; i < 80 && !snapshotApplied; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::get_if<NeedsSession>(&*ea)) {
            ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
        } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac.SupplySnapshot(START, ByteSpan{snapData}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            snapshotApplied = true;
            cc.ConfirmApply(as->StreamGen);
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
    }
    REQUIRE(snapshotApplied);

    // Sentinel bytes — distinct from 0xBB overflow.
    const std::vector<std::byte> sentinel(8, std::byte{0xCC});
    std::ignore = r.Agent.Write(ByteSpan{sentinel});

    int deliverCount = 0;
    bool sawOverflowByte = false;
    for (int i = 0; i < 120; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* d = std::get_if<Deliver>(&*ec)) {
            ++deliverCount;
            for (auto byte : d->Bytes) {
                if (byte == std::byte{0xBB}) {
                    sawOverflowByte = true;
                }
            }
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
        if (deliverCount > 0) {
            break;
        }
    }
    CHECK(deliverCount > 0);
    CHECK_FALSE(sawOverflowByte);
}

// ── Test 9: ReleaseSnapshot clears HasSnapshot ─────────────────────────────
//
// Kills: ReleaseSnapshot does not free (HasSnapshot stays true).

TEST_CASE_FIXTURE(SodiumFixture, "ReleaseSnapshot clears HasSnapshot") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('X')};
    p->FeedBytes(&b, 1);

    std::ignore = BuildNow(*p, 24, 80);
    CHECK(p->HasSnapshot());

    p->ReleaseSnapshot();
    CHECK_FALSE(p->HasSnapshot());
}

// ── Test 10: 1 MiB byte cap fires ground-wait ─────────────────────────────
//
// Kills: TryBuild does not check the byte cap.  The clock stays at START, so
// only the byte cap can make TryBuild fire; one byte under the cap must not.

TEST_CASE_FIXTURE(SodiumFixture,
                  "1 MiB byte cap fires ground-wait when parser non-Ground") {
    auto p = MakePump();
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    auto immediate = p->OnNeedsSnapshot(24, 80, START);
    CHECK_FALSE(immediate.has_value());
    CHECK(p->HasPendingSnap());

    // One byte short of the cap: still waiting.
    const std::vector<std::byte> filler(GROUND_WAIT_BYTES - 1, std::byte{0x00});
    p->FeedBytes(filler.data(), filler.size());
    REQUIRE(p->ParserState() != EParserState::Ground);
    {
        auto r = p->TryBuild(START);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->has_value());
    }

    // The byte that reaches the cap: the build fires, at the same clock.
    p->FeedBytes(&ESC_BYTE, 1);
    auto r = p->TryBuild(START);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->Flags != 0);  // forced out of a non-Ground parser
    p->ReleaseSnapshot();
}

// ── Test 11: OnNeedsSnapshot(0,0) on non-80×24 grid uses actual size ───────
//
// Kills: 0×0 means 80×24 instead of the grid's current size (the snapshot
// would be 80×24 instead of 40×10).

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot(0,0) on non-80x24 grid uses actual size") {
    auto p = MakePump(40, 10);
    const auto ready = BuildNow(*p, 0, 0);
    const std::vector<std::byte> got{ready.Bytes.begin(), ready.Bytes.end()};
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);

    auto ref = MakePump(40, 10);
    const auto refReady = BuildNow(*ref, 10, 40);
    const std::vector<std::byte> want{refReady.Bytes.begin(),
                                      refReady.Bytes.end()};
    CHECK(got == want);
    p->ReleaseSnapshot();
}

// ── Agent tests ────────────────────────────────────────────────────────────
//
// Shared shape for the snapshot tests: connection 1 is grid-capable, goes Live
// and is dropped, so the session is Detached with Resync overflow.  Printing
// more than DisconnectMax (512) bytes then overflows the ring, which makes
// connection 2 take path B: the agent asks for a snapshot (NeedsSnapshot).

constexpr std::size_t OVERFLOW_BYTES = 600;

// ── Agent: pty bytes reach the grid exactly once ───────────────────────────
//
// Kills: MovePtyOutput does not call GridPump_->FeedBytes (the snapshot is of
// a blank grid), and a FeedBytes call that feeds a range twice (the snapshot
// differs from the reference).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: pty bytes reach the grid exactly once") {
    AgentRig rig{71};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    const std::vector<std::byte> printed(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(24, 80);
    REQUIRE(rig.AwaitSnapshot(*c2));

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.GridBytes == OVERFLOW_BYTES);
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({printed}, 24, 80));
}

// ── Agent: bytes the session cannot take are fed to the grid once ──────────
//
// A Live client that never acks leaves the ring full, so the rest of what the
// shell printed stays in PtyRead_ (PtyFrom_ < PtyTo_) and MovePtyOutput runs
// on every loop turn.  Extra connections force loop turns.
//
// Kills: GridFed_ does not advance after FeedBytes (the same bytes are fed on
// every turn, GridBytes is a multiple of 1000).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes the session cannot take are fed to the grid "
                  "once") {
    AgentRig rig{72};
    auto idle = rig.Connect();
    idle->QueueResize(24, 80);
    REQUIRE(idle->RunUntil(8000, [&] { return idle->IsLive(); }));
    // `idle` is not pumped from here on, so it never acks.

    const std::vector<std::byte> printed(1000, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(200);

    std::vector<io::UniqueFd> pokes;
    for (int i = 0; i < 5; ++i) {
        pokes.push_back(TcpConnect(rig.Port));
        SleepMs(50);
    }

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.GridBytes == printed.size());
    CHECK(seen.Cell00 == static_cast<char32_t>('A'));
}

// ── Agent: bytes fed while waiting for Ground are not replayed ─────────────
//
// The shell stops inside an escape sequence, so the snapshot waits for Ground
// (TrySupplySnapshot).  The rest of the sequence arrives during the wait: the
// grid takes it, the session (Live, ring full) cannot, and once the snapshot
// is supplied those bytes must not be written to the new generation.
//
// Kills: `PtyFrom_ = PtyTo_` removed from TrySupplySnapshot (the client gets
// "[HQ" again after the snapshot).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes fed while waiting for Ground are not "
                  "replayed") {
    AgentRig rig{73};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    std::vector<std::byte> first(OVERFLOW_BYTES, std::byte{'A'});
    first.push_back(ESC_BYTE);
    rig.PtyPrints(first);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(24, 80);
    c2->Run(300);
    c2->Run(200);
    REQUIRE(c2->Snapshots.empty());  // parser is inside the escape

    const auto rest = Bytes("[HQ");  // CSI H, then a printable
    rig.PtyPrints(rest);
    REQUIRE(rig.AwaitSnapshot(*c2));
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({first, rest}, 24, 80));

    c2->Run(300);
    rig.PtyPrints(Bytes("Z"));
    c2->Run(500);
    CHECK(c2->Delivered == Bytes("Z"));
}

// ── Agent: bytes fed while waiting for the Resize are not replayed ─────────
//
// The client withholds its post-Resume Resize, so the agent holds off
// NeedsSnapshot (up to RESIZE_WAIT_TIMEOUT).  The shell prints during that
// time; the parser is in Ground, so the snapshot is supplied inline in Pump
// as soon as the Resize arrives.
//
// Kills: `PtyFrom_ = PtyTo_` removed from the inline supply in Pump.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes fed while waiting for the Resize are not "
                  "replayed") {
    AgentRig rig{74};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    const std::vector<std::byte> first(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(first);
    SleepMs(300);

    auto c2 = rig.Connect();  // no QueueResize yet
    c2->Run(300);
    REQUIRE(c2->Snapshots.empty());

    const auto rest = Bytes("QQQ");
    rig.PtyPrints(rest);
    SleepMs(200);

    c2->QueueResize(24, 80);
    REQUIRE(rig.AwaitSnapshot(*c2));
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({first, rest}, 24, 80));

    c2->Run(300);
    rig.PtyPrints(Bytes("Z"));
    c2->Run(500);
    CHECK(c2->Delivered == Bytes("Z"));
}

// ── Agent: a reconnect snapshot does not wait out RESIZE_WAIT_TIMEOUT ───────
//
// The client's Resize follows its Resume, usually in the same read.  The agent
// must take it off the buffer on the same turn; if it does not, NeedsSnapshot
// waits for RESIZE_WAIT_TIMEOUT (2 s) with nothing to wake the loop.
//
// Kills: PumpResume returning Idle after the path-B stage change.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a reconnect snapshot does not wait out "
                  "RESIZE_WAIT_TIMEOUT") {
    AgentRig rig{77};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    rig.PtyPrints(std::vector<std::byte>(OVERFLOW_BYTES, std::byte{'A'}));
    SleepMs(300);

    auto c = rig.Connect();
    c->QueueResize(24, 80);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig.AwaitSnapshot(*c));
    CHECK(std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds{1500});
}

// ── Agent: no Resize and no known size: the snapshot follows the wait ───────
//
// With no size ever reported the agent still waits for the Resize (design.md
// §9, step 1).  None comes, nothing else is on the wire, and only the
// connection's deadline can wake the loop; after RESIZE_WAIT_TIMEOUT the
// snapshot is built at the grid's size.
//
// Kills: no wake-up for the Resize wait when no size is known.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: with no Resize and no known size the snapshot "
                  "follows the Resize wait") {
    AgentRig rig{79};
    {
        auto c = rig.Connect();  // never sends a Resize
        REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 1; }));
        rig.AwaitResumeTaken(*c);
    }
    SleepMs(300);  // the agent notices the drop
    rig.PtyPrints(std::vector<std::byte>(OVERFLOW_BYTES, std::byte{'A'}));
    SleepMs(300);

    auto c = rig.Connect();
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig.AwaitSnapshot(*c, 6000));
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= std::chrono::milliseconds{1500});
    CHECK(took < std::chrono::milliseconds{4000});
}

// ── Agent: the pty gets the size the grid has ──────────────────────────────
//
// A Resize of 0×0 is accepted; the grid clamps it to 1×1, and the pty must be
// told the same, not 0×0.
//
// Kills: the pty resized with the raw Resize instead of the grid's size.

TEST_CASE_FIXTURE(SodiumFixture, "Agent: a 0x0 Resize leaves the pty at 1x1") {
    AgentRig rig{80};
    auto c = rig.Connect();
    c->QueueResize(30, 100);
    REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 2; }));
    rig.AwaitPtySize(*c, 30, 100);

    c->QueueResize(0, 0);
    REQUIRE(c->RunUntil(8000, [&] {
        const auto ws = rig.SlaveSize();
        return ws.ws_row != 30 || ws.ws_col != 100;
    }));
    const auto size = rig.SlaveSize();
    CHECK(size.ws_row == 1);
    CHECK(size.ws_col == 1);
}

// ── Agent: a second snapshot cycle succeeds and frees the first ────────────
//
// Cycle 2 waits for Ground, which is the path that needs the first snapshot
// released.
//
// Kills: GridEmissionDone never returns true / is not consulted, and
// ReleaseSnapshot never called (HasSnapshot stays true at exit and the second
// wait never builds).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a second snapshot cycle succeeds and frees the "
                  "first") {
    AgentRig rig{75};

    // Cycle 1.
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    const std::vector<std::byte> a(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(a);
    SleepMs(300);
    {
        auto c = rig.Connect();
        c->QueueResize(24, 80);
        REQUIRE(rig.AwaitSnapshot(*c));
        c->Run(300);  // GridEnd sent, GridApplied back: the pump releases.
    }
    SleepMs(300);

    // Cycle 2: overflow again, with the parser left inside an escape.
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    std::vector<std::byte> b(OVERFLOW_BYTES, std::byte{'B'});
    b.push_back(ESC_BYTE);
    rig.PtyPrints(b);
    SleepMs(300);
    auto c4 = rig.Connect();
    c4->QueueResize(24, 80);
    c4->Run(300);
    const auto rest = Bytes("[H");
    rig.PtyPrints(rest);
    REQUIRE(rig.AwaitSnapshot(*c4));
    c4->Run(300);

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK_FALSE(seen.HasSnapshot);
    CHECK_FALSE(seen.HasPendingSnap);
}

// ── Agent: Resized is forwarded to the grid ───────────────────────────────
//
// Kills: the Resized handler does not call GridPump_->HandleResize (the grid
// stays 80×24 while the shell prints at 40 columns, so the snapshot differs
// from the reference).

TEST_CASE_FIXTURE(SodiumFixture, "Agent: Resized is forwarded to the grid") {
    AgentRig rig{76};
    rig.ConnectAndDrop(10, 40);
    SleepMs(300);

    const std::vector<std::byte> printed(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(10, 40);
    REQUIRE(rig.AwaitSnapshot(*c2));

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.Cols == 40);
    CHECK(seen.Rows == 10);

    // The grid was resized before the shell's bytes were fed.
    auto ref = MakePump(80, 24);
    REQUIRE(ref->HandleResize(40, 10).has_value());
    ref->FeedBytes(printed.data(), printed.size());
    const auto ready = BuildNow(*ref, 10, 40);
    CHECK(c2->Snapshots.front() ==
          std::vector<std::byte>{ready.Bytes.begin(), ready.Bytes.end()});
}

// ── Input for a shell that has gone ────────────────────────────────────────
//
// The test seam presets the child as exited, so the pty never takes what the
// client types.  The agent drops it and acknowledges it as gone, which is how
// the client's window opens again (design.md §9); the agent does not hold on
// to input nobody will read.
//
// Kills: RunLoop does not clear PtyInput_ once the child has gone (the
// window stays full, the client never writes again).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: input for a shell that has gone is dropped and "
                  "acknowledged") {
    AgentRig rig{78};
    const std::vector<std::byte> typed(TinyLimits().ResendMax, std::byte{'k'});
    REQUIRE(rig.Session().Write(typed) == typed.size());
    CHECK_FALSE(rig.Session().AcceptsWrites());  // the window is full

    auto c = rig.Connect();
    c->QueueResize(10, 40);
    CHECK(c->RunUntil(
        8000, [&] { return c->IsLive() && rig.Session().AcceptsWrites(); }));
}
