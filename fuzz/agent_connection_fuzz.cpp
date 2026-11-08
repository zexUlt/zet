// AgentConnection fuzzer, §16 / §17.
//
// One AgentConnection over one AgentSession is driven the way a hostile or
// buggy client would drive it: bytes in chunks cut from the input, interleaved
// with time advances and the owner's calls (Pump, Write, InputDelivered,
// SupplySnapshot, SetReadGate, SetSendGate, OnTimer, Close, and the EchoAck
// calls of the agent loop: InputWritten, EchoDue/SettleEcho, SetPtyReadOpen).
// session_fuzz and session_fuzz_v2 start after the handshake and talk to a
// Channel; this one starts at the first byte of the connection and covers the
// stage machine around it: Hello, routing, the handshake, Resume, path B and
// the live stage.
//
// Reaching the live stage
// -----------------------
// Random ciphertext never authenticates, so the fuzzer holds the session's
// secrets, as a client that knows K_master would. The first input byte picks
// how far the peer is honest:
//
//   mode 0  nothing honest: every byte comes from the input
//   mode 1  a real ClientConnection sends its Hello; the input does the rest
//   mode 2  a real ClientConnection runs Hello, Auth and Resume; the agent is
//           live, and the input's bytes follow. Sealed frames of the input's
//           choosing are made with keys derived from the same secrets
//   mode 3  as mode 2 but the agent routes to RouteUnknown, so the handshake
//           must end in a refusal and the connection can never go live
//
// Input layout: byte 0 = mode (bits 0-1), ceiling 1 (bit 2), GRID_CAPABLE
// (bit 3), auto-acknowledge of delivered input (bit 4), ForcePath::B (bit 5,
// which puts a GRID_CAPABLE peer on path B), output written before the
// connection (bit 6), ECHO_ACK offered in Hello (bit 7). Then operations, the
// opcode being the low 4 bits of the first byte of each:
//
//  00 cs nn [nn B]   Raw       — feed nn bytes as they are; cs picks the chunk
//                                size, and bit 7 of cs ignores ReadRoom so that
//                                an oversized read meets the error path
//  01 cs nn [nn B]   Inject    — seal nn bytes as a frame from the client
//                                (modes 2 and 3) and feed it
//  02                Pump      — until Idle or an error
//  03 dd             Advance   — dd < 128: dd * 50 ms; else (dd - 127) s
//  04 b              ReadGate  — SetReadGate(b & 1)
//  05 b              SendGate  — SetSendGate(b & 1)
//  06 nn             Write     — the shell prints nn bytes
//  07 ff             AckInput  — for ff/255 of what is pending: InputWritten
//                                (ff odd, the pty took it at the current time)
//                                or InputDelivered (ff even, discarded)
//  08 hi lo          Supply    — SupplySnapshot of (hi:lo) % 8193 bytes
//  09                OwnerTimer — AgentSession::OnTimer
//  0A                Close     — Close(now); the run ends
//  0B                EchoTimer — if EchoDue(now): SettleEcho(now)
//  0C b              PtyRead   — SetPtyReadOpen(b & 1)
//  0D dd             Tick      — Advance by dd ms: the spacing of two EchoAcks
//                                (ECHO_ACK_INTERVAL_MIN, 20 ms) is below the
//                                50 ms step of Advance
//
// Properties (each violation → __builtin_trap())
// -----------------------------------------------
//  C1  A connection that failed stays failed: every later Pump returns the
//      same error, never an event.
//  C2  An error is a disposition, not a crash: KillSession only from a peer
//      that authenticated, and then the session is already Reaped. A peer
//      that did not authenticate (modes 0, 1, 3) leaves the session as it was
//      (§5: nothing to mutate before tag_c): no remembered nonces, no
//      attached state, no end, no moved receive offset. It is never IsLive.
//  C3  Memory is bounded: ReadRoom <= ReadCapacity <= one whole frame, and
//      before authentication ReadCapacity is one handshake frame; Pump never
//      writes more than CONNECTION_OUT_SIZE nor delivers more than
//      CONNECTION_SCRATCH_SIZE; the Pump loop goes quiet.
//  C4  The session's receive offset never decreases and never passes what the
//      connection has delivered; InputDelivered of what is pending succeeds.
//  C5  Every frame the agent sends after AuthOk opens under the keys derived
//      from the session's secrets, in order (modes 2 and 3).
//  C6  An honest handshake (mode 2) ends live, not failed.
//  C7  A closed send gate holds back every frame of a connection that was
//      live when it closed; a closed read gate keeps PeerSilent from firing.
//  C8  Events only an agent produces: never Ended, ApplySnapshot or
//      SessionOverflowed. NeedsSession is asked once.
//  C9  After Close the session is not Live.
//  C10 Every EchoAck the agent sends (seen in mode 2, where the harness opens
//      the agent's frames) is one the connection may send: only when ECHO_ACK
//      was offered and the version is 2; never between GridBegin and GridEnd;
//      stream_gen is the generation the connection is in; output_offset is at
//      most the highest end of a Data the agent has sealed in that generation;
//      the fields never decrease within a generation (input_offset never at
//      all, across generations too). Also: input_offset is 0 or the end of an
//      input write the pty took (InputWritten, not InputDelivered) at least
//      ECHO_TIMEOUT before; the first EchoAck of a negotiated connection
//      precedes its first Data; two EchoAcks are at least ECHO_ACK_INTERVAL_MIN
//      apart (checked while no send gate has held a frame back).
//  C11 After SettleEcho(now) the EchoAck is not due again at `now`: a due
//      deadline the settle cannot satisfy is a spin of the agent loop.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/client_session.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/stream_state.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/sealed_frame.hpp"

namespace {

using zet::ByteSpan;
using zet::MutableByteSpan;
using zet::Timestamp;
using namespace zet::session;
namespace wire = zet::wire;
namespace crypto = zet::crypto;

// Streams small enough that backpressure and overflow are a few writes away.
constexpr std::size_t STREAM_STORAGE = 4096;

// Per-run operation budget.
constexpr std::size_t MAX_OPS = 128;

// A Pump loop that has not gone quiet after this many events is spinning.
constexpr int MAX_PUMP_STEPS = 10'000;

// Widest plaintext one Inject seals.
constexpr std::size_t MAX_INJECT_LEN = 255;

constexpr std::size_t SNAPSHOT_STORE = 8192;

// Agent frames before the first sealed one: Challenge and AuthOk.
constexpr unsigned CLEARTEXT_AGENT_FRAMES = 2;

// Client frames of the handshake: Hello, Auth, Resume.
constexpr unsigned CLIENT_HANDSHAKE_FRAMES = 3;

constexpr std::uint8_t MASTER_SEED = 0x42;
constexpr std::uint8_t CLIENT_NONCE_SEED = 0xAB;
constexpr std::uint8_t SERVER_NONCE_SEED = 0xCD;
constexpr std::uint8_t LOCAL_SECRET_SEED = 0x99;

[[nodiscard]] crypto::Key MakeKey(std::uint8_t seed) noexcept {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte{static_cast<std::uint8_t>(
            seed ^ static_cast<std::uint8_t>(i * 7U))};
    }
    return crypto::Key{bytes};
}

[[nodiscard]] wire::HandshakeNonce MakeNonce(std::uint8_t seed) noexcept {
    wire::HandshakeNonce n{};
    for (std::size_t i = 0; i < n.size(); ++i) {
        n[i] = std::byte{static_cast<std::uint8_t>(
            seed ^ static_cast<std::uint8_t>(i * 11U))};
    }
    return n;
}

[[nodiscard]] crypto::Key CloneKey(const crypto::Key& k) noexcept {
    crypto::Key copy;
    copy.Expose() = k.Expose();
    return copy;
}

// Names the property that broke before trapping: the artifact reproduces it,
// but a CI log without the line is a slower start.
[[noreturn]] void Violation(const char* what, int line) noexcept {
    std::fprintf(stderr, "agent_connection_fuzz: violated %s (line %d)\n", what,
                 line);
    __builtin_trap();
}

#define REQUIRE(condition) \
    ((condition) ? static_cast<void>(0) : Violation(#condition, __LINE__))
#define VIOLATION() Violation("unreachable", __LINE__)

// Too large for a fuzzer thread's stack, and every user overwrites what it
// uses, so one copy serves all runs.
std::array<std::byte, CONNECTION_FRAME_STORAGE> G_AGENT_FRAMES{};
std::array<std::byte, CONNECTION_FRAME_STORAGE> G_CLIENT_FRAMES{};
std::array<std::byte, CONNECTION_OUT_SIZE> G_OUT{};
std::array<std::byte, CONNECTION_OUT_SIZE> G_CLIENT_OUT{};
std::array<std::byte, CONNECTION_SCRATCH_SIZE> G_SCRATCH{};
std::array<std::byte, CONNECTION_SCRATCH_SIZE> G_CLIENT_SCRATCH{};
std::array<std::byte, wire::MAX_FRAME> G_OPEN_SCRATCH{};
std::array<std::byte, SNAPSHOT_STORE> G_SNAPSHOT = [] {
    std::array<std::byte, SNAPSHOT_STORE> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte{static_cast<std::uint8_t>(i * 31U + 7U)};
    }
    return bytes;
}();

// What the session shows from outside before a peer has authenticated.
struct SessionView {
    std::size_t Nonces{0};
    std::uint64_t ReceiveOffset{0};
    ESessionState State{ESessionState::Detached};
    bool Ended{false};
    bool Reaped{false};

    [[nodiscard]] bool operator==(const SessionView&) const noexcept = default;
};

[[nodiscard]] SessionView ViewOf(AgentSession& session) noexcept {
    return SessionView{.Nonces = session.Ref().RememberedNonces(),
                       .ReceiveOffset = session.Ref().ReceiveOffset(),
                       .State = session.State(),
                       .Ended = session.HasEnded(),
                       .Reaped = session.ReapReason().has_value()};
}

[[nodiscard]] wire::SessionSecrets Secrets() noexcept {
    return wire::DeriveSessionSecrets(MakeKey(MASTER_SEED));
}

struct Rig {
    Timestamp Now{Timestamp::FromNanos(1'000'000'000)};
    std::array<std::byte, STREAM_STORAGE> AgentStore{};
    std::array<std::byte, STREAM_STORAGE> ClientStore{};
    crypto::Key LocalSecret{MakeKey(LOCAL_SECRET_SEED)};
    AgentSession Session;
    ClientSession Client;
    AgentConnection Agent;
    SessionView Initial;

    bool RouteUnknown{false};
    // True until a peer has proved K_auth, and for ever on the modes where it
    // cannot.
    bool PreAuth{true};
    bool Dead{false};
    bool Closed{false};
    std::optional<SessionError> Failed;

    unsigned RouteAsked{0};
    unsigned AgentFrames{0};
    std::optional<wire::FrameOpener> FromAgent;
    std::optional<wire::FrameSealer> ToAgent;

    bool ReadGateClosedLive{false};
    bool SendGateClosedLive{false};
    bool AutoAck{false};
    std::uint64_t Delivered{0};
    std::uint64_t Marked{0};
    std::uint64_t LastReceiveOffset{0};
    std::uint64_t Pattern{0};

    // C10: what the agent's frames have shown so far.
    bool EchoOffered{false};
    std::uint16_t Ceiling{0};
    bool InGrid{false};
    std::uint32_t Gen{0};
    // The highest end of any Data of the generation, across connections: a
    // resend after a rewind does not lower it.
    std::uint64_t SentEnd{0};
    std::optional<wire::EchoAckMessage> LastAck;
    // Across generations: input_offset never goes back.
    std::uint64_t LastInput{0};
    // The ends of the input writes the pty took, with the moment of each.
    std::vector<std::pair<std::uint64_t, Timestamp>> Writes;
    bool SawEcho{false};
    // Send times of the EchoAcks, for the spacing. A frame held back by a
    // closed send gate leaves the send time later than the moment it was
    // queued, so a gate closing taints the spacing of this and the next one.
    std::optional<Timestamp> LastAckAt;
    bool GateClosedSinceAck{false};
    bool PrevAckDelayed{false};

    Rig(std::uint16_t ceiling, bool autoAck, EForcePath force, bool echoOffered)
        : Session(Secrets(),
                  SessionStreams{.Storage = MutableByteSpan{AgentStore},
                                 .Now = Now,
                                 .Limits = {.ResendMax = STREAM_STORAGE,
                                            .DisconnectMax = STREAM_STORAGE,
                                            .ForcePath = force}}),
          Client(Secrets(),
                 SessionStreams{.Storage = MutableByteSpan{ClientStore},
                                .Now = Now,
                                .Limits = {.ResendMax = STREAM_STORAGE,
                                           .DisconnectMax = STREAM_STORAGE}}),
          Agent(MutableByteSpan{G_AGENT_FRAMES}, Now, ceiling),
          Initial(ViewOf(Session)),
          AutoAck(autoAck),
          EchoOffered(echoOffered),
          Ceiling(ceiling) {}
};

// C10: the input the pty took just now, `count` bytes ending at Marked.
void NoteWrite(Rig& rig, std::uint64_t count) {
    if (count > 0) {
        rig.Writes.emplace_back(rig.Marked, rig.Now);
    }
}

void CheckInvariants(Rig& rig) noexcept {
    // C3
    REQUIRE(rig.Agent.ReadRoom() <= rig.Agent.ReadCapacity());
    REQUIRE(rig.Agent.ReadCapacity() <= CONNECTION_FRAME_STORAGE);
    if (rig.PreAuth) {
        REQUIRE(rig.Agent.ReadCapacity() <=
                wire::HEADER_SIZE + wire::MAX_HANDSHAKE_FRAME);
        // C2
        REQUIRE(!rig.Agent.IsLive());
        REQUIRE(ViewOf(rig.Session) == rig.Initial);
    }
    // C4
    const auto offset = rig.Session.Ref().ReceiveOffset();
    REQUIRE(offset >= rig.LastReceiveOffset);
    REQUIRE(offset <= rig.Delivered);
    rig.LastReceiveOffset = offset;
}

void OnPumpError(Rig& rig, const SessionError& error) noexcept {
    if (rig.Failed) {
        // C1
        REQUIRE(error == *rig.Failed);
        return;
    }
    rig.Failed = error;
    // C2
    if (error.Disposition() == zet::EDisposition::KillSession) {
        REQUIRE(!rig.PreAuth);
        REQUIRE(rig.Session.State() == ESessionState::Reaped);
    }
    // C7
    if (error.Session() == ESessionError::PeerSilent) {
        REQUIRE(!rig.ReadGateClosedLive);
    }
}

// C10. A frame the harness could not parse is C5's business, not this one's.
void SniffAgentMessage(Rig& rig, ByteSpan body) noexcept {
    zet::ByteReader reader{body};
    const auto type = wire::ParseMessageType(reader);
    if (!type) {
        return;
    }
    switch (*type) {
        case wire::EMessageType::Data: {
            const auto data = wire::ParseData(reader);
            if (rig.EchoOffered && rig.Ceiling == wire::PROTOCOL_VERSION_MAX) {
                // The first EchoAck of the connection goes before any Data.
                REQUIRE(rig.SawEcho);
            }
            if (data) {
                rig.SentEnd = std::max<std::uint64_t>(
                    rig.SentEnd, data->Offset + data->Payload.size());
            }
            break;
        }
        case wire::EMessageType::GridBegin: {
            const auto begin = wire::ParseGridBegin(reader);
            if (begin) {
                rig.InGrid = true;
                rig.Gen = begin->StreamGen;
                rig.SentEnd = 0;
                rig.LastAck.reset();
            }
            break;
        }
        case wire::EMessageType::GridEnd:
            rig.InGrid = false;
            break;
        case wire::EMessageType::EchoAck: {
            const auto ack = wire::ParseEchoAck(reader);
            REQUIRE(ack.has_value());
            REQUIRE(rig.EchoOffered &&
                    rig.Ceiling == wire::PROTOCOL_VERSION_MAX);
            REQUIRE(!rig.InGrid);
            REQUIRE(ack->StreamGen == rig.Gen);
            REQUIRE(ack->OutputOffset <= rig.SentEnd);
            if (rig.LastAck) {
                REQUIRE(ack->OutputOffset >= rig.LastAck->OutputOffset);
                REQUIRE(ack->InputOffset >= rig.LastAck->InputOffset);
            }
            rig.LastAck = *ack;
            rig.SawEcho = true;
            // The input is what the pty took, and it lay there ECHO_TIMEOUT.
            REQUIRE(ack->InputOffset >= rig.LastInput);
            rig.LastInput = ack->InputOffset;
            if (ack->InputOffset != 0) {
                const auto write = std::find_if(
                    rig.Writes.begin(), rig.Writes.end(),
                    [&](const auto& w) { return w.first == ack->InputOffset; });
                REQUIRE(write != rig.Writes.end());
                REQUIRE(rig.Now - write->second >= zet::session::ECHO_TIMEOUT);
            }
            // The spacing, where the send time is the queue time.
            if (rig.LastAckAt && !rig.PrevAckDelayed &&
                !rig.GateClosedSinceAck) {
                REQUIRE(rig.Now - *rig.LastAckAt >=
                        zet::session::ECHO_ACK_INTERVAL_MIN);
            }
            rig.PrevAckDelayed = rig.GateClosedSinceAck;
            rig.GateClosedSinceAck = false;
            rig.LastAckAt = rig.Now;
            break;
        }
        default:
            break;
    }
}

// Reads what the agent put in G_OUT. `toClient` is told about the cleartext
// frames the real client has to see; the sealed ones go through the opener.
template <typename TToClient>
void OnAgentFrame(Rig& rig, std::size_t written, TToClient&& toClient) {
    // C3
    REQUIRE(written >= wire::HEADER_SIZE && written <= CONNECTION_OUT_SIZE);
    ++rig.AgentFrames;
    const ByteSpan frame = ByteSpan{G_OUT}.first(written);
    if (rig.AgentFrames <= CLEARTEXT_AGENT_FRAMES) {
        toClient(frame);
        return;
    }
    if (!rig.FromAgent) {
        return;
    }
    // C5
    const auto view = wire::ReadFrame(frame, wire::MAX_FRAME);
    REQUIRE(view.has_value() && view->Consumed == written);
    const auto body =
        rig.FromAgent->Open(MutableByteSpan{G_OPEN_SCRATCH}, *view);
    REQUIRE(body.has_value());
    SniffAgentMessage(rig, *body);
}

// Pump until Idle or an error. Returns true if it ended on an error.
template <typename TToClient>
bool PumpWith(Rig& rig, TToClient&& toClient) {
    for (int step = 0; step < MAX_PUMP_STEPS; ++step) {
        const bool liveBefore = rig.Agent.IsLive();
        const auto event = rig.Agent.Pump(rig.Now, MutableByteSpan{G_OUT},
                                          MutableByteSpan{G_SCRATCH});
        if (!event) {
            OnPumpError(rig, event.error());
            CheckInvariants(rig);
            return true;
        }
        // C1
        REQUIRE(!rig.Failed.has_value());

        if (const auto* need = std::get_if<NeedsSession>(&*event)) {
            // C8
            REQUIRE(++rig.RouteAsked == 1);
            // What the zet-agent does: route to the session the sid names,
            // else to the decoy.
            if (rig.RouteUnknown || need->Id != rig.Session.Ref().Id()) {
                rig.Agent.RouteUnknown(rig.LocalSecret,
                                       MakeNonce(SERVER_NONCE_SEED));
            } else {
                rig.Agent.Route(rig.Session.Ref(),
                                MakeNonce(SERVER_NONCE_SEED));
            }
        } else if (const auto* send = std::get_if<SendFrame>(&*event)) {
            // C7
            REQUIRE(!(rig.SendGateClosedLive && liveBefore));
            OnAgentFrame(rig, send->Written, toClient);
        } else if (const auto* deliver = std::get_if<Deliver>(&*event)) {
            // C3
            REQUIRE(deliver->Bytes.size() <= CONNECTION_SCRATCH_SIZE);
            rig.Delivered += deliver->Bytes.size();
            if (rig.AutoAck) {
                const auto pending = rig.Delivered - rig.Marked;
                REQUIRE(rig.Session.InputWritten(pending, rig.Now));
                rig.Marked = rig.Delivered;
                NoteWrite(rig, pending);
            }
        } else if (std::holds_alternative<Idle>(*event)) {
            CheckInvariants(rig);
            return false;
        } else if (std::holds_alternative<Ended>(*event) ||
                   std::holds_alternative<ApplySnapshot>(*event) ||
                   std::holds_alternative<SessionOverflowed>(*event)) {
            // C8
            VIOLATION();
        }
        CheckInvariants(rig);
    }
    VIOLATION();
}

void Pump(Rig& rig) {
    std::ignore = PumpWith(rig, [](ByteSpan /*frame*/) {});
}

[[nodiscard]] std::size_t ChunkSize(std::uint8_t selector) noexcept {
    static constexpr std::array<std::size_t, 8> SIZES{1,  2,  3,    5,
                                                      13, 64, 1024, 65536};
    return SIZES.at(selector & 7U);
}

// Hands bytes to OnBytes in pieces. Bit 7 of `selector` ignores ReadRoom, as a
// caller that reads whatever the socket has would; otherwise a full buffer is
// drained by Pump first, as the zet-agent does. An error from OnBytes is the
// frame layer's refusal: the caller gives the connection up, after one more
// Pump to let it say why.
void Feed(Rig& rig, ByteSpan bytes, std::uint8_t selector) {
    const bool ignoreRoom = (selector & 0x80U) != 0;
    std::size_t pos = 0;
    int guard = 0;
    while (pos < bytes.size() && !rig.Dead && ++guard < 4096) {
        std::size_t piece =
            std::min(ChunkSize(selector >> 3U), bytes.size() - pos);
        if (!ignoreRoom) {
            if (rig.Agent.ReadRoom() == 0) {
                Pump(rig);
            }
            piece = std::min(piece, rig.Agent.ReadRoom());
            if (piece == 0) {
                return;
            }
        }
        if (!rig.Agent.OnBytes(bytes.subspan(pos, piece))) {
            Pump(rig);
            rig.Dead = true;
            return;
        }
        pos += piece;
        CheckInvariants(rig);
    }
}

// Mode 1-3 front half: a real client says Hello, and in modes 2 and 3 goes on
// to Auth and Resume. Stops sending after Resume, so that the connection's
// first sealed frame from the client is the one the input's frames follow.
void Handshake(Rig& rig, std::uint8_t mode, bool gridCapable,
               std::uint16_t ceiling) {
    ClientConnection client{rig.Client,
                            MakeNonce(CLIENT_NONCE_SEED),
                            MutableByteSpan{G_CLIENT_FRAMES},
                            rig.Now,
                            gridCapable,
                            ceiling,
                            {},
                            rig.EchoOffered};
    unsigned sent = 0;
    const unsigned limit = mode == 1 ? 1 : CLIENT_HANDSHAKE_FRAMES;
    bool agentDone = false;
    auto toClient = [&](ByteSpan frame) {
        if (mode != 1) {
            REQUIRE(client.OnBytes(frame).has_value());
        }
    };
    for (int round = 0; round < 64 && !agentDone; ++round) {
        agentDone = PumpWith(rig, toClient);
        if (agentDone || sent >= limit) {
            break;
        }
        const auto event = client.Pump(rig.Now, MutableByteSpan{G_CLIENT_OUT},
                                       MutableByteSpan{G_CLIENT_SCRATCH});
        if (!event) {
            // The client only fails on what the agent sent, and the agent
            // is the one under test: a decoy ends here, an honest agent does
            // not.
            REQUIRE(mode == 3);
            break;
        }
        if (const auto* send = std::get_if<SendFrame>(&*event)) {
            ++sent;
            if (sent == 2) {
                // Everything before the Auth reaches the agent has left the
                // session untouched (§5).
                REQUIRE(ViewOf(rig.Session) == rig.Initial);
            }
            if (sent == 2 && mode != 3) {
                rig.PreAuth = false;
            }
            const auto accepted =
                rig.Agent.OnBytes(ByteSpan{G_CLIENT_OUT}.first(send->Written));
            REQUIRE(accepted.has_value());
        }
    }
    if (mode == 2) {
        agentDone = PumpWith(rig, toClient);
        // C6
        REQUIRE(!agentDone && rig.Agent.IsLive());
    }
}

[[nodiscard]] bool Live(const Rig& rig) noexcept {
    return rig.Agent.IsLive() && !rig.Failed;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    if (!crypto::Init()) {
        __builtin_trap();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    zet::ByteReader reader{
        ByteSpan{reinterpret_cast<const std::byte*>(data), size}};
    const auto config = reader.ReadU8();
    if (!config) {
        return 0;
    }
    const std::uint8_t mode = *config & 0x03U;
    const std::uint16_t ceiling = (*config & 0x04U) != 0
                                      ? wire::PROTOCOL_VERSION_MIN
                                      : wire::PROTOCOL_VERSION_MAX;
    const bool gridCapable = (*config & 0x08U) != 0;

    Rig rig{ceiling, (*config & 0x10U) != 0,
            (*config & 0x20U) != 0 ? EForcePath::B : EForcePath::None,
            (*config & 0x80U) != 0};
    if ((*config & 0x40U) != 0) {
        // The shell printed while nobody was connected, so that Resume has
        // something to replay or to supersede with a snapshot.
        std::array<std::byte, 700> printed{};
        for (std::size_t i = 0; i < printed.size(); ++i) {
            printed.at(i) = std::byte{static_cast<std::uint8_t>(i * 3U + 1U)};
        }
        std::ignore = rig.Session.Write(ByteSpan{printed});
        rig.Initial = ViewOf(rig.Session);
    }
    rig.RouteUnknown = mode == 3;

    if (mode == 2) {
        // Derived the way both ends do from what the client holds. The opener
        // is in place before the handshake: the agent's first sealed frames
        // may come before the input has said anything.
        const auto keys = wire::DeriveConnectionKeys(
            Secrets().ConnectionSeed, MakeNonce(CLIENT_NONCE_SEED),
            MakeNonce(SERVER_NONCE_SEED));
        rig.FromAgent.emplace(CloneKey(keys.ServerToClient),
                              keys.SaltServerToClient);
        rig.ToAgent.emplace(CloneKey(keys.ClientToServer),
                            keys.SaltClientToServer);
    }
    if (mode != 0) {
        Handshake(rig, mode, gridCapable, ceiling);
    }
    if (mode == 2) {
        // The client's sealer is where the real client left it: Resume was
        // the first frame.
        std::array<std::byte, wire::HEADER_SIZE + crypto::TAG_SIZE> burn{};
        REQUIRE(rig.ToAgent->Seal(MutableByteSpan{burn}, {}).has_value());
    }
    CheckInvariants(rig);

    std::size_t ops = 0;
    while (ops < MAX_OPS && !reader.Exhausted() && !rig.Closed) {
        ++ops;
        const auto opByte = reader.ReadU8();
        if (!opByte) {
            break;
        }
        switch (*opByte & 0x0FU) {
            case 0x00:
            case 0x01: {
                const auto selector = reader.ReadU8();
                const auto length = reader.ReadU8();
                if (!selector || !length) {
                    break;
                }
                const auto payload = reader.ReadBytes(
                    std::min<std::size_t>(*length, reader.Remaining()));
                if (!payload) {
                    break;
                }
                if ((*opByte & 0x0FU) == 0x00) {
                    Feed(rig, *payload, *selector);
                    break;
                }
                if (!rig.ToAgent || payload->size() > MAX_INJECT_LEN) {
                    break;
                }
                std::array<std::byte, wire::HEADER_SIZE + MAX_INJECT_LEN +
                                          crypto::TAG_SIZE>
                    sealed{};
                const auto written =
                    rig.ToAgent->Seal(MutableByteSpan{sealed}, *payload);
                if (written) {
                    Feed(rig, ByteSpan{sealed}.first(*written), *selector);
                }
                break;
            }
            case 0x02:
                Pump(rig);
                break;
            case 0x03: {
                const auto delta = reader.ReadU8();
                if (!delta) {
                    break;
                }
                rig.Now += *delta < 128 ? std::chrono::milliseconds{*delta * 50}
                                        : std::chrono::seconds{*delta - 127};
                break;
            }
            case 0x04: {
                const auto open = reader.ReadU8();
                if (!open) {
                    break;
                }
                rig.Agent.SetReadGate((*open & 1U) != 0, rig.Now);
                // A call before the connection is live is dropped.
                rig.ReadGateClosedLive = (*open & 1U) == 0 && Live(rig) ? true
                                         : (*open & 1U) != 0
                                             ? false
                                             : rig.ReadGateClosedLive;
                break;
            }
            case 0x05: {
                const auto open = reader.ReadU8();
                if (!open) {
                    break;
                }
                rig.Agent.SetSendGate((*open & 1U) != 0);
                if ((*open & 1U) == 0) {
                    rig.GateClosedSinceAck = true;
                }
                rig.SendGateClosedLive = (*open & 1U) == 0 && Live(rig) ? true
                                         : (*open & 1U) != 0
                                             ? false
                                             : rig.SendGateClosedLive;
                break;
            }
            case 0x06: {
                const auto count = reader.ReadU8();
                if (!count) {
                    break;
                }
                std::array<std::byte, 256> bytes{};
                for (std::size_t i = 0; i < *count; ++i) {
                    bytes.at(i) = std::byte{
                        static_cast<std::uint8_t>(rig.Pattern++ * 13U + 5U)};
                }
                const auto taken = rig.Session.Write(
                    ByteSpan{bytes}.first(static_cast<std::size_t>(*count)));
                REQUIRE(!taken || *taken <= *count);
                // The owner's own doing: an overflow while nobody is
                // connected reaps the session, and the peer had no part in it.
                rig.Initial = ViewOf(rig.Session);
                break;
            }
            case 0x07: {
                const auto fraction = reader.ReadU8();
                if (!fraction) {
                    break;
                }
                const auto pending = rig.Delivered - rig.Marked;
                const auto count = pending * *fraction / 255U;
                // C4
                REQUIRE(((*fraction & 1U) != 0
                             ? rig.Session.InputWritten(count, rig.Now)
                             : rig.Session.InputDelivered(count)));
                rig.Marked += count;
                if ((*fraction & 1U) != 0) {
                    NoteWrite(rig, count);
                }
                break;
            }
            case 0x08: {
                const auto high = reader.ReadU8();
                const auto low = reader.ReadU8();
                if (!high || !low) {
                    break;
                }
                const std::size_t length =
                    ((std::size_t{*high} << 8U) | *low) % (SNAPSHOT_STORE + 1);
                rig.Agent.SupplySnapshot(
                    rig.Now, ByteSpan{G_SNAPSHOT}.first(length), 0, 0);
                // The loop pumps right after: the EchoAck that opens the
                // generation is sent at the moment it is queued.
                Pump(rig);
                break;
            }
            case 0x09:
                rig.Session.OnTimer(rig.Now);
                rig.Initial = ViewOf(rig.Session);
                break;
            case 0x0A:
                rig.Agent.Close(rig.Now);
                rig.Closed = true;
                break;
            case 0x0B:
                if (rig.Agent.EchoDue(rig.Now)) {
                    rig.Agent.SettleEcho(rig.Now);
                    // C11
                    REQUIRE(!rig.Agent.EchoDue(rig.Now));
                    // The loop sends what the settle queued at once.
                    Pump(rig);
                }
                break;
            case 0x0C: {
                const auto open = reader.ReadU8();
                if (!open) {
                    break;
                }
                rig.Agent.SetPtyReadOpen((*open & 1U) != 0);
                break;
            }
            case 0x0D: {
                const auto delta = reader.ReadU8();
                if (!delta) {
                    break;
                }
                rig.Now += std::chrono::milliseconds{*delta};
                break;
            }
            default:
                Pump(rig);
                break;
        }
        CheckInvariants(rig);
        if (rig.Dead) {
            break;
        }
    }

    if (!rig.Closed) {
        rig.Agent.Close(rig.Now);
    }
    // C9
    REQUIRE(rig.Session.State() != ESessionState::Live);
    return 0;
}
