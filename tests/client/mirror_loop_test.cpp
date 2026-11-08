/// The client's mirror grid in the real loop (design.md §9, M7): the Client
/// connects over loopback TCP to a listener the test owns, the test plays the
/// agent through AgentConnection, and the mirror's state is read from the
/// snapshot the client writes on exit (ZET_TEST_MIRROR_DUMP).

#include "client.hpp"
#include "pump_test_api.hpp"

#include <doctest/doctest.h>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/client_session.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;
using Clock = std::chrono::steady_clock;

struct SodiumFixture {
    SodiumFixture() {
        REQUIRE(crypto::Init());
        // A test that closes the client's stdout must see EPIPE, not die.
        ::signal(SIGPIPE, SIG_IGN);
    }
};

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t v = seed;
    for (auto& b : bytes) {
        b = std::byte{v};
        v = static_cast<std::uint8_t>(v + 7);
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint64_t counter) {
    HandshakeNonce n{};
    std::uint64_t v = counter * 0x9E3779B97F4A7C15ULL + 1;
    for (auto& b : n) {
        b = static_cast<std::byte>(v & 0xFF);
        v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return n;
}

/// Tiny limits so the ring overflows after a small write.
SessionLimits TinyLimits() {
    return SessionLimits{
        .ResendMax = 512,
        .DisconnectMax = 512,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = Duration{0},
                                   .PeerSilenceTimeout = Duration{0}},
    };
}

struct Space {
    std::array<std::byte, CONNECTION_FRAME_STORAGE> AgentFrames{};
    std::array<std::byte, CONNECTION_FRAME_STORAGE> ClientFrames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    std::vector<std::byte> SnapshotBuf{wire::SNAPSHOT_MAX_BYTES, std::byte{}};
};

// ── Agent side ────────────────────────────────────────────────────────────

/// The socket is non-blocking and a frame dropped halfway desynchronises the
/// stream, so wait for room instead of discarding what did not fit.
void WriteAll(int fd, ByteSpan bytes) {
    while (!bytes.empty()) {
        auto n = io::WriteSome(fd, bytes);
        if (n) {
            bytes = bytes.subspan(*n);
            continue;
        }
        REQUIRE(n.error().WouldBlock());
        pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
        REQUIRE(::poll(&pfd, 1, 3000) > 0);
    }
}

/// One accepted connection and the agent end of it.
struct AgentLink {
    explicit AgentLink(io::UniqueFd fd,
                       std::uint16_t ceiling = wire::PROTOCOL_VERSION_GRID)
        : Fd(std::move(fd)),
          Ac{MutableByteSpan{Sp->AgentFrames}, io::MonotonicNow(), ceiling} {}

    io::UniqueFd Fd;
    std::unique_ptr<Space> Sp = std::make_unique<Space>();
    AgentConnection Ac;

    /// Feeds what the client sent, sends up to `maxFrames` frames, and
    /// returns the first event that is neither a frame nor Idle.
    std::optional<ConnectionEvent> Tick(std::size_t maxFrames = 64) {
        const Timestamp now = io::MonotonicNow();
        std::array<std::byte, 4096> buf{};
        pollfd pfd{.fd = Fd.Get(), .events = POLLIN, .revents = 0};
        while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            auto n = io::ReadSome(Fd.Get(), MutableByteSpan{buf});
            if (!n || *n == 0) break;
            if (!Ac.OnBytes(ByteSpan{buf}.first(*n))) break;
            pfd.revents = 0;
        }
        for (std::size_t i = 0; i < maxFrames; ++i) {
            auto r = Ac.Pump(now, MutableByteSpan{Sp->Out},
                             MutableByteSpan{Sp->Scratch});
            if (!r) return std::nullopt;
            if (const auto* sf = std::get_if<SendFrame>(&*r)) {
                WriteAll(Fd.Get(), ByteSpan{Sp->Out}.first(sf->Written));
                continue;
            }
            if (std::holds_alternative<Idle>(*r)) return std::nullopt;
            return *r;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool ToLive(AgentSession& session, std::uint64_t& nonces) {
        const auto until = Clock::now() + 3s;
        while (Clock::now() < until) {
            auto ev = Tick();
            if (ev && std::holds_alternative<NeedsSession>(*ev)) {
                Ac.Route(session.Ref(), MakeNonce(++nonces));
            }
            if (Ac.IsLive()) return true;
            ::usleep(500);
        }
        return false;
    }

    [[nodiscard]] bool UntilNeedsSnapshot() {
        const auto until = Clock::now() + 3s;
        while (Clock::now() < until) {
            auto ev = Tick();
            if (ev && std::holds_alternative<NeedsSnapshot>(*ev)) return true;
            ::usleep(500);
        }
        return false;
    }

    [[nodiscard]] bool Supply(ByteSpan snap, std::uint8_t flags = 0) {
        if (!UntilNeedsSnapshot()) return false;
        Ac.SupplySnapshot(io::MonotonicNow(), snap, 0, flags);
        return true;
    }

    void Cut() {
        Ac.Close(io::MonotonicNow());
        Fd.Reset();
    }
};

/// AgentSession starts in Reap mode, so overflowing a fresh session reaps it.
/// One in-memory connection runs SetGridInfo (capable → Resync), then
/// detaches, leaving the session ready for an overflow that forces path B.
void PrimeAgentSession(AgentSession& agentSession, std::uint8_t keySeed,
                       const SessionLimits& lim, std::uint64_t& nonces) {
    const Timestamp now = io::MonotonicNow();
    auto sp = std::make_unique<Space>();
    std::vector<std::byte> clientStorage(lim.ResendMax + 256);
    ClientSession clientSession{
        DeriveSessionSecrets(MakeKey(keySeed)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = now,
                       .Limits = lim}};
    AgentConnection ac{MutableByteSpan{sp->AgentFrames}, now,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{clientSession,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp->ClientFrames},
                        now,
                        true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{sp->SnapshotBuf}};
    for (int i = 0; i < 64 && !ac.IsLive(); ++i) {
        auto ea = ac.Pump(now, MutableByteSpan{sp->Out},
                          MutableByteSpan{sp->Scratch});
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSession>(*ea)) {
            ac.Route(agentSession.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(
                cc.OnBytes(ByteSpan{sp->Out}.first(sf->Written)).has_value());
        }
        auto ec = cc.Pump(now, MutableByteSpan{sp->Out},
                          MutableByteSpan{sp->Scratch});
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(
                ac.OnBytes(ByteSpan{sp->Out}.first(sf->Written)).has_value());
        }
    }
    REQUIRE(ac.IsLive());
    ac.Close(now);
}

/// An agent session for the client to connect to.  `overflowed` leaves its
/// ring overflowed while detached in Resync mode, so the client's first
/// connection takes path B.
struct AgentSide {
    AgentSide(std::uint8_t keySeed, SessionLimits limits, bool overflowed)
        : KeySeed{keySeed},
          Limits{limits},
          Storage(std::max(limits.ResendMax, limits.DisconnectMax) + 256),
          Session{DeriveSessionSecrets(MakeKey(keySeed)),
                  SessionStreams{.Storage = MutableByteSpan{Storage},
                                 .Now = io::MonotonicNow(),
                                 .Limits = limits}} {
        if (!overflowed) return;
        PrimeAgentSession(Session, KeySeed, Limits, Nonces);
        const std::vector<std::byte> fill(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i) std::ignore = Session.Write(ByteSpan{fill});
        REQUIRE_FALSE(Session.HasEnded());
    }

    std::uint8_t KeySeed;
    SessionLimits Limits;
    std::vector<std::byte> Storage;
    AgentSession Session;
    std::uint64_t Nonces = 0;
};

// ── Client side ───────────────────────────────────────────────────────────

struct RunOptions {
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
    Duration GiveUpAfter{1s};
    /// Where the client writes the mirror on exit; empty keeps no mirror.
    std::string MirrorDump{};
    std::optional<wire::ResizeMessage> InitialSize{};
    /// stderr is the terminal too: notices go into the stdout queue.
    bool NoticesInline{false};
    std::function<std::optional<wire::ResizeMessage>()> SizeSource{};
};

/// The client under test, on its own thread, against a loopback listener.
/// Every wait is bounded, and the destructor cuts every way the client could
/// be waiting, so a failed test cannot hang the suite.
class ClientRun {
public:
    explicit ClientRun(AgentSide& agent, RunOptions options = {})
        : Agent_{agent} {
        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        Listener_ = std::move(*listener);
        auto addresses = io::Resolve("127.0.0.1", Listener_.Port);
        REQUIRE(addresses.has_value());
        auto out = io::MakePipe();
        REQUIRE(out.has_value());
        Stdout_ = std::move(*out);
        auto err = io::MakePipe();
        REQUIRE(err.has_value());
        Stderr_ = std::move(*err);

        client::SessionTestConfig config{
            .Secrets = DeriveSessionSecrets(MakeKey(agent.KeySeed)),
            .Limits = agent.Limits,
            .Addresses = *addresses,
            .StdoutFd = Stdout_.Write.Get(),
            .StderrFd = Stderr_.Write.Get(),
            .NoticesInline = options.NoticesInline,
            .Ceiling = options.Ceiling,
            .GiveUpAfter = options.GiveUpAfter,
            .MirrorDump = options.MirrorDump,
            .InitialSize = options.InitialSize,
            .SizeSource = std::move(options.SizeSource),
        };
        Result_ = std::async(
            std::launch::async, [config = std::move(config)]() mutable {
                return client::RunSessionForTest(std::move(config));
            });
    }

    ClientRun(const ClientRun&) = delete;
    ClientRun& operator=(const ClientRun&) = delete;
    ClientRun(ClientRun&&) = delete;
    ClientRun& operator=(ClientRun&&) = delete;

    ~ClientRun() {
        Stdout_.Read.Reset();
        Listener_.Socket.Reset();
        for (auto& link : Links_) {
            link->Fd.Reset();
        }
        if (Result_.valid()) {
            Result_.wait();
        }
    }

    /// The next connection the client makes.
    AgentLink& Accept(std::uint16_t ceiling = wire::PROTOCOL_VERSION_GRID) {
        pollfd pfd{
            .fd = Listener_.Socket.Get(), .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&pfd, 1, 3000) > 0);
        auto fd = io::Accept(Listener_.Socket.Get());
        REQUIRE(fd.has_value());
        Links_.push_back(std::make_unique<AgentLink>(std::move(*fd), ceiling));
        return *Links_.back();
    }

    /// The next connection, brought live.
    AgentLink& AcceptLive(std::uint16_t ceiling = wire::PROTOCOL_VERSION_GRID) {
        AgentLink& link = Accept(ceiling);
        REQUIRE(link.ToLive(Agent_.Session, Agent_.Nonces));
        return link;
    }

    [[nodiscard]] bool Exited(std::chrono::milliseconds within) {
        return Result_.wait_for(within) == std::future_status::ready;
    }

    /// The client's result; FAILs, rather than hangs, if it has not exited.
    client::SessionTestResult Result(
        std::chrono::milliseconds within = 5000ms) {
        if (!Exited(within)) {
            FAIL("the client did not exit in time");
        }
        return Result_.get();
    }

    /// Fills the stdout pipe from the client's side, before it writes.
    std::size_t FillStdout() {
        const std::array<std::byte, 4096> zeros{};
        std::size_t total = 0;
        for (;;) {
            auto n = io::WriteSome(Stdout_.Write.Get(), ByteSpan{zeros});
            if (!n || *n == 0) break;
            total += *n;
        }
        return total;
    }

    /// Reads at most `max` bytes of what stdout holds now.
    std::size_t ReadStdout(std::size_t max = SIZE_MAX) {
        std::array<std::byte, 16384> buf{};
        std::size_t total = 0;
        while (total < max) {
            const std::size_t want = std::min(buf.size(), max - total);
            auto n = io::ReadSome(Stdout_.Read.Get(),
                                  MutableByteSpan{buf}.first(want));
            if (!n || *n == 0) break;
            Out.insert(Out.end(), buf.begin(),
                       buf.begin() + static_cast<std::ptrdiff_t>(*n));
            total += *n;
        }
        return total;
    }

    /// Whether the client has written to stdout what is still unread.
    [[nodiscard]] bool StdoutReadable() {
        pollfd pfd{.fd = Stdout_.Read.Get(), .events = POLLIN, .revents = 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

    /// Ticks `link` and reads stdout until `done` or the deadline.
    template <typename TDone>
    [[nodiscard]] bool PumpUntil(AgentLink& link, TDone done,
                                 std::chrono::milliseconds timeout = 5000ms,
                                 std::size_t readPerTick = SIZE_MAX) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            static_cast<void>(link.Tick());
            ReadStdout(readPerTick);
            if (done()) return true;
            ::usleep(200);
        }
        return false;
    }

    /// Everything left on stdout, once the client has exited.
    void FinishStdout() {
        Stdout_.Write.Reset();
        while (ReadStdout() > 0) {
        }
    }

    /// Everything on stderr so far.
    std::string Stderr() {
        std::array<char, 4096> buf{};
        for (;;) {
            const auto n = ::read(Stderr_.Read.Get(), buf.data(), buf.size());
            if (n <= 0) break;
            Err_.append(buf.data(), static_cast<std::size_t>(n));
        }
        return Err_;
    }

    void CloseStdoutReader() { Stdout_.Read.Reset(); }

    std::vector<std::byte> Out;

private:
    AgentSide& Agent_;
    io::Listener Listener_;
    io::Pipe Stdout_;
    io::Pipe Stderr_;
    std::string Err_;
    std::vector<std::unique_ptr<AgentLink>> Links_;
    std::future<client::SessionTestResult> Result_;
};

/// Ends the session with status 0 and returns the client's result.
client::SessionTestResult EndCleanly(ClientRun& run, AgentSide& agent,
                                     AgentLink& link) {
    agent.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
    CHECK(run.PumpUntil(link, [&] { return link.Ac.DeliveredEnd(); }));
    auto result = run.Result();
    run.FinishStdout();
    return result;
}

// ── The mirror in the loop ────────────────────────────────────────────────

constexpr std::uint16_t COLS = 40;
constexpr std::uint16_t ROWS = 10;

std::vector<std::byte> Concat(std::vector<std::byte> a, ByteSpan b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

ByteSpan AsBytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::vector<std::byte> ToVector(ByteSpan bytes) {
    return {bytes.begin(), bytes.end()};
}

/// Scratch under the build tree's temp directory, removed on scope exit.
struct DumpPath {
    DumpPath() {
        static int counter = 0;
        Path =
            std::string{std::getenv("TMPDIR") != nullptr ? std::getenv("TMPDIR")
                                                         : "/tmp"} +
            "/zet_mirror_loop_" + std::to_string(::getpid()) + "_" +
            std::to_string(++counter);
    }
    ~DumpPath() { std::remove(Path.c_str()); }
    DumpPath(const DumpPath&) = delete;
    DumpPath& operator=(const DumpPath&) = delete;

    [[nodiscard]] std::vector<std::byte> Read() const {
        std::ifstream in{Path, std::ios::binary};
        std::vector<char> raw{std::istreambuf_iterator<char>{in},
                              std::istreambuf_iterator<char>{}};
        std::vector<std::byte> out(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            out[i] = static_cast<std::byte>(raw[i]);
        }
        return out;
    }
    [[nodiscard]] bool Exists() const {
        return std::ifstream{Path, std::ios::binary}.good();
    }

    std::string Path;
};

/// What a grid of the terminal's size, with no history, holds after `parts`
/// fed in order: the mirror's expected state.
std::vector<std::byte> ReferenceDump(std::span<const ByteSpan> parts,
                                     std::uint16_t cols = COLS,
                                     std::uint16_t rows = ROWS) {
    auto created = grid::Grid::Create(cols, rows, 0);
    REQUIRE(created.has_value());
    const std::unique_ptr<grid::Grid> g(*created);
    grid::VtParser parser;
    for (const auto part : parts) {
        parser.Feed(part.data(), part.size(), *g);
    }
    auto snap = grid::Snapshot::Create(*g);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    return ToVector(owned->Bytes());
}

/// Output that scrolls the screen, wraps, colours and addresses the cursor.
std::string Stream() {
    std::string s;
    for (int i = 0; i < 30; ++i) {
        s += "row " + std::to_string(i) + " \x1b[3" + std::to_string(i % 8) +
             "mcolour\x1b[0m and enough text to wrap the forty columns\r\n";
    }
    s += "\x1b[2;5Hat\x1b[1;48;5;99mbold\x1b[m\x1b[?25l\x1b[10;3Hend";
    return s;
}

/// Writes `chunks` as the agent's output, one by one, ticking the link so
/// the client sees them as separate reads.
void WriteChunks(ClientRun& run, AgentSide& agent, AgentLink& link,
                 std::span<const std::string> chunks) {
    for (const auto& chunk : chunks) {
        REQUIRE(agent.Session.Write(AsBytes(chunk)).has_value());
        const std::size_t wanted = run.Out.size() + chunk.size();
        REQUIRE(run.PumpUntil(link, [&] { return run.Out.size() >= wanted; }));
    }
}

/// Holds SIGWINCH back from the thread that makes it, and from the client
/// thread that inherits the mask, so that a raised one waits for the client's
/// signalfd instead of being ignored.
struct BlockWinch {
    BlockWinch() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGWINCH);
        ::pthread_sigmask(SIG_BLOCK, &set, &Old);
    }
    ~BlockWinch() { ::pthread_sigmask(SIG_SETMASK, &Old, nullptr); }
    BlockWinch(const BlockWinch&) = delete;
    BlockWinch& operator=(const BlockWinch&) = delete;

    sigset_t Old{};
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "the mirror equals the grid of the same stream and stdout "
                  "is the stream, byte for byte") {
    AgentSide agent{71, SessionLimits{}, false};
    DumpPath dump;
    ClientRun run{agent,
                  {.MirrorDump = dump.Path,
                   .InitialSize = wire::ResizeMessage{
                       .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0}}};
    AgentLink& link = run.AcceptLive();

    const std::string stream = Stream();
    // Cut mid-escape and mid-character so the reads do not line up with the
    // sequences.
    const std::string_view whole{stream};
    const std::array<std::string, 3> chunks{
        std::string{whole.substr(0, 61)},
        std::string{whole.substr(61, 333)},
        std::string{whole.substr(394)},
    };
    WriteChunks(run, agent, link, chunks);
    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == ToVector(AsBytes(stream)));

    const std::array<ByteSpan, 1> parts{AsBytes(stream)};
    REQUIRE(dump.Exists());
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture, "without a consumer there is no mirror") {
    AgentSide agent{72, SessionLimits{}, false};
    DumpPath dump;
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const std::array<std::string, 1> chunks{Stream()};
    WriteChunks(run, agent, link, chunks);
    CHECK(EndCleanly(run, agent, link).Status == 0);
    CHECK(run.Out == ToVector(AsBytes(chunks[0])));
    CHECK_FALSE(dump.Exists());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "after a path-B snapshot the mirror is the snapshot fed to "
                  "a fresh grid, plus what follows") {
    AgentSide agent{73, TinyLimits(), true};
    DumpPath dump;
    ClientRun run{agent,
                  {.MirrorDump = dump.Path,
                   .InitialSize = wire::ResizeMessage{
                       .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0}}};
    AgentLink& link = run.AcceptLive();

    // The agent's grid, with history, at the terminal's size.
    auto created = grid::Grid::Create(COLS, ROWS, 1000);
    REQUIRE(created.has_value());
    const std::unique_ptr<grid::Grid> agentGrid(*created);
    grid::VtParser agentParser;
    const std::string missed = Stream();
    agentParser.Feed(AsBytes(missed).data(), missed.size(), *agentGrid);
    auto snap = grid::Snapshot::Create(*agentGrid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    const auto snapBytes = ToVector(owned->Bytes());

    REQUIRE(link.Supply(ByteSpan{snapBytes}));
    REQUIRE(run.PumpUntil(link,
                          [&] { return run.Out.size() >= snapBytes.size(); }));
    const std::array<std::string, 1> after{"\x1b[3;3Hafter\x1b[1;31m!\r\nmore"};
    WriteChunks(run, agent, link, after);

    CHECK(EndCleanly(run, agent, link).Status == 0);
    CHECK(run.Out == Concat(snapBytes, AsBytes(after[0])));

    const std::array<ByteSpan, 2> parts{ByteSpan{snapBytes}, AsBytes(after[0])};
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a notice written inline to the terminal reaches the mirror") {
    AgentSide agent{74, TinyLimits(), true};
    DumpPath dump;
    ClientRun run{
        agent,
        {.MirrorDump = dump.Path,
         .InitialSize =
             wire::ResizeMessage{
                 .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0},
         .NoticesInline = true}};
    AgentLink& link = run.AcceptLive();
    const std::string snap = "\x1b[2Jsnapshot";
    REQUIRE(link.Supply(AsBytes(snap)));
    const std::array<std::string, 1> after{"after"};
    // Data is held until the snapshot is confirmed, which is when the
    // notice is queued: "after" comes behind it.
    WriteChunks(run, agent, link, after);
    CHECK(EndCleanly(run, agent, link).Status == 0);

    const std::string out{reinterpret_cast<const char*>(run.Out.data()),
                          run.Out.size()};
    REQUIRE(out.find("zet: screen restored") != std::string::npos);
    REQUIRE(out.starts_with(snap));
    REQUIRE(out.ends_with("after"));
    const std::array<ByteSpan, 1> parts{ByteSpan{run.Out}};
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a second snapshot replaces the mirror, it does not add to "
                  "what the first left") {
    AgentSide agent{75, TinyLimits(), true};
    DumpPath dump;
    ClientRun run{agent,
                  {.MirrorDump = dump.Path,
                   .InitialSize = wire::ResizeMessage{
                       .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0}}};
    // What a snapshot's repaint does not unset: the alternate screen's
    // content, saved cursors, tab stops, a cursor style, modes.
    const std::string stale =
        "\x1b[3;4r\x1b[?6h\x1b[2;2H\x1b"
        "7\x1b[4;4Hmain\x1b[?1049h\x1b[5;5Halt content\x1b[3;3H\x1b"
        "7\x1b[?1049l\x1b[3g\x1b[1;6H\x1bH\x1b[5 q\x1b[?1000h\x1b[?7l\x1b[4h"
        "\x1b(0\x1b[?1004h";
    AgentLink& first = run.AcceptLive();
    REQUIRE(first.Supply(AsBytes(stale)));
    // GridEnd has gone out once the client has the bytes; GridApplied is
    // what ends the agent's wait.
    REQUIRE(run.PumpUntil(first, [&] {
        return run.Out.size() >= stale.size() && !first.Ac.GridEmissionDone();
    }));
    first.Cut();
    // Output the ring cannot hold while nobody is connected forces path B
    // again.
    const std::vector<std::byte> fill(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = agent.Session.Write(ByteSpan{fill});
    }

    auto created = grid::Grid::Create(COLS, ROWS, 1000);
    REQUIRE(created.has_value());
    const std::unique_ptr<grid::Grid> agentGrid(*created);
    grid::VtParser agentParser;
    const std::string current = Stream();
    agentParser.Feed(AsBytes(current).data(), current.size(), *agentGrid);
    auto snap = grid::Snapshot::Create(*agentGrid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    const auto second = ToVector(owned->Bytes());

    AgentLink& link = run.AcceptLive();
    REQUIRE(link.Supply(ByteSpan{second}));
    REQUIRE(run.PumpUntil(
        link, [&] { return run.Out.size() >= stale.size() + second.size(); }));
    CHECK(EndCleanly(run, agent, link).Status == 0);
    CHECK(run.Out == Concat(ToVector(AsBytes(stale)), ByteSpan{second}));

    const std::array<ByteSpan, 1> parts{ByteSpan{second}};
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a terminal resize resizes the mirror to the new size") {
    constexpr std::uint16_t newCols = 20;
    constexpr std::uint16_t newRows = 6;
    const BlockWinch block;
    AgentSide agent{76, SessionLimits{}, false};
    DumpPath dump;
    std::atomic<bool> asked{false};
    ClientRun run{
        agent,
        {.MirrorDump = dump.Path,
         .InitialSize =
             wire::ResizeMessage{
                 .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0},
         .SizeSource = [&asked] {
             asked = true;
             return std::optional{wire::ResizeMessage{
                 .Rows = newRows, .Cols = newCols, .XPixel = 0, .YPixel = 0}};
         }}};
    AgentLink& link = run.AcceptLive();

    // kill, not raise: raise targets this thread, which blocks the signal.
    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(run.PumpUntil(link, [&] { return asked.load(); }));
    // The client is single-threaded: what it delivers from here on comes
    // after the resize.
    const std::array<std::string, 1> chunks{Stream()};
    WriteChunks(run, agent, link, chunks);
    CHECK(EndCleanly(run, agent, link).Status == 0);

    const std::array<ByteSpan, 1> parts{AsBytes(chunks[0])};
    CHECK(dump.Read() == ReferenceDump(parts, newCols, newRows));
    CHECK(dump.Read() != ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a snapshot flagged as diverged mirrors both notices") {
    AgentSide agent{77, TinyLimits(), true};
    DumpPath dump;
    ClientRun run{
        agent,
        {.MirrorDump = dump.Path,
         .InitialSize =
             wire::ResizeMessage{
                 .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0},
         .NoticesInline = true}};
    AgentLink& link = run.AcceptLive();
    const std::string snap = "\x1b[2Jsnapshot";
    constexpr std::uint8_t possiblyDiverged = 0x01;
    REQUIRE(link.Supply(AsBytes(snap), possiblyDiverged));
    const std::array<std::string, 1> after{"after"};
    WriteChunks(run, agent, link, after);
    CHECK(EndCleanly(run, agent, link).Status == 0);

    const std::string out{reinterpret_cast<const char*>(run.Out.data()),
                          run.Out.size()};
    REQUIRE(out.find("zet: screen restored") != std::string::npos);
    REQUIRE(out.find("the restored screen may differ") != std::string::npos);
    const std::array<ByteSpan, 1> parts{ByteSpan{run.Out}};
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the notices for a lost and a regained link reach the "
                  "mirror") {
    AgentSide agent{78, SessionLimits{}, false};
    DumpPath dump;
    ClientRun run{
        agent,
        {.GiveUpAfter = 5s,
         .MirrorDump = dump.Path,
         .InitialSize =
             wire::ResizeMessage{
                 .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0},
         .NoticesInline = true}};
    AgentLink& first = run.AcceptLive();
    const std::array<std::string, 1> before{"before"};
    WriteChunks(run, agent, first, before);
    first.Cut();

    AgentLink& link = run.AcceptLive();
    const std::string regained = "zet: reconnected";
    const auto seen = [&](const std::string& text) {
        const std::string out{reinterpret_cast<const char*>(run.Out.data()),
                              run.Out.size()};
        return out.find(text) != std::string::npos;
    };
    REQUIRE(run.PumpUntil(link, [&] { return seen(regained); }));
    const std::array<std::string, 1> after{"after"};
    WriteChunks(run, agent, link, after);
    CHECK(EndCleanly(run, agent, link).Status == 0);

    REQUIRE(seen("zet: connection lost"));
    const std::array<ByteSpan, 1> parts{ByteSpan{run.Out}};
    CHECK(dump.Read() == ReferenceDump(parts));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output delivered a single byte at a time reaches the "
                  "mirror") {
    AgentSide agent{79, SessionLimits{}, false};
    DumpPath dump;
    ClientRun run{agent,
                  {.MirrorDump = dump.Path,
                   .InitialSize = wire::ResizeMessage{
                       .Rows = ROWS, .Cols = COLS, .XPixel = 0, .YPixel = 0}}};
    AgentLink& link = run.AcceptLive();

    const std::string stream = "ab\x1b[3;4Hcd\x1b[1mef";
    std::vector<std::string> chunks;
    for (const char c : stream) chunks.emplace_back(1, c);
    WriteChunks(run, agent, link, chunks);
    CHECK(EndCleanly(run, agent, link).Status == 0);
    CHECK(run.Out == ToVector(AsBytes(stream)));

    const std::array<ByteSpan, 1> parts{AsBytes(stream)};
    CHECK(dump.Read() == ReferenceDump(parts));
}
