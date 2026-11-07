/// Shared harness for the agent tests: a GridPump helper set and the real
/// agent pump loop on a TCP listener with a real pty (pump_test_api.hpp), with
/// real ClientConnections to talk to it.  Everything lives in an anonymous
/// namespace so each translation unit gets its own copy.
#pragma once

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
#include <span>
#include <string>
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
#include "zet/grid/snapshot.hpp"
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
[[maybe_unused]] std::unique_ptr<GridPump> MakePump(std::uint16_t cols = 80,
                                                    std::uint16_t rows = 24) {
    auto r = GridPump::Create(cols, rows);
    REQUIRE(r.has_value());
    return std::move(*r);
}

/// OnNeedsSnapshot, unwrapped: REQUIREs that a snapshot was built at once.
[[maybe_unused]] GridPump::SnapReady BuildNow(GridPump& p, std::uint16_t rows,
                                              std::uint16_t cols) {
    auto r = p.OnNeedsSnapshot(rows, cols, START);
    REQUIRE(r.has_value());
    return *r;
}

[[maybe_unused]] std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out;
    for (char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// `bytes` parsed into a fresh cols × rows grid: what a terminal that has been
/// handed a snapshot shows.
[[maybe_unused]] std::unique_ptr<Grid> ParseInto(
    std::span<const std::byte> bytes, std::uint16_t cols, std::uint16_t rows) {
    auto g = Grid::Create(cols, rows);
    REQUIRE(g.has_value());
    std::unique_ptr<Grid> out{*g};
    VtParser parser;
    parser.Feed(bytes.data(), bytes.size(), *out);
    return out;
}

/// The text of one screen row: blank cells are spaces, the right-hand blanks
/// are cut, a wide glyph's continuation cell is skipped.
[[maybe_unused]] std::string RowText(const Grid& g, std::uint16_t row) {
    std::string text;
    for (std::uint16_t col = 0; col < g.Cols(); ++col) {
        const Cell& cell = g.CellAt(row, col);
        if (cell.Width == 0) {
            continue;
        }
        text += cell.Codepoint == 0
                    ? ' '
                    : static_cast<char>(cell.Codepoint < 0x80 ? cell.Codepoint
                                                              : U'?');
    }
    const auto end = text.find_last_not_of(' ');
    text.resize(end == std::string::npos ? 0 : end + 1);
    return text;
}

/// ESC byte — takes the VT parser out of Ground state.
[[maybe_unused]] constexpr std::byte ESC_BYTE{0x1B};

[[maybe_unused]] bool ConnectAndClose(Rig& r) {
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
[[maybe_unused]] std::vector<std::byte> ReferenceSnapshot(
    const std::vector<std::vector<std::byte>>& feeds, std::uint16_t rows,
    std::uint16_t cols) {
    auto ref = MakePump(80, 24);
    for (const auto& f : feeds) {
        ref->FeedBytes(f.data(), f.size());
    }
    const auto ready = BuildNow(*ref, rows, cols);
    return {ready.Bytes.begin(), ready.Bytes.end()};
}

[[maybe_unused]] void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

// ── Real-agent harness ─────────────────────────────────────────────────────

/// Connects a non-blocking TCP socket to 127.0.0.1:port; invalid fd on failure.
[[maybe_unused]] io::UniqueFd TcpConnect(std::uint16_t port) {
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

    /// One turn of the connection: read, pump, write.
    void Pump() { Step(); }

    /// True once the connection has ended: the socket closed or the frame
    /// layer failed.
    [[nodiscard]] bool Ended() const { return Closed || Failed; }

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
        // No more than the frame layer takes: a whole frame is worked through
        // by Pump before the next read.
        const std::size_t room = std::min<std::size_t>(Cc_.ReadRoom(), 65536);
        if (room > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            std::array<std::byte, 65536> buf{};
            auto n =
                io::ReadSome(Sock_.Get(), MutableByteSpan{buf}.first(room));
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
            // A peer that hung up gets nothing (a write would raise SIGPIPE).
            while (!bytes.empty() && !Closed) {
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
    bool ParserInGround{false};
    bool PossiblyDiverged{false};
    /// A snapshot of the grid at the moment the loop returned.
    std::vector<std::byte> GridSnapshot;
};

/// RunAgentForTest on a TCP listener and a real pty.  The test holds the pty's
/// slave and writes there what the "shell" prints.
class AgentRig {
public:
    explicit AgentRig(std::uint8_t key) : AgentRig(key, TinyLimits()) {}

    AgentRig(std::uint8_t key, SessionLimits limits)
        : Lim_(limits),
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
                        Seen.ParserInGround =
                            gp.ParserState() == EParserState::Ground;
                        Seen.PossiblyDiverged = gp.GetGrid().PossiblyDiverged();
                        if (auto snap = Snapshot::Create(gp.GetGrid())) {
                            const auto bytes = (*snap)->Bytes();
                            Seen.GridSnapshot.assign(bytes.begin(),
                                                     bytes.end());
                            delete *snap;
                        }
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

    /// What the shell prints.  A full pty is waited out: the agent drains it.
    void PtyPrints(const std::vector<std::byte>& data) {
        ByteSpan remaining{data};
        while (!remaining.empty()) {
            auto w = io::WriteSome(Slave_.Get(), remaining);
            if (!w && w.error().WouldBlock()) {
                SleepMs(1);
                continue;
            }
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
