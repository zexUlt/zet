#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/receive_cursor.hpp"
#include "zet/session/send_buffer.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"
#include "zet/wire/sealed_frame.hpp"

namespace zet::session {

/// Which direction this Channel reads from and writes to.
///
/// The names match §3: the client's "outgoing" key encrypts the c→s direction,
/// and the agent's "outgoing" key encrypts the s→c direction. Role determines
/// which of ConnectionKeys.{ClientToServer,ServerToClient} is the sealer and
/// which is the opener.
enum class ERole : std::uint8_t {
    Client,
    Agent,
};

/// What the Channel delivered from one incoming frame, per §7.
///
/// A variant rather than an out-parameter per result type keeps the call site
/// exhaustive: the caller switch is on the type, so a new result kind added
/// here causes a compile error at every site that does not handle it.
struct DeliveredData {
    /// A sub-span into the caller's scratch buffer that was passed to Receive.
    /// No copy beyond the decrypt; valid until the next call on the channel.
    ByteSpan Bytes;
    /// The byte offset of the first byte in Bytes within the stream.
    std::uint64_t Offset{0};
};

struct DeliveredResize {
    wire::ResizeMessage Resize;
};

/// The agent's shell is gone and the client holds all it wrote. Client only.
struct DeliveredSessionEnd {
    wire::SessionEndMessage End;
};

struct DeliveredNothing {};

/// A complete grid snapshot has been received and validated. Client only.
///
/// `Bytes` points into the Channel's internal accumulation buffer and is
/// valid until the next Receive or Poll call. The caller must apply the
/// snapshot atomically and then call QueueGridApplied() before returning.
/// The Channel resets RecvCursor to 0 before delivering this event. §9.
struct DeliveredGridSnapshot {
    /// The raw snapshot bytes (scrollback prefix + screen).
    ByteSpan Bytes;
    std::uint32_t StreamGen{0};
    std::uint32_t ScrollbackLen{0};
    std::uint8_t Flags{0};
};

/// The client applied the grid snapshot. Agent only.
///
/// On receipt, the agent exits the GridAwaitApplied window and begins
/// sending Data from offset 0 of the new generation. §9.
struct DeliveredGridApplied {
    std::uint32_t StreamGen{0};
};

/// A validated EchoAck from the agent. Client only (§7, §9).
///
/// The fields were checked against the stream: the generation is the one Data
/// is received in, the output offset is no further than the client received
/// there, the input offset no further than it wrote, and neither went back
/// since the previous EchoAck of this connection.
struct DeliveredEchoAck {
    wire::EchoAckMessage Ack;
};

using IncomingEvent =
    std::variant<DeliveredData, DeliveredResize, DeliveredSessionEnd,
                 DeliveredGridSnapshot, DeliveredGridApplied, DeliveredEchoAck,
                 DeliveredNothing>;

/// The sealed, post-authentication traffic of one connection, symmetric for
/// client and agent. §9.
///
/// The Channel borrows a SendBuffer and a ReceiveCursor from the session, which
/// owns them for the life of the session; the Channel only exists for the
/// lifetime of one connection. The same session can create a new Channel on
/// reconnect using the same buffer and cursor, picking up exactly where it
/// left off.
///
/// **Incoming (Receive):** opens one FrameView into caller-provided scratch,
/// parses the message type, updates stream state, and returns what happened.
/// The caller keeps the scratch buffer alive until the next call; the returned
/// DeliveredData.Bytes is a sub-span of it.
///
/// **Outgoing (Poll):** writes one sealed frame into the caller's `out` buffer
/// and returns how many bytes were written. Zero means nothing to send right
/// now. `scratch` is used to stage the plaintext for Data frames — it must be
/// at least POLL_SCRATCH_SIZE bytes. Neither `out` nor `scratch` is retained
/// between calls; they may be stack or heap as the caller sees fit.
///
/// **NextDeadline:** reports the next time-driven event (idle Ack, time-based
/// rekey, keepalive, peer silence) so the event loop can set a real timer
/// instead of polling.
///
/// **Liveness (§9):** a Ping goes out once nothing has been sent for
/// KeepaliveInterval, and Poll fails with PeerSilent once nothing at all —
/// not a Data, not a Pong — has opened for PeerSilenceTimeout. That is how a
/// path that drops packets without a reset ends in seconds rather than in
/// TCP's quarter of an hour.
///
/// Memory bound: the channel never reads past SendBuffer::Room() == 0 (§9
/// backpressure). The caller is responsible for filling the buffer; the channel
/// is responsible for not sending what the buffer does not contain.
/// The two halves of one connection's sealed traffic as seen from one end:
/// what it seals with and what it opens with.
struct DirectionFrames {
    wire::FrameSealer Sealer;
    wire::FrameOpener Opener;
    /// Which end these are for: some messages go one way only.
    ERole Role{ERole::Client};
};

/// Builds both halves for `role` from the connection keys, counters at zero.
///
/// The connection, not the Channel, is where these are first used: Resume is
/// the client's first sealed frame and opens on the agent before any Channel
/// exists. Both objects are then moved into the Channel with their counters
/// where Resume left them — building the Channel from the keys instead would
/// seal its first frame under the nonce Resume already used.
[[nodiscard]] DirectionFrames MakeDirectionFrames(
    const wire::ConnectionKeys& keys, ERole role,
    std::uint64_t frameLimit = wire::MAX_FRAMES_PER_EPOCH,
    std::uint64_t byteLimit = wire::MAX_BYTES_PER_EPOCH) noexcept;

class Channel {
public:
    /// Minimum scratch size for the Poll call. One full-size Data plaintext:
    /// type(1) + ack:u64(8) + offset:u64(8) + payload(MAX_DATA_PAYLOAD).
    static constexpr std::size_t POLL_SCRATCH_SIZE =
        1 + 2 * sizeof(std::uint64_t) + wire::MAX_DATA_PAYLOAD;

    /// Takes over the sealer and opener a connection has already used.
    ///
    /// `sendBuf` and `recvCursor` are borrowed; they must outlive this object.
    /// `now` anchors the epoch timer for time-triggered rekey (§8).
    /// `liveness` sets the keepalive and peer-silence timers; both run from
    /// `now`. `version` is the negotiated protocol version; it gates ver=2-only
    /// messages such as SessionEnd Overflow.
    /// `snapshotBuf` is the caller-supplied storage for incoming grid snapshots
    /// (§1 forbids allocation in the core; the connection layer provides the
    /// buffer). Must be at least wire::SNAPSHOT_MAX_BYTES for client-role
    /// channels; agent-role channels never receive snapshots and may pass {}.
    /// `agentStreamGen` seeds the generation baseline for GridBegin validation
    /// on the client side: the first GridBegin received must have
    /// stream_gen > agentStreamGen. Pass Session_.StreamGen() on reconnect so
    /// a forged GridBegin from a previous generation is rejected. §9 item (4).
    Channel(DirectionFrames frames, SendBuffer& sendBuf,
            ReceiveCursor& recvCursor, Timestamp now, LivenessLimits liveness,
            std::uint16_t version, MutableByteSpan snapshotBuf = {},
            std::uint32_t agentStreamGen = 0) noexcept;

    /// Builds a Channel straight from connection keys, counters at zero.
    ///
    /// Valid only for keys nothing has been sealed under yet, which is never
    /// true of a real connection — Resume comes first. It exists for tests and
    /// fuzzers that drive a Channel without one; a connection uses the
    /// constructor above. `frameLimit` and `byteLimit` let tests shorten the
    /// rekey threshold; the defaults are the values from §8.
    Channel(const wire::ConnectionKeys& keys, ERole role, SendBuffer& sendBuf,
            ReceiveCursor& recvCursor, Timestamp now,
            std::uint64_t frameLimit = wire::MAX_FRAMES_PER_EPOCH,
            std::uint64_t byteLimit = wire::MAX_BYTES_PER_EPOCH,
            LivenessLimits liveness = {},
            std::uint16_t version = wire::PROTOCOL_VERSION_MIN,
            MutableByteSpan snapshotBuf = {},
            std::uint32_t agentStreamGen = 0) noexcept;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;
    ~Channel() = default;

    /// Process one incoming sealed frame.
    ///
    /// `scratch` must be at least wire::MAX_FRAME bytes; it is used as the
    /// AEAD output buffer. DeliveredData.Bytes is a sub-span of it and is
    /// valid until the next Receive() or Poll() call.
    ///
    /// Errors carry a disposition (CloseConnection or KillSession). The channel
    /// makes no state changes on error: a rejected frame does not move any
    /// cursor or acknowledge anything. A frame that opens, whatever it says,
    /// counts as hearing from the peer at `now`.
    [[nodiscard]] ChannelResult<IncomingEvent> Receive(
        Timestamp now, const wire::FrameView& frame,
        MutableByteSpan scratch) noexcept;

    /// Write one outgoing sealed frame into `out` and return its length.
    ///
    /// Returns 0 when there is nothing to send right now.
    ///
    /// `out` must be at least wire::HEADER_SIZE + wire::MAX_FRAME bytes.
    /// `scratch` must be at least POLL_SCRATCH_SIZE bytes and is used to stage
    /// the plaintext for Data frames (§1 forbids internal allocation in the
    /// protocol core; the caller provides storage instead).
    ///
    /// Fails with PeerSilent, before writing anything, once nothing has been
    /// heard for PeerSilenceTimeout.
    ///
    /// Priority: Pong > EchoAck > Data (with piggybacked Ack) > SessionEnd >
    /// Resize > bare Ack > keepalive Ping. §9: "кумулятивные ACK ... пиггибэком
    /// в заголовке plaintext каждого кадра данных плюс отдельным Ack при
    /// простое дольше 200 мс".
    [[nodiscard]] ChannelResult<std::size_t> Poll(
        Timestamp now, MutableByteSpan out, MutableByteSpan scratch) noexcept;

    /// The next time-driven event, for the event loop's timer queue.
    ///
    /// Returns the earlier of:
    ///   - the idle-Ack deadline (ACK_IDLE_DELAY after the first Poll() that
    ///     saw un-acked bytes and had nothing to send);
    ///   - the time-rekey deadline (REKEY_INTERVAL after the epoch began, §8);
    ///   - the keepalive (KeepaliveInterval after the last frame sent);
    ///   - the peer-silence limit (PeerSilenceTimeout after the last frame
    ///     heard), at which Poll fails.
    ///
    /// Returns an unset Deadline when nothing is pending.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    /// Tells the channel whether its owner is reading the socket. A caller that
    /// stops reading because its own consumer is full (§9 backpressure) closes
    /// the gate: while it is closed the peer cannot be heard, so Poll does not
    /// fail with PeerSilent and NextDeadline leaves the silence limit out.
    /// Reopening restarts the silence timer from `now`. Idempotent; keepalives
    /// and every other timer are unaffected.
    void SetReadGate(bool open, Timestamp now) noexcept;

    /// Tells the channel whether its owner has room for another frame. A
    /// caller whose socket queue is full closes the gate: while it is closed
    /// Poll sends nothing (it still enforces the silence limit) and
    /// NextDeadline leaves out the send timers, which could not be served.
    /// This is how a replay larger than the owner's queue goes out as the
    /// socket drains (§9). Idempotent; the owner reopens it once the queue has
    /// room and polls again.
    void SetSendGate(bool open) noexcept { SendGateClosed_ = !open; }

    /// Queue a Resize to be sent on the next Poll. Only meaningful on the
    /// client side; calling it from the agent side is harmless — the other side
    /// will apply the dimensions or ignore them. Idempotent: a later call
    /// replaces an unanswered one. §9.
    void QueueResize(wire::ResizeMessage resize) noexcept;

    /// Has SessionEnd go out once the outgoing stream is fully acknowledged:
    /// nothing unsent, nothing unacknowledged, so it can never overtake the
    /// shell's last bytes (§9). Agent only. Once per channel — later calls
    /// change nothing; the next connection's channel sends it again.
    void QueueSessionEnd(wire::SessionEndMessage end) noexcept;

    /// Whether this channel has sent the SessionEnd it was given.
    [[nodiscard]] bool SessionEndSent() const noexcept { return EndSent_; }

    /// How many post-auth frames with unknown types have been received.
    /// Counted rather than errored per §7: "unknown post-auth type → ignored
    /// and counted".
    [[nodiscard]] std::uint64_t UnknownTypeCount() const noexcept {
        return UnknownTypeCount_;
    }

    // ── M6: grid snapshot exchange (§9 ADR-030). ───────────────────────────

    /// Agent side: begin emitting a grid snapshot in the current output
    /// generation. `snapshot` is borrowed; the caller must keep it alive
    /// until `GridEmissionDone()` returns true. `streamGen` is the new
    /// session_gen from NewGeneration(); `scrollbackLen` and `flags` come
    /// from the grid serializer. §9.
    ///
    /// Returns true when the grid emission was started, false when it was
    /// rejected (wrong role, emission already in progress, empty snapshot, or
    /// snapshot exceeds SNAPSHOT_MAX_BYTES). Poll will send
    /// GridBegin → GridChunk* → GridEnd on subsequent calls.
    [[nodiscard]] bool StartGrid(ByteSpan snapshot, std::uint8_t flags,
                                 std::uint32_t streamGen,
                                 std::uint32_t scrollbackLen) noexcept;

    /// True once GridEnd has been sent and the channel is waiting for
    /// GridApplied from the client.
    [[nodiscard]] bool GridEmissionDone() const noexcept {
        return GridAwaitingApplied_;
    }

    /// True while a grid snapshot is in progress (GridBegin sent but
    /// GridApplied not yet received). Data is suppressed in this window.
    [[nodiscard]] bool GridInFlight() const noexcept {
        return GridEmitting_.has_value() || GridAwaitingApplied_;
    }

    /// Client side: queue a GridApplied to be sent on the next Poll, after
    /// the caller has applied the snapshot delivered as DeliveredGridSnapshot.
    /// `streamGen` is the StreamGen from the snapshot event.
    void QueueGridApplied(std::uint32_t streamGen) noexcept;

    /// Agent side: suppress outgoing Data until the next StartGrid call.
    ///
    /// Called once after Channel creation on path B, before SupplySnapshot
    /// has cleared the ring.  Without this guard, Poll would send pre-snapshot
    /// bytes that belong to the old generation.  StartGrid clears the flag.
    void SuppressDataUntilGrid() noexcept;

    // ── M7: echo-ack (§7, §9 ADR-036). ─────────────────────────────────────

    /// Records that this connection's Hello carried ECHO_ACK: the client sent
    /// it, or the agent read it from the authenticated Hello. Without it an
    /// EchoAck is a violation on the client and unsendable on the agent. Only
    /// ver=2 counts, whatever was called here.
    void EnableEchoAck() noexcept { EchoAckEnabled_ = true; }

    /// Lets the client add a Ping to the input it sends when the last RTT
    /// sample is old (RTT_PROBE_INTERVAL). Off by default, so a connection
    /// that does not use the figure sends the frames it always did; at ver=1
    /// it stays off whatever was called. Client only; the agent never probes.
    void EnableRttProbe() noexcept { RttProbeEnabled_ = true; }

    /// Whether EchoAck may be exchanged on this connection.
    [[nodiscard]] bool EchoAckNegotiated() const noexcept {
        return EchoAckEnabled_ && Version_ >= wire::PROTOCOL_VERSION_GRID;
    }

    /// Client side, read-only views of what the EchoAck checks compare
    /// against, for tests and fuzzers: the previous accepted EchoAck, the
    /// generation Data is received in, and whether a snapshot is mid-reception.
    [[nodiscard]] const std::optional<wire::EchoAckMessage>& LastEchoAck()
        const noexcept {
        return LastEchoAck_;
    }
    [[nodiscard]] std::uint32_t AgentStreamGen() const noexcept {
        return AgentStreamGen_;
    }
    [[nodiscard]] bool GridReceiving() const noexcept {
        return GridReceiving_.has_value();
    }

    /// Agent side: queue an EchoAck for the next Poll. A later call replaces
    /// one not yet sent. Poll holds it back while a snapshot is being emitted
    /// or awaited from SupplySnapshot, and sends it ahead of Data. Whether it
    /// is due (the send reached `OutputOffset`, the rate limit) is the
    /// caller's rule, not the channel's. Returns false, queueing nothing, when
    /// the role or the negotiation does not allow one.
    [[nodiscard]] bool QueueEchoAck(const wire::EchoAckMessage& ack) noexcept;

    // ── M7: round-trip time (§9, «RTT», ADR-038). ──────────────────────────

    /// The smoothed round trip of this connection's Ping/Pong exchanges, per
    /// RFC 6298 (alpha = 1/8, the first sample taken as it is). Unset until a
    /// Pong has answered a Ping of this channel within RTT_SAMPLE_MAX. Only
    /// the owner's reading of the figure; nothing on the wire depends on it.
    [[nodiscard]] std::optional<Duration> Srtt() const noexcept {
        return Srtt_;
    }

private:
    /// Poll without the liveness bookkeeping around it.
    [[nodiscard]] ChannelResult<std::size_t> PollOnce(
        Timestamp now, MutableByteSpan out, MutableByteSpan scratch) noexcept;

    /// Seals a fixed-size control message built by `write` into `out`.
    template <std::size_t tSize, typename TWrite>
    [[nodiscard]] ChannelResult<std::size_t> SealControl(
        MutableByteSpan out, TWrite&& write) noexcept;

    /// Rekey if the sealer reports NeedsRekey() or REKEY_INTERVAL has elapsed
    /// since the epoch began. Returns false only on EpochsExhausted — the
    /// connection cannot send another frame and must be closed. §8.
    [[nodiscard]] ChannelResult<void> RekeyIfNeeded(Timestamp now) noexcept;

    /// Seal `plaintext` into `out`. Returns the byte count of the sealed frame
    /// (header + ciphertext + tag), or 0 on any error (buffer too small,
    /// epochs exhausted).
    [[nodiscard]] ChannelResult<std::size_t> SealInto(
        MutableByteSpan out, ByteSpan plaintext) noexcept;

    SendBuffer& SendBuf_;
    ReceiveCursor& RecvCursor_;

    /// When the current epoch began, for the time-triggered rekey. §8.
    Timestamp EpochStart_;

    /// The receive offset we last sent to the peer, either piggybacked on a
    /// Data frame or in a bare Ack. Used to decide whether a bare Ack is due.
    std::uint64_t LastAckedOffset_{0};

    std::uint64_t UnknownTypeCount_{0};

    /// When a frame last opened, and when one last went out. §9 liveness.
    Timestamp LastHeard_;
    Timestamp LastSent_;
    /// The owner has stopped reading the socket; the silence timer is off.
    bool ReadGateClosed_{false};
    /// The owner has no room for a frame; Poll sends nothing.
    bool SendGateClosed_{false};
    /// The token of the next Ping. Its value means nothing to the peer, which
    /// only echoes it.
    std::uint64_t NextPingToken_{1};

    // ── M7 round-trip measurement. §9. ─────────────────────────────────────
    /// The one Ping of this channel that no Pong has answered: a later Ping
    /// replaces it, and a Pong with another token measures nothing.
    struct OutstandingPing {
        std::uint64_t Token{0};
        Timestamp SentAt;
    };
    std::optional<OutstandingPing> OutstandingPing_;
    std::optional<Duration> Srtt_;
    std::optional<Timestamp> LastRttSampleAt_;
    bool RttProbeEnabled_{false};

    /// Whether a Ping should ride along with the Data about to go out: the
    /// client's own probe, so that samples keep coming while the user types.
    [[nodiscard]] bool RttProbeDue(Timestamp now) const noexcept;
    /// Seals a Ping with the next token and remembers it as outstanding.
    [[nodiscard]] ChannelResult<std::size_t> SealPing(
        Timestamp now, MutableByteSpan out) noexcept;

    LivenessLimits Liveness_;

    /// When Poll first noticed un-acked receive bytes with nothing else to
    /// send. std::nullopt means no idle-Ack is pending.
    std::optional<Timestamp> AckPendingSince_;

    /// A Pong we owe the peer, set by Receive on a Ping. Idempotent: a new
    /// Ping replaces an unanswered one, per §9.
    std::optional<wire::PongMessage> PendingPong_;

    /// Caller-supplied storage for incoming grid snapshots. Borrowed; must
    /// outlive this Channel. Agent-role channels pass {} (never receive). §1.
    MutableByteSpan SnapshotBuf_;

    // ── M6 grid state (§9 ADR-030) ─────────────────────────────────────────

    // ── Agent side (emitting GridBegin → GridChunk* → GridEnd):
    struct GridEmission {
        ByteSpan Snapshot;  ///< Borrowed; caller keeps alive.
        std::uint32_t StreamGen{0};
        std::uint32_t TotalLen{0};
        std::uint32_t ScrollbackLen{0};
        std::uint8_t Flags{0};
        std::uint32_t BytesSent{0};  ///< Cumulative bytes sent via GridChunk.
        bool BeginSent{false};
    };
    std::optional<GridEmission> GridEmitting_;

    // ── Client side (receiving GridBegin → GridChunk* → GridEnd):
    struct GridReception {
        std::uint32_t StreamGen{0};
        std::uint32_t TotalLen{0};
        std::uint32_t ScrollbackLen{0};
        std::uint8_t Flags{0};
        std::uint32_t BytesReceived{0};
        // Bytes are written directly into SnapshotBuf_; no per-reception
        // buffer.
    };
    std::optional<GridReception> GridReceiving_;

    wire::FrameSealer Sealer_;
    wire::FrameOpener Opener_;

    /// The last successfully applied stream generation (validated GridEnd).
    std::uint32_t AgentStreamGen_{0};

    /// GridApplied to send on the next Poll, set by QueueGridApplied. §9.
    std::optional<std::uint32_t> PendingGridApplied_;

    std::uint16_t Version_{wire::PROTOCOL_VERSION_MIN};

    /// ECHO_ACK was in this connection's Hello. See EnableEchoAck. §7.
    bool EchoAckEnabled_{false};
    /// Client side: the previous EchoAck of this connection, which the next
    /// one is checked against. §7.
    std::optional<wire::EchoAckMessage> LastEchoAck_;
    /// Agent side: the EchoAck to send on the next Poll. §9.
    std::optional<wire::EchoAckMessage> PendingEchoAck_;

    /// A Resize queued by QueueResize, to be sent on the next Poll. Replaces
    /// any previous value that has not yet been sent. §9.
    std::optional<wire::ResizeMessage> PendingResize_;

    ERole Role_;
    bool EndSent_{false};
    /// True after GridEnd is sent; cleared when GridApplied is received.
    bool GridAwaitingApplied_{false};
    /// The stream generation the agent expects to see in the next GridApplied.
    /// Set by StartGrid; validated when GridApplied arrives. §9.
    std::uint32_t GridExpectedAppliedGen_{0};
    /// Set by SuppressDataUntilGrid(); cleared by StartGrid(). Prevents Poll
    /// from sending pre-NewGeneration bytes between AttachPathB and
    /// SupplySnapshot. §9 ADR-030.
    bool AwaitingSnapshotSupply_{false};

    /// The SessionEnd to send once the stream is acknowledged, and whether it
    /// has gone.
    std::optional<wire::SessionEndMessage> PendingEnd_;
};

}  // namespace zet::session
