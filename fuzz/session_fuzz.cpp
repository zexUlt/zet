// Post-authentication Channel pair fuzzer, §16 / §17 M2.
//
// A Channel pair (Client + Agent) sharing one fixed ConnectionKeys set
// processes a byte-coded sequence of operations. The fuzzer never crosses
// the AEAD barrier by guessing ciphertext; instead it holds the same keys
// as both sides and seals inject frames itself.  This reaches the message
// parser and stream logic — the layers §16 says "random ciphertext never
// would" — while relying on sealed_frame_fuzz to stress the AEAD path.
//
// Injection sealer design
// -----------------------
// The Channel internally uses a FrameSealer/FrameOpener pair.  An opener
// accepts frames sealed at its current counter; a frame sealed at any other
// counter fails the AEAD tag.  To inject arbitrary plaintext that the
// receiver actually processes, we maintain a dedicated injection sealer per
// direction (S2cInject for agent→client, C2sInject for client→agent).
// Poll() is called for code-path coverage but its output is discarded;
// all delivery to Receive() goes through the injection sealers.  This keeps
// both sealers and both openers in lock-step: inject counter N → opener
// advances to N+1 → next inject at N+1.  Real Poll() frames are sealed by
// the Channel's own internal sealer (a separate counter), so they are never
// mixed with injection-sealer frames on the same opener — which would
// desynchronise the counter and cause AuthenticationFailed on every inject
// thereafter.  A reconnect-events extension (future PR) would need to reset
// both sealers and openers together when a new Channel is created over the
// same SendBuffer/ReceiveCursor, which is the natural boundary.
//
// Operations (opcode = low 3 bits of the first byte; high bits unused)
// --------------------------------------------------------------------
//  00 nn           AppendClient  — append nn+1 deterministic bytes to the
//                                  client's SendBuffer
//  01 nn           AppendAgent   — same for agent
//  02 dd           PollClient    — Client.Poll(now), advance now by dd ms;
//                                  check return-value bounds, discard frame
//  03 dd           PollAgent     — same for agent
//  04 nn [nn+1 B]  InjectToClient — seal nn+1 bytes with S2cInject,
//                                  deliver to Client.Receive()
//  05 nn [nn+1 B]  InjectToAgent  — seal nn+1 bytes with C2sInject,
//                                  deliver to Agent.Receive()
//  06 rr cc        QueueResize   — Client.QueueResize({rr+1, cc+1, 0, 0})
//  07 dd           AdvanceTime   — now += dd * 100 ms
//
// Properties (each violation → __builtin_trap())
// -----------------------------------------------
//  P1  recv_offset never decreases (ReceiveCursor)
//  P2  SendBuffer.Base() never decreases
//  P3  SendBuffer.Base() <= SendBuffer.SendNext() <= SendBuffer.End()
//  P4  Every byte delivered at absolute offset O equals what was first
//      delivered there (append-only, no rewrite), and DeliveredData::Offset
//      is the recv_offset the delivery started at.
//  P5  SendBuffer.Size() <= SendBuffer.Capacity()
//  P6  Receive() never returns KillSession: a Channel only ever closes its
//      connection, and killing the session is the layer above's call.
//  P7  Poll() return value in [0, HEADER_SIZE + MAX_FRAME].
//  P8  Operation budget: at most MAX_OPS steps for any input length.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/channel.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/error.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/receive_cursor.hpp"
#include "zet/session/send_buffer.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"
#include "zet/wire/sealed_frame.hpp"

namespace {

using zet::ByteSpan;
using zet::ByteWriter;
using zet::MutableByteSpan;
using zet::Timestamp;
using namespace zet::session;
using namespace zet::wire;

// Namespace aliases so that wire::X and crypto::X are valid qualified names
// alongside the unqualified ones brought in by the using-directives above.
// Without these, wire:: and crypto:: are not usable as prefixes because
// neither sub-namespace is a top-level name.
namespace wire = zet::wire;
namespace crypto = zet::crypto;

// SendBuffer capacity: small enough that backpressure is reachable quickly
// without spending the whole budget on appends.
constexpr std::size_t SEND_BUF_SIZE = 512;

// Small frame limit so that rekey paths are exercised without sealing millions
// of frames.  Eight frames per epoch gives a handful of rekeys in a typical
// run.
constexpr std::uint64_t FUZZ_FRAME_LIMIT = 8;

// Per-run operation budget: prevents spinning on inputs that keep producing
// empty Poll returns or keep failing injection.
constexpr std::size_t MAX_OPS = 128;

// Largest inject payload read from the input in one shot.  Wide enough to
// carry the widest session message (Data: 17-byte header + payload), but
// small enough that the sealer output fits in a fixed stack buffer.
constexpr std::size_t MAX_INJECT_LEN = 80;

// Max append bytes per AppendClient / AppendAgent.
constexpr std::size_t MAX_APPEND_LEN = 48;

// Deterministic reference byte at absolute stream offset i.
[[nodiscard]] constexpr std::byte RefByte(std::uint64_t i) noexcept {
    // Linear congruential mix — not cryptographic, just non-trivial.
    return std::byte{static_cast<std::uint8_t>(
        (i * 0x9e3779b97f4a7c15ULL + 0x6c62272e07bb0142ULL) >> 56U)};
}

[[nodiscard]] crypto::Key MakeKey(std::uint8_t seed) noexcept {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte{static_cast<std::uint8_t>(
            seed ^ static_cast<std::uint8_t>(i * 7U))};
    }
    return crypto::Key{bytes};
}

[[nodiscard]] HandshakeNonce MakeNonce(std::uint8_t seed) noexcept {
    HandshakeNonce n{};
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

// A FrameSealer together with an output buffer large enough to hold one sealed
// inject frame.
struct InjectSealer {
    FrameSealer Sealer;
    // HEADER_SIZE(5) + MAX_INJECT_LEN(80) + TAG_SIZE(16).
    std::array<std::byte, wire::HEADER_SIZE + MAX_INJECT_LEN + crypto::TAG_SIZE>
        Buf{};

    InjectSealer(crypto::Key key, DirectionSalt salt) noexcept
        : Sealer{std::move(key), salt, FUZZ_FRAME_LIMIT,
                 wire::MAX_BYTES_PER_EPOCH} {}

    // Seal `plain` and return a FrameView into Buf, or nullopt on any error.
    // If the epoch is exhausted, Rekey() is tried once.
    [[nodiscard]] std::optional<FrameView> Seal(ByteSpan plain) noexcept {
        if (plain.size() > MAX_INJECT_LEN) {
            return std::nullopt;
        }
        auto tryOnce = [&]() -> std::optional<FrameView> {
            const auto n = Sealer.Seal(
                MutableByteSpan{Buf}.first(wire::HEADER_SIZE + plain.size() +
                                           crypto::TAG_SIZE),
                plain);
            if (!n) {
                return std::nullopt;
            }
            const auto view =
                ReadFrame(ByteSpan{Buf}.first(*n), wire::MAX_FRAME);
            if (!view) {
                return std::nullopt;
            }
            return *view;
        };

        if (const auto v = tryOnce()) {
            return v;
        }
        // NeedsRekey: advance the epoch and try again.
        if (!Sealer.NeedsRekey()) {
            return std::nullopt;
        }
        if (!Sealer.Rekey()) {
            return std::nullopt;
        }
        return tryOnce();
    }
};

// Property snapshot taken before each operation.
struct Snapshot {
    std::uint64_t ClientRecvOffset{0};
    std::uint64_t AgentRecvOffset{0};
    std::uint64_t ClientSendBase{0};
    std::uint64_t AgentSendBase{0};
};

// Delivered-stream record: tracks every byte ever returned by Receive() so
// that we can assert the append-only property (P4).
struct StreamRecord {
    std::vector<std::byte> Bytes;
    std::uint64_t RecvEnd{0};  // == Bytes.size() == RecvCursor.RecvOffset()
};

// Extend the record with newly delivered bytes (from DeliveredData).
// Returns false if the delivered bytes would rewrite already-recorded content
// (P4 violation).
[[nodiscard]] bool RecordDelivery(StreamRecord& rec, std::uint64_t oldOffset,
                                  ByteSpan delivered) noexcept {
    // oldOffset is the stream offset of the first byte in `delivered`.
    if (oldOffset != rec.RecvEnd) {
        // The delivery didn't start exactly where our record ends.
        // This should be impossible: ReceiveCursor only yields new tail bytes.
        return false;
    }
    for (const std::byte b : delivered) {
        rec.Bytes.push_back(b);
    }
    rec.RecvEnd += static_cast<std::uint64_t>(delivered.size());
    return true;
}

// Check all monotonicity and bounds properties.
void CheckProperties(const Snapshot& before, const SendBuffer& clientSend,
                     const SendBuffer& agentSend,
                     const ReceiveCursor& clientRecv,
                     const ReceiveCursor& agentRecv) noexcept {
    // P1: recv_offset never decreases.
    if (clientRecv.RecvOffset() < before.ClientRecvOffset) {
        __builtin_trap();
    }
    if (agentRecv.RecvOffset() < before.AgentRecvOffset) {
        __builtin_trap();
    }
    // P2: SendBuffer.Base() never decreases.
    if (clientSend.Base() < before.ClientSendBase) {
        __builtin_trap();
    }
    if (agentSend.Base() < before.AgentSendBase) {
        __builtin_trap();
    }
    // P3: Base <= SendNext <= End.
    if (clientSend.SendNext() < clientSend.Base() ||
        clientSend.SendNext() > clientSend.End()) {
        __builtin_trap();
    }
    if (agentSend.SendNext() < agentSend.Base() ||
        agentSend.SendNext() > agentSend.End()) {
        __builtin_trap();
    }
    // P5: Size <= Capacity.
    if (clientSend.Size() > clientSend.Capacity()) {
        __builtin_trap();
    }
    if (agentSend.Size() > agentSend.Capacity()) {
        __builtin_trap();
    }
}

// Process a ChannelResult from Receive() and apply property checks.
void HandleReceiveResult(const ChannelResult<IncomingEvent>& result,
                         StreamRecord& rec, std::uint64_t oldRecvOffset,
                         const ReceiveCursor& cursor) noexcept {
    if (!result) {
        // P6.
        if (result.error().Disposition() == zet::EDisposition::KillSession) {
            __builtin_trap();
        }
        return;
    }

    if (std::holds_alternative<DeliveredData>(*result)) {
        const DeliveredData& dd = std::get<DeliveredData>(*result);
        if (dd.Offset != oldRecvOffset) {
            __builtin_trap();
        }
        // P4: bytes delivered at oldRecvOffset must extend the record.
        if (!RecordDelivery(rec, oldRecvOffset, dd.Bytes)) {
            __builtin_trap();
        }
        // RecvOffset should now equal rec.RecvEnd.
        if (cursor.RecvOffset() != rec.RecvEnd) {
            __builtin_trap();
        }
    }
}

// Perform one inject: seal `plain` and call Receive() on `chan`.
void DoInject(InjectSealer& injector, Channel& chan, ByteSpan plain,
              StreamRecord& rec, const ReceiveCursor& cursor,
              std::vector<std::byte>& scratch) noexcept {
    const std::uint64_t oldRecvOffset = cursor.RecvOffset();
    const auto frame = injector.Seal(plain);
    if (!frame) {
        // Seal failed (e.g., epochs exhausted): skip this inject, no state
        // changed.
        return;
    }
    if (scratch.size() < wire::MAX_FRAME) {
        scratch.resize(wire::MAX_FRAME);
    }
    const auto result = chan.Receive(*frame, MutableByteSpan{scratch});
    HandleReceiveResult(result, rec, oldRecvOffset, cursor);
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    if (!zet::crypto::Init()) {
        __builtin_trap();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    // Fixed connection keys derived from a fixed master — finding reproduces
    // from the artifact alone.
    static const ConnectionKeys kKeys =
        DeriveConnectionKeys(DeriveSessionSecrets(MakeKey(0x42)).ConnectionSeed,
                             MakeNonce(0xAB), MakeNonce(0xCD));

    // ── Storage ──────────────────────────────────────────────────────────

    std::array<std::byte, SEND_BUF_SIZE> clientSendStore{};
    std::array<std::byte, SEND_BUF_SIZE> agentSendStore{};

    SendBuffer clientSendBuf{MutableByteSpan{clientSendStore}, 0};
    SendBuffer agentSendBuf{MutableByteSpan{agentSendStore}, 0};

    ReceiveCursor clientRecvCursor{0};
    ReceiveCursor agentRecvCursor{0};

    Timestamp now{};

    Channel clientChan{kKeys,
                       ERole::Client,
                       clientSendBuf,
                       clientRecvCursor,
                       now,
                       FUZZ_FRAME_LIMIT,
                       wire::MAX_BYTES_PER_EPOCH};
    Channel agentChan{kKeys,
                      ERole::Agent,
                      agentSendBuf,
                      agentRecvCursor,
                      now,
                      FUZZ_FRAME_LIMIT,
                      wire::MAX_BYTES_PER_EPOCH};

    // Injection sealers: same keys as Channel internals, independent counters.
    // Client sends c2s (ClientToServer); agent sends s2c (ServerToClient).
    InjectSealer c2sInject{CloneKey(kKeys.ClientToServer),
                           kKeys.SaltClientToServer};
    InjectSealer s2cInject{CloneKey(kKeys.ServerToClient),
                           kKeys.SaltServerToClient};

    // Stream records for P4.
    StreamRecord clientRecvRec;  // s2c direction: what Client.Receive() yields
    StreamRecord agentRecvRec;   // c2s direction: what Agent.Receive() yields

    // Working buffers for Poll() and Receive().
    // Static: together over half a megabyte, too much for the stack of a
    // fuzzer thread. Poll overwrites what it uses, so reuse across runs is
    // harmless.
    static std::array<std::byte, wire::HEADER_SIZE + wire::MAX_FRAME> pollOut{};
    static std::array<std::byte, Channel::POLL_SCRATCH_SIZE> pollScratch{};
    std::vector<std::byte> recvScratch(wire::MAX_FRAME);

    // Reference stream cursor: absolute position in the deterministic client
    // send stream (used to fill AppendClient bytes so agentRecvRec can be
    // checked).
    std::uint64_t clientSendRef{0};
    std::uint64_t agentSendRef{0};

    zet::ByteReader reader{
        ByteSpan{reinterpret_cast<const std::byte*>(data), size}};

    std::size_t ops = 0;
    const std::size_t budget = std::min(size / 2 + 1, MAX_OPS);

    while (ops < budget && !reader.Exhausted()) {
        ++ops;

        const auto opByte = reader.ReadU8();
        if (!opByte) {
            break;
        }
        const std::uint8_t op = static_cast<std::uint8_t>(*opByte & 0x07U);

        // Snapshot before the operation for monotonicity checks (P1–P3, P5).
        const Snapshot before{clientRecvCursor.RecvOffset(),
                              agentRecvCursor.RecvOffset(),
                              clientSendBuf.Base(), agentSendBuf.Base()};

        switch (op) {
            // ── 00: AppendClient ─────────────────────────────────────────
            case 0x00: {
                const auto lenByte = reader.ReadU8();
                if (!lenByte) {
                    break;
                }
                const std::size_t want =
                    static_cast<std::size_t>(*lenByte % MAX_APPEND_LEN) + 1;
                std::array<std::byte, MAX_APPEND_LEN> buf{};
                for (std::size_t i = 0; i < want; ++i) {
                    buf[i] = RefByte(clientSendRef + i);
                }
                const std::size_t taken =
                    clientSendBuf.Append(ByteSpan{buf}.first(want));
                clientSendRef += taken;
                break;
            }
            // ── 01: AppendAgent ──────────────────────────────────────────
            case 0x01: {
                const auto lenByte = reader.ReadU8();
                if (!lenByte) {
                    break;
                }
                const std::size_t want =
                    static_cast<std::size_t>(*lenByte % MAX_APPEND_LEN) + 1;
                std::array<std::byte, MAX_APPEND_LEN> buf{};
                for (std::size_t i = 0; i < want; ++i) {
                    buf[i] = RefByte(agentSendRef + i);
                }
                const std::size_t taken =
                    agentSendBuf.Append(ByteSpan{buf}.first(want));
                agentSendRef += taken;
                break;
            }
            // ── 02: PollClient ───────────────────────────────────────────
            case 0x02: {
                const auto deltaByte = reader.ReadU8();
                if (!deltaByte) {
                    break;
                }
                now += std::chrono::milliseconds{
                    static_cast<long long>(*deltaByte)};
                const auto n = clientChan.Poll(now, MutableByteSpan{pollOut},
                                               MutableByteSpan{pollScratch});
                if (n) {
                    // P7: return value in [0, HEADER_SIZE + MAX_FRAME].
                    if (*n > wire::HEADER_SIZE + wire::MAX_FRAME) {
                        __builtin_trap();
                    }
                }
                // Errors from Poll (e.g., EpochsExhausted) are CloseConnection
                // per channel_error.hpp; both dispositions are acceptable here.
                break;
            }
            // ── 03: PollAgent ────────────────────────────────────────────
            case 0x03: {
                const auto deltaByte = reader.ReadU8();
                if (!deltaByte) {
                    break;
                }
                now += std::chrono::milliseconds{
                    static_cast<long long>(*deltaByte)};
                const auto n = agentChan.Poll(now, MutableByteSpan{pollOut},
                                              MutableByteSpan{pollScratch});
                if (n) {
                    if (*n > wire::HEADER_SIZE + wire::MAX_FRAME) {
                        __builtin_trap();
                    }
                }
                break;
            }
            // ── 04: InjectToClient ───────────────────────────────────────
            case 0x04: {
                const auto lenByte = reader.ReadU8();
                if (!lenByte) {
                    break;
                }
                const std::size_t want =
                    static_cast<std::size_t>(*lenByte % MAX_INJECT_LEN) + 1;
                const auto payload =
                    reader.ReadBytes(std::min(want, reader.Remaining()));
                if (!payload) {
                    break;
                }
                DoInject(s2cInject, clientChan, *payload, clientRecvRec,
                         clientRecvCursor, recvScratch);
                break;
            }
            // ── 05: InjectToAgent ────────────────────────────────────────
            case 0x05: {
                const auto lenByte = reader.ReadU8();
                if (!lenByte) {
                    break;
                }
                const std::size_t want =
                    static_cast<std::size_t>(*lenByte % MAX_INJECT_LEN) + 1;
                const auto payload =
                    reader.ReadBytes(std::min(want, reader.Remaining()));
                if (!payload) {
                    break;
                }
                DoInject(c2sInject, agentChan, *payload, agentRecvRec,
                         agentRecvCursor, recvScratch);
                break;
            }
            // ── 06: QueueResize ──────────────────────────────────────────
            case 0x06: {
                const auto rowByte = reader.ReadU8();
                const auto colByte = reader.ReadU8();
                if (!rowByte || !colByte) {
                    break;
                }
                clientChan.QueueResize(wire::ResizeMessage{
                    .Rows = static_cast<std::uint16_t>(*rowByte + 1U),
                    .Cols = static_cast<std::uint16_t>(*colByte + 1U),
                    .XPixel = 0,
                    .YPixel = 0,
                });
                break;
            }
            // ── 07: AdvanceTime ──────────────────────────────────────────
            case 0x07: {
                const auto deltaByte = reader.ReadU8();
                if (!deltaByte) {
                    break;
                }
                // 100 ms steps: a few steps cross ACK_IDLE_DELAY (200 ms) and
                // many steps cross REKEY_INTERVAL (1 h).
                now += std::chrono::milliseconds{static_cast<int>(*deltaByte) *
                                                 100};
                break;
            }
            default:
                break;
        }

        CheckProperties(before, clientSendBuf, agentSendBuf, clientRecvCursor,
                        agentRecvCursor);
    }

    return 0;
}
