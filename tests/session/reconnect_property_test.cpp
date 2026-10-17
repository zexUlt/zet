// The recovery property of docs/design.md §16: cut the transport at any byte
// and, after the reconnect, the stream on the receiving side is byte for byte
// the one that was sent. Two layers, as §16 lays them out:
//
//   short   every byte offset, in both directions: 16 KiB on every pull
//           request, 64 KiB nightly (ZET_CUT_SCENARIO_BYTES=65536)
//   10 MB   one run cut on every reconnect — a byte short of a frame boundary,
//           on it, a byte past it, or at a seeded offset, in turn — and the
//           byte either side of RESEND_BUFFER_MAX and DISCONNECT_BUFFER_MAX
//
// Deterministic throughout: virtual time, nonces from a counter, the seed
// printed on failure.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "zet/session/connection.hpp"
#include "zet/wire/key_schedule.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// The short scenario on every run of the suite. Every offset costs a full
/// run of its own, so the time grows with the square of the size: 16 KiB is
/// eighteen seconds in a debug build, 64 KiB five minutes.
constexpr std::size_t SHORT_SCENARIO = 16 * 1024;

/// The short scenario of §16, run nightly through ZET_CUT_SCENARIO_BYTES.
constexpr std::size_t FULL_SHORT_SCENARIO = 64 * 1024;

/// The long scenario of §16.
constexpr std::size_t LONG_SCENARIO = 10 * 1000 * 1000;

/// Each kind of cut must come round at least this often in the long scenario
/// for it to say anything about that kind.
constexpr std::size_t MIN_CUTS_PER_KIND = 100;

constexpr std::uint64_t SEED = 0x5EED'2024'C0FF'EE00;

/// How much the local writer offers per step: a pty read, roughly.
constexpr std::size_t WRITE_CHUNK = 4096;

/// A run that makes no progress for this many quiet rounds is stuck.
constexpr int MAX_QUIET_ROUNDS = 64;

/// Steps one connection may take before it is considered spinning.
constexpr int MAX_STEPS_PER_CONNECTION = 1'000'000;

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t value = seed;
    for (auto& byte : bytes) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 5);
    }
    return crypto::Key{bytes};
}

HandshakeNonce NonceFrom(std::uint64_t counter) {
    HandshakeNonce nonce{};
    std::uint64_t value = counter * 0x9E37'79B9'7F4A'7C15ULL + 1;
    for (auto& byte : nonce) {
        byte = static_cast<std::byte>(value & 0xFFU);
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return nonce;
}

std::vector<std::byte> Script(std::size_t size, std::uint64_t seed) {
    std::vector<std::byte> bytes(size);
    std::mt19937_64 random{seed};
    for (auto& byte : bytes) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    return bytes;
}

std::size_t ShortScenario() {
    const char* raw = std::getenv("ZET_CUT_SCENARIO_BYTES");
    if (raw == nullptr) {
        return SHORT_SCENARIO;
    }
    const std::string_view text{raw};
    std::size_t size = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), size);
    if (error != std::errc{} || end != text.data() + text.size()) {
        FAIL("ZET_CUT_SCENARIO_BYTES is not a byte count: " << text);
    }
    return std::min(size, FULL_SHORT_SCENARIO);
}

/// The script of the ending scenario: a few frames, so that cutting at
/// every offset of it and of the SessionEnd after it stays cheap.
constexpr std::size_t ENDING_SCENARIO = 4 * 1024;

enum class EDirection : std::uint8_t { AgentToClient, ClientToAgent };

/// One frame about to cross the cut direction of the link.
struct FrameAt {
    std::size_t Connection{0};
    /// Frames this connection has already carried in the cut direction.
    std::size_t Index{0};
    std::size_t Size{0};
    /// Bytes this connection has already carried in the cut direction.
    std::uint64_t Carried{0};
};

/// Asked for every frame of the cut direction: how many of its bytes get
/// through before the connection dies, or nullopt to carry it whole and live.
/// Deciding frame by frame, on the run as it goes, is what lets a cut land on
/// a boundary of the stream as it is after earlier resends moved it.
using CutPlan = std::function<std::optional<std::size_t>(const FrameAt&)>;

struct Trace {
    /// Wire bytes the cut direction carried in each connection.
    std::vector<std::uint64_t> CarriedPerConnection;
    std::size_t Connections{0};
};

/// The connection-sized buffers, a megabyte together. Shared by every run of
/// a test: tens of thousands of runs each zeroing its own made the short
/// scenario take minutes, all of it in memset.
struct Workspace {
    std::vector<std::byte> AgentFrames =
        std::vector<std::byte>(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> ClientFrames =
        std::vector<std::byte>(CONNECTION_FRAME_STORAGE);
    std::vector<std::byte> Out = std::vector<std::byte>(CONNECTION_OUT_SIZE);
    std::vector<std::byte> Scratch =
        std::vector<std::byte>(CONNECTION_SCRATCH_SIZE);
};

Workspace& Shared() {
    static Workspace workspace;
    return workspace;
}

/// Stream storage for a scenario: the ceiling the session will be held to,
/// but never more than the script needs, plus a byte so the full stream fits.
std::size_t StorageFor(std::size_t script, const SessionLimits& limits) {
    return std::min(limits.DisconnectMax, script + 1);
}

/// Whether the agent's shell ends once the script is written: the session
/// then runs until the client hears SessionEnd, not just until the bytes are
/// in.
enum class EEnding : std::uint8_t { Open, ShellExits };

/// What the shell exits with in the ending scenarios.
constexpr wire::SessionEndMessage SHELL_EXIT{
    .Kind = wire::ESessionEndKind::Exited, .Value = 3};

/// Runs one scenario: `script` written by the local side of `from`, cut by
/// `plan`, reconnecting until all of it has arrived. Returns what arrived.
class Rig {
public:
    Rig(EDirection direction, std::vector<std::byte> script, CutPlan plan,
        SessionLimits limits, EEnding ending = EEnding::Open)
        : Direction_(direction),
          Ending_(ending),
          Script_(std::move(script)),
          Plan_(std::move(plan)),
          AgentStorage_(StorageFor(Script_.size(), limits)),
          ClientStorage_(StorageFor(Script_.size(), limits)),
          Agent_(DeriveSessionSecrets(MakeKey(1)),
                 SessionStreams{.Storage = MutableByteSpan{AgentStorage_},
                                .Now = START,
                                .Limits = limits}),
          Client_(DeriveSessionSecrets(MakeKey(1)),
                  SessionStreams{.Storage = MutableByteSpan{ClientStorage_},
                                 .Now = START,
                                 .Limits = limits}) {}

    std::vector<std::byte> Run() {
        while (!Done()) {
            RunConnection();
            Now_ += 1s;
            REQUIRE_MESSAGE(Agent_.State() != ESessionState::Reaped,
                            Describe(*Agent_.ReapReason()));
        }
        return Received_;
    }

    /// Runs connections until one dies, without finishing the stream.
    void RunUntilCut() { RunConnection(); }

    /// The local writer goes on while nobody is connected, as a shell does:
    /// everything of the script not yet written, in one go.
    [[nodiscard]] SessionResult<std::size_t> WriteRestDetached() {
        REQUIRE(Direction_ == EDirection::AgentToClient);
        auto taken = Agent_.Write(ByteSpan{Script_}.subspan(Written_));
        if (taken) {
            Written_ += *taken;
        }
        return taken;
    }

    [[nodiscard]] std::size_t Unwritten() const {
        return Script_.size() - Written_;
    }

    [[nodiscard]] const AgentSession& Agent() const { return Agent_; }

    [[nodiscard]] const Trace& Seen() const { return Trace_; }

    /// How many SessionEnds the client heard, over all connections.
    [[nodiscard]] std::size_t EndsHeard() const { return EndsHeard_; }

private:
    [[nodiscard]] bool Done() const {
        if (Ending_ == EEnding::ShellExits) {
            return Client_.EndedWith().has_value();
        }
        return Received_.size() == Script_.size();
    }

    void RunConnection() {
        const std::size_t index = Trace_.Connections++;
        std::uint64_t carried = 0;
        std::size_t frames = 0;

        AgentConnection agent{MutableByteSpan{Space_.AgentFrames}, Now_};
        ClientConnection client{Client_, NonceFrom(++Nonces_),
                                MutableByteSpan{Space_.ClientFrames}, Now_};
        int quiet = 0;
        for (int step = 0; step < MAX_STEPS_PER_CONNECTION; ++step) {
            FeedLocalWriter();
            bool busy = false;
            bool dead = false;

            // Agent side.
            auto fromAgent = agent.Pump(Now_, MutableByteSpan{Space_.Out},
                                        MutableByteSpan{Space_.Scratch});
            REQUIRE_MESSAGE(fromAgent.has_value(),
                            "agent: " << fromAgent.error().Description());
            if (const auto* need = std::get_if<NeedsSession>(&*fromAgent)) {
                REQUIRE(need->Id == Agent_.Ref().Id());
                agent.Route(Agent_.Ref(), NonceFrom(++Nonces_));
                busy = true;
            } else if (const auto* send = std::get_if<SendFrame>(&*fromAgent)) {
                busy = true;
                dead = Carry(send->Written, EDirection::AgentToClient, index,
                             frames, carried, client);
            } else if (const auto* got = std::get_if<Deliver>(&*fromAgent)) {
                busy = true;
                Take(got->Bytes, EDirection::ClientToAgent);
            }

            // Client side.
            if (!dead) {
                auto fromClient = client.Pump(Now_, MutableByteSpan{Space_.Out},
                                              MutableByteSpan{Space_.Scratch});
                REQUIRE_MESSAGE(fromClient.has_value(),
                                "client: " << fromClient.error().Description());
                if (const auto* send = std::get_if<SendFrame>(&*fromClient)) {
                    busy = true;
                    dead = Carry(send->Written, EDirection::ClientToAgent,
                                 index, frames, carried, agent);
                } else if (const auto* got =
                               std::get_if<Deliver>(&*fromClient)) {
                    busy = true;
                    Take(got->Bytes, EDirection::AgentToClient);
                } else if (const auto* ended =
                               std::get_if<Ended>(&*fromClient)) {
                    // §9: never ahead of the shell's last byte.
                    REQUIRE(Ending_ == EEnding::ShellExits);
                    REQUIRE(Received_.size() == Script_.size());
                    REQUIRE(ended->End == SHELL_EXIT);
                    ++EndsHeard_;
                    busy = true;
                }
            }

            if (dead) {
                agent.Close(Now_);
                client.Close(Now_);
                Trace_.CarriedPerConnection.push_back(carried);
                return;
            }
            if (Done()) {
                Trace_.CarriedPerConnection.push_back(carried);
                agent.Close(Now_);
                client.Close(Now_);
                return;
            }
            if (busy) {
                quiet = 0;
                continue;
            }
            // Quiet: let the idle Ack go out so the sender's buffer drains,
            // which is what lets the writer past the resend ceiling.
            Now_ += ACK_IDLE_DELAY;
            REQUIRE_MESSAGE(++quiet < MAX_QUIET_ROUNDS,
                            "no progress at " << Received_.size() << " of "
                                              << Script_.size());
        }
        FAIL("a connection never went quiet");
    }

    /// Hands what the local writer has left to its session, as far as the
    /// session takes it.
    void FeedLocalWriter() {
        if (Written_ == Script_.size()) {
            if (Ending_ == EEnding::ShellExits) {
                Agent_.End(SHELL_EXIT);
            }
            return;
        }
        const std::size_t size =
            std::min(WRITE_CHUNK, Script_.size() - Written_);
        const auto chunk = ByteSpan{Script_}.subspan(Written_, size);
        const auto taken = Direction_ == EDirection::AgentToClient
                               ? Agent_.Write(chunk)
                               : Client_.Write(chunk);
        REQUIRE_MESSAGE(taken.has_value(), Describe(taken.error()));
        Written_ += *taken;
    }

    /// Carries one frame towards `to`, or the part of it the cut lets
    /// through. Returns whether the connection died on it.
    template <typename TConnection>
    bool Carry(std::size_t size, EDirection direction, std::size_t connection,
               std::size_t& frames, std::uint64_t& carried, TConnection& to) {
        std::size_t pass = size;
        bool dies = false;
        if (direction == Direction_) {
            const auto cut = Plan_(FrameAt{.Connection = connection,
                                           .Index = frames,
                                           .Size = size,
                                           .Carried = carried});
            if (cut) {
                pass = std::min(*cut, size);
                dies = true;
            }
            carried += pass;
            ++frames;
        }
        if (pass > 0) {
            REQUIRE(to.OnBytes(ByteSpan{Space_.Out}.first(pass)));
        }
        return dies;
    }

    void Take(ByteSpan bytes, EDirection direction) {
        CAPTURE(Received_.size());
        REQUIRE(direction == Direction_);
        Received_.insert(Received_.end(), bytes.begin(), bytes.end());
        // Any byte that differs fails here rather than at the end, so the
        // failure names the first wrong offset.
        REQUIRE(Received_.size() <= Script_.size());
        const auto from = Received_.size() - bytes.size();
        REQUIRE_MESSAGE(
            std::equal(bytes.begin(), bytes.end(),
                       Script_.begin() + static_cast<std::ptrdiff_t>(from)),
            "wrong bytes at offset " << from);
    }

    EDirection Direction_;
    EEnding Ending_;
    std::size_t EndsHeard_{0};
    std::vector<std::byte> Script_;
    CutPlan Plan_;
    std::vector<std::byte> AgentStorage_;
    std::vector<std::byte> ClientStorage_;
    AgentSession Agent_;
    ClientSession Client_;
    Workspace& Space_{Shared()};
    std::vector<std::byte> Received_;
    std::size_t Written_{0};
    std::uint64_t Nonces_{0};
    Timestamp Now_{START};
    Trace Trace_;
};

/// The first connection dies once it has carried `offset` bytes of the cut
/// direction; every later one lives.
CutPlan CutOnceAt(std::uint64_t offset) {
    return [offset](const FrameAt& frame) -> std::optional<std::size_t> {
        if (frame.Connection != 0 || frame.Carried + frame.Size < offset) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(offset - frame.Carried);
    };
}

CutPlan NeverCut() {
    return [](const FrameAt&) -> std::optional<std::size_t> {
        return std::nullopt;
    };
}

/// How a connection of the long scenario dies. Rotated by connection index.
enum class EBoundaryCut : std::uint8_t {
    /// One byte short of the end of a frame.
    Before,
    /// Exactly at the end of a frame.
    At,
    /// One byte into the frame after it.
    After,
    /// Anywhere in the first stretch of the connection, from the seed.
    Sampled,
};
constexpr std::size_t BOUNDARY_CUT_KINDS = 4;

/// Frames a connection of the long scenario carries whole before its cut:
/// one to three, so that cuts land deep into a connection as well as in its
/// handshake.
constexpr std::size_t MAX_FRAMES_BEFORE_CUT = 3;

/// Bytes into a connection a sampled cut may land at.
constexpr std::uint64_t SAMPLED_CUT_RANGE = 16 * 1024;

/// Cuts every connection, the way §16 lays out for the long scenario, and
/// counts into `counts` how many died of each kind.
/// How many connections died of each kind of cut.
using CutCounts = std::array<std::size_t, BOUNDARY_CUT_KINDS>;

CutPlan CutAtBoundaries(std::uint64_t seed,
                        const std::shared_ptr<CutCounts>& counts) {
    struct State {
        std::mt19937_64 Random;
        std::size_t Connection{SIZE_MAX};
        std::uint64_t SampledAt{0};
    };
    auto state =
        std::make_shared<State>(State{.Random = std::mt19937_64{seed}});
    return [state, counts](const FrameAt& frame) -> std::optional<std::size_t> {
        const auto die = [&](std::size_t pass) -> std::optional<std::size_t> {
            ++counts->at(frame.Connection % BOUNDARY_CUT_KINDS);
            return pass;
        };
        if (state->Connection != frame.Connection) {
            state->Connection = frame.Connection;
            std::uniform_int_distribution<std::uint64_t> range{
                1, SAMPLED_CUT_RANGE};
            state->SampledAt = range(state->Random);
        }
        const auto kind =
            static_cast<EBoundaryCut>(frame.Connection % BOUNDARY_CUT_KINDS);
        const std::size_t whole =
            1 + (frame.Connection / BOUNDARY_CUT_KINDS) % MAX_FRAMES_BEFORE_CUT;
        switch (kind) {
            case EBoundaryCut::Before:
                // Size counts the header, so Size - 1 is never below zero.
                if (frame.Index == whole) {
                    return die(frame.Size - 1);
                }
                break;
            case EBoundaryCut::At:
                if (frame.Index == whole) {
                    return die(frame.Size);
                }
                break;
            case EBoundaryCut::After:
                if (frame.Index == whole + 1) {
                    return die(1);
                }
                break;
            case EBoundaryCut::Sampled:
                if (frame.Carried + frame.Size >= state->SampledAt) {
                    return die(static_cast<std::size_t>(state->SampledAt -
                                                        frame.Carried));
                }
                break;
        }
        return std::nullopt;
    };
}

void EveryOffset(EDirection direction) {
    const auto script = Script(ShortScenario(), SEED);
    // The uncut run tells how many bytes the cut direction carries at all.
    Rig reference{direction, script, NeverCut(), SessionLimits{}};
    REQUIRE(reference.Run() == script);
    REQUIRE(reference.Seen().CarriedPerConnection.size() == 1);
    const std::uint64_t total = reference.Seen().CarriedPerConnection.front();

    for (std::uint64_t offset = 0; offset < total; ++offset) {
        CAPTURE(offset);
        Rig rig{direction, script, CutOnceAt(offset), SessionLimits{}};
        CHECK(rig.Run() == script);
    }
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a cut at every byte of the agent's stream loses nothing") {
    EveryOffset(EDirection::AgentToClient);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a cut at every byte of the client's stream loses nothing") {
    EveryOffset(EDirection::ClientToAgent);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "the shell's end reaches the client after its last byte, cut anywhere") {
    // Every offset of a short stream and of the SessionEnd behind it: a cut
    // inside that frame has the next connection send it again.
    const auto script = Script(ENDING_SCENARIO, SEED);
    Rig reference{EDirection::AgentToClient, script, NeverCut(),
                  SessionLimits{}, EEnding::ShellExits};
    REQUIRE(reference.Run() == script);
    CHECK(reference.EndsHeard() == 1);
    REQUIRE(reference.Seen().CarriedPerConnection.size() == 1);
    const std::uint64_t total = reference.Seen().CarriedPerConnection.front();

    for (std::uint64_t offset = 0; offset < total; ++offset) {
        CAPTURE(offset);
        Rig rig{EDirection::AgentToClient, script, CutOnceAt(offset),
                SessionLimits{}, EEnding::ShellExits};
        CHECK(rig.Run() == script);
        CHECK(rig.EndsHeard() >= 1);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "ten megabytes survive a cut on every reconnect") {
    CAPTURE(SEED);
    const auto script = Script(LONG_SCENARIO, SEED);
    auto counts = std::make_shared<CutCounts>();
    Rig rig{EDirection::AgentToClient, script, CutAtBoundaries(SEED, counts),
            SessionLimits{}};
    CHECK(rig.Run() == script);
    // Every connection but the last died, and each kind of cut came round
    // often enough to mean something.
    CAPTURE(rig.Seen().Connections);
    for (const std::size_t count : *counts) {
        CHECK(count >= MIN_CUTS_PER_KIND);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a cut either side of the resend ceiling loses nothing") {
    // Twice the ceiling, so the writer sits in backpressure when the cut
    // comes.
    const auto script = Script(2 * RESEND_BUFFER_MAX, SEED);
    for (const std::uint64_t at : {std::uint64_t{RESEND_BUFFER_MAX} - 1,
                                   std::uint64_t{RESEND_BUFFER_MAX},
                                   std::uint64_t{RESEND_BUFFER_MAX} + 1}) {
        CAPTURE(at);
        Rig rig{EDirection::AgentToClient, script, CutOnceAt(at),
                SessionLimits{}};
        CHECK(rig.Run() == script);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a detach holding the disconnect ceiling resumes whole") {
    // The cut comes before any output, so all of it piles up detached.
    for (const std::size_t held :
         {DISCONNECT_BUFFER_MAX - 1, DISCONNECT_BUFFER_MAX}) {
        CAPTURE(held);
        const auto script = Script(held, SEED);
        Rig rig{EDirection::AgentToClient, script, CutOnceAt(0),
                SessionLimits{}};
        rig.RunUntilCut();
        const std::size_t rest = rig.Unwritten();
        REQUIRE(rig.WriteRestDetached() == rest);
        // Nothing went out before the cut, so all of it waits unacknowledged.
        CHECK(rig.Agent().State() == ESessionState::Detached);
        CHECK(rig.Run() == script);
    }
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "one byte past the disconnect ceiling ends the session with a reason") {
    // §9: past the ceiling the stream cannot be resumed whole, and losing a
    // byte silently is the one thing not allowed — so the session ends, and
    // says why.
    const auto script = Script(DISCONNECT_BUFFER_MAX + 1, SEED);
    Rig rig{EDirection::AgentToClient, script, CutOnceAt(0), SessionLimits{}};
    rig.RunUntilCut();
    const auto overflow = rig.WriteRestDetached();
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == ESessionError::DisconnectBufferOverflow);
    CHECK(rig.Agent().State() == ESessionState::Reaped);
}
