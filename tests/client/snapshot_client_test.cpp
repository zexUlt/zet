/// The client's side of grid snapshots (§9 path B, ADR-035), driven end to
/// end: the real Client connects over loopback TCP to a listener the test
/// owns, and the test plays the agent through AgentConnection.

#include "client.hpp"
#include "pump_test_api.hpp"

#include <doctest/doctest.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
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

// Match client.cpp.
constexpr int OVERFLOW_STATUS = 70;
constexpr int FAILURE_STATUS = 255;
constexpr std::size_t STDOUT_QUEUE_MAX = 1024 * 1024;
constexpr std::string_view RESTORE_NOTICE = "zet: screen restored";
constexpr std::string_view DIVERGED_NOTICE =
    "zet: warning: the restored screen";

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

std::vector<std::byte> Pattern(std::size_t size, std::uint8_t salt) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31 + salt) & 0xFF);
    }
    return bytes;
}

std::vector<std::byte> Concat(std::vector<std::byte> a, ByteSpan b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::size_t CountOf(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
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
            .NoticesInline = false,
            .Ceiling = options.Ceiling,
            .GiveUpAfter = options.GiveUpAfter,
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

}  // namespace

// ── TryWriteSnapshot ──────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot writes bytes to a pipe in order") {
    const auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());
    const auto payload = Pattern(64, 3);
    REQUIRE(client::TryWriteSnapshot(pipe->Write.Get(), ByteSpan{payload},
                                     io::MonotonicNow() + 5s));
    std::vector<std::byte> received(payload.size());
    const auto n = ::read(pipe->Read.Get(), received.data(), received.size());
    REQUIRE(n == static_cast<ssize_t>(payload.size()));
    CHECK(received == payload);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot returns false on write error") {
    auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());
    pipe->Read.Reset();
    const std::vector<std::byte> payload(32, std::byte{0xAB});
    CHECK_FALSE(client::TryWriteSnapshot(pipe->Write.Get(), ByteSpan{payload},
                                         io::MonotonicNow() + 5s));
}

// ── Restoring a snapshot ──────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "a snapshot reaches stdout and the client exits cleanly") {
    AgentSide agent{41, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(64, 41);
    REQUIRE(link.Supply(ByteSpan{snap}));

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(result.SnapshotBufferBytes == wire::SNAPSHOT_MAX_BYTES);
    CHECK(run.Out == snap);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a PossiblyDiverged snapshot warns on stderr, not on stdout") {
    AgentSide agent{43, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(64, 43);
    REQUIRE(link.Supply(ByteSpan{snap}, 1));

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == snap);
    const auto err = run.Stderr();
    CHECK(CountOf(err, RESTORE_NOTICE) == 1);
    CHECK(CountOf(err, DIVERGED_NOTICE) == 1);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a snapshot without PossiblyDiverged does not warn") {
    AgentSide agent{44, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(64, 44);
    REQUIRE(link.Supply(ByteSpan{snap}, 0));

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    const auto err = run.Stderr();
    CHECK(CountOf(err, RESTORE_NOTICE) == 1);
    CHECK(CountOf(err, DIVERGED_NOTICE) == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a v1 session never allocates the snapshot buffer") {
    AgentSide agent{60, TinyLimits(), false};
    ClientRun run{agent, {.Ceiling = wire::PROTOCOL_VERSION_MIN}};
    AgentLink& link = run.AcceptLive(wire::PROTOCOL_VERSION_MIN);

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(result.SnapshotBufferBytes == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "ConfirmApply waits until stdout has taken the snapshot") {
    // Agent Data is held while the grid awaits GridApplied, and SessionEnd
    // waits for every byte to be acknowledged.  With stdout full the snapshot
    // cannot drain, so ConfirmApply, and with it SessionEnd, must wait.
    AgentSide agent{42, TinyLimits(), true};
    ClientRun run{agent};
    const std::size_t prefilled = run.FillStdout();
    REQUIRE(prefilled > 0);
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(64, 42);
    const std::vector<std::byte> tail(1, std::byte{0x7A});
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(agent.Session.Write(ByteSpan{tail}).has_value());
    agent.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});

    CHECK_FALSE(
        run.PumpUntil(link, [&] { return link.Ac.DeliveredEnd(); }, 300ms, 0));
    CHECK(run.PumpUntil(link, [&] { return link.Ac.DeliveredEnd(); }));
    CHECK(run.Result().Status == 0);
    run.FinishStdout();
    const std::vector<std::byte> zeros(prefilled, std::byte{0});
    CHECK(run.Out == Concat(Concat(zeros, ByteSpan{snap}), ByteSpan{tail}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a 600 KiB snapshot through a slow reader is confirmed only "
                  "after its last byte") {
    AgentSide agent{59, TinyLimits(), true};
    ClientRun run{agent, {.GiveUpAfter = 2s}};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(600 * 1024, 59);
    const std::vector<std::byte> tail(1, std::byte{0x7A});
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(agent.Session.Write(ByteSpan{tail}).has_value());
    agent.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});

    // Take a little: the client is mid-snapshot, with partial writes and
    // EAGAIN behind it, and must neither confirm nor fail.
    CHECK(run.PumpUntil(
        link, [&] { return run.Out.size() >= 64 * 1024; }, 3000ms, 4096));
    CHECK_FALSE(
        run.PumpUntil(link, [&] { return link.Ac.DeliveredEnd(); }, 400ms, 0));
    CHECK(run.Out.size() < snap.size());
    CHECK_FALSE(run.Exited(0ms));

    CHECK(run.PumpUntil(
        link, [&] { return link.Ac.DeliveredEnd(); }, 6000ms, 16384));
    CHECK(run.Result().Status == 0);
    run.FinishStdout();
    CHECK(run.Out == Concat(snap, ByteSpan{tail}));
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 1);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a 2 MiB snapshot, larger than the stdout queue, is "
                  "restored whole") {
    AgentSide agent{58, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(2 * 1024 * 1024, 58);
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(run.PumpUntil(
        link, [&] { return run.Out.size() == snap.size(); }, 8000ms));

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == snap);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a snapshot of exactly SNAPSHOT_MAX_BYTES through a slow "
                  "reader is restored whole") {
    AgentSide agent{43, TinyLimits(), true};
    ClientRun run{agent, {.GiveUpAfter = 2s}};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(wire::SNAPSHOT_MAX_BYTES, 43);
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(run.PumpUntil(
        link, [&] { return run.Out.size() == snap.size(); }, 20000ms, 8192));

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == snap);
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 1);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the restore notice appears once, not again for output "
                  "after the snapshot") {
    AgentSide agent{44, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(32, 44);
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(run.PumpUntil(link, [&] { return run.Out.size() == snap.size(); }));

    // Output after the confirm: every drain passes the confirm check again.
    std::vector<std::byte> more;
    for (std::uint8_t i = 0; i < 4; ++i) {
        const auto chunk = Pattern(100, i);
        REQUIRE(agent.Session.Write(ByteSpan{chunk}).has_value());
        more = Concat(more, ByteSpan{chunk});
        REQUIRE(run.PumpUntil(
            link, [&] { return run.Out.size() == snap.size() + more.size(); }));
    }

    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == Concat(snap, ByteSpan{more}));
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 1);
}

// ── Reconnecting around a snapshot ────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "a snapshot left unconfirmed by a dropped connection prints "
                  "no restore notice") {
    AgentSide agent{61, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& first = run.AcceptLive();
    const auto s1 = Pattern(600 * 1024, 61);
    REQUIRE(first.Supply(ByteSpan{s1}));
    // The client has the snapshot once it starts writing it; stdout is not
    // read, so most of it is still queued when the link goes.
    REQUIRE(
        run.PumpUntil(first, [&] { return run.StdoutReadable(); }, 5000ms, 0));
    first.Cut();

    AgentLink& second = run.AcceptLive();
    REQUIRE(second.UntilNeedsSnapshot());
    // Drain the first snapshot while the second connection waits for its own.
    REQUIRE(run.PumpUntil(second, [&] { return run.Out.size() == s1.size(); }));
    ::usleep(50'000);
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 0);

    const auto s2 = Pattern(4096, 62);
    second.Ac.SupplySnapshot(io::MonotonicNow(), ByteSpan{s2}, 0, 0);
    REQUIRE(run.PumpUntil(
        second, [&] { return run.Out.size() == s1.size() + s2.size(); }));

    const auto result = EndCleanly(run, agent, second);
    CHECK(result.Status == 0);
    CHECK(run.Out == Concat(s1, ByteSpan{s2}));
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 1);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output queued before a snapshot reaches stdout ahead of "
                  "it") {
    AgentSide agent{63, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& first = run.AcceptLive();
    const auto s1 = Pattern(600 * 1024, 63);
    REQUIRE(first.Supply(ByteSpan{s1}));
    REQUIRE(
        run.PumpUntil(first, [&] { return run.StdoutReadable(); }, 5000ms, 0));
    first.Cut();

    // The second snapshot lands while most of the first is still queued.
    AgentLink& second = run.AcceptLive();
    const auto s2 = Pattern(64 * 1024, 64);
    REQUIRE(second.Supply(ByteSpan{s2}));
    REQUIRE(run.PumpUntil(
        second, [&] { return run.Out.size() == s1.size() + s2.size(); }));

    const auto result = EndCleanly(run, agent, second);
    CHECK(result.Status == 0);
    CHECK(run.Out == Concat(s1, ByteSpan{s2}));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a connection cut mid-snapshot puts nothing on stdout") {
    AgentSide agent{65, TinyLimits(), true};
    ClientRun run{agent, {.GiveUpAfter = 500ms}};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(600 * 1024, 65);
    REQUIRE(link.UntilNeedsSnapshot());
    link.Ac.SupplySnapshot(io::MonotonicNow(), ByteSpan{snap}, 0, 0);
    // GridBegin and one chunk, then the link goes.
    static_cast<void>(link.Tick(2));
    link.Cut();

    CHECK(run.Result().Status == FAILURE_STATUS);
    run.FinishStdout();
    CHECK(run.Out.empty());
    CHECK(CountOf(run.Stderr(), RESTORE_NOTICE) == 0);
}

// ── Stdout ────────────────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture, "a stdout write error ends the client") {
    AgentSide agent{66, TinyLimits(), true};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    const auto snap = Pattern(600 * 1024, 66);
    REQUIRE(link.Supply(ByteSpan{snap}));
    REQUIRE(
        run.PumpUntil(link, [&] { return run.StdoutReadable(); }, 5000ms, 0));
    run.CloseStdoutReader();

    // The link stays up: only the broken terminal can end the client.
    const auto until = Clock::now() + 3s;
    while (!run.Exited(1ms) && Clock::now() < until) {
        static_cast<void>(link.Tick());
    }
    CHECK(run.Result(0ms).Status == FAILURE_STATUS);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "output past the stdout queue waits in the socket behind a "
                  "slow reader") {
    const SessionLimits limits{
        .ResendMax = 64 * 1024,
        .DisconnectMax = 64 * 1024,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = Duration{0},
                                   .PeerSilenceTimeout = Duration{0}},
    };
    AgentSide agent{67, limits, false};
    ClientRun run{agent, {.GiveUpAfter = 2s}};
    AgentLink& link = run.AcceptLive();
    const auto payload = Pattern(3 * STDOUT_QUEUE_MAX, 67);

    std::size_t written = 0;
    const auto writeSome = [&] {
        while (written < payload.size() && agent.Session.AcceptsWrites()) {
            const auto chunk = ByteSpan{payload}.subspan(written).first(
                std::min<std::size_t>(16384, payload.size() - written));
            const auto n = agent.Session.Write(chunk);
            REQUIRE(n.has_value());
            if (*n == 0) break;
            written += *n;
        }
    };
    // Nothing is read until the client stops taking output: past its queue
    // it must leave the rest in the socket.
    auto lastProgress = Clock::now();
    const auto until = Clock::now() + 10s;
    while (Clock::now() < until && Clock::now() - lastProgress < 300ms) {
        const std::size_t before = written;
        writeSome();
        static_cast<void>(link.Tick());
        if (written != before) lastProgress = Clock::now();
        ::usleep(200);
    }
    // The client took output until its queue neared full, then stopped.
    CHECK(written > STDOUT_QUEUE_MAX / 2);
    CHECK(written < payload.size());

    CHECK(run.PumpUntil(
        link,
        [&] {
            writeSome();
            return run.Out.size() == payload.size();
        },
        15000ms));
    const auto result = EndCleanly(run, agent, link);
    CHECK(result.Status == 0);
    CHECK(run.Out == payload);
}

// ── Session end ───────────────────────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "SessionEnd{Overflow} exits with OVERFLOW_STATUS") {
    AgentSide agent{45, TinyLimits(), false};
    ClientRun run{agent};
    AgentLink& link = run.AcceptLive();
    agent.Session.End({.Kind = wire::ESessionEndKind::Overflow, .Value = 0});
    CHECK(run.PumpUntil(link, [&] { return link.Ac.DeliveredEnd(); }));
    CHECK(run.Result().Status == OVERFLOW_STATUS);
}
