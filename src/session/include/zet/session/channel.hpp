#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

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

using IncomingEvent =
    std::variant<DeliveredData, DeliveredResize, DeliveredSessionEnd,
                 DeliveredGridSnapshot, DeliveredGridApplied, DeliveredNothing>;

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
    Channel(DirectionFrames frames, SendBuffer& sendBuf,
            ReceiveCursor& recvCursor, Timestamp now, LivenessLimits liveness,
            std::uint16_t version) noexcept;

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
            std::uint16_t version = wire::PROTOCOL_VERSION_MIN) noexcept;

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
    /// Priority: Pong > Data (with piggybacked Ack) > SessionEnd > Resize >
    /// bare Ack > keepalive Ping.
    /// §9: "кумулятивные ACK ... пиггибэком в заголовке plaintext каждого
    /// кадра данных плюс отдельным Ack при простое дольше 200 мс".
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
    /// No-op if not the agent role or if a grid emission is already in
    /// progress. Poll will send GridBegin → GridChunk* → GridEnd on
    /// subsequent calls.
    void StartGrid(ByteSpan snapshot, std::uint8_t flags,
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
    /// The token of the next keepalive Ping. Its value means nothing to the
    /// peer, which only echoes it.
    std::uint64_t NextPingToken_{1};

    LivenessLimits Liveness_;

    /// When Poll first noticed un-acked receive bytes with nothing else to
    /// send. std::nullopt means no idle-Ack is pending.
    std::optional<Timestamp> AckPendingSince_;

    /// A Pong we owe the peer, set by Receive on a Ping. Idempotent: a new
    /// Ping replaces an unanswered one, per §9.
    std::optional<wire::PongMessage> PendingPong_;

    /// Completed snapshot buffer, kept alive until the next Receive/Poll so
    /// that the DeliveredGridSnapshot.Bytes span remains valid. §9.
    std::vector<std::byte> GridSnapshotBuf_;

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
        bool EndSent{false};
    };
    std::optional<GridEmission> GridEmitting_;

    // ── Client side (receiving GridBegin → GridChunk* → GridEnd):
    struct GridReception {
        std::uint32_t StreamGen{0};
        std::uint32_t TotalLen{0};
        std::uint32_t ScrollbackLen{0};
        std::uint8_t Flags{0};
        std::uint32_t BytesReceived{0};
        std::vector<std::byte> Buffer;  ///< Accumulated chunk bytes.
    };
    std::optional<GridReception> GridReceiving_;

    wire::FrameSealer Sealer_;
    wire::FrameOpener Opener_;

    /// The last successfully applied stream generation (validated GridEnd).
    std::uint32_t AgentStreamGen_{0};

    /// GridApplied to send on the next Poll, set by QueueGridApplied. §9.
    std::optional<std::uint32_t> PendingGridApplied_;

    std::uint16_t Version_{wire::PROTOCOL_VERSION_MIN};

    /// A Resize queued by QueueResize, to be sent on the next Poll. Replaces
    /// any previous value that has not yet been sent. §9.
    std::optional<wire::ResizeMessage> PendingResize_;

    ERole Role_;
    bool EndSent_{false};
    /// True after GridEnd is sent; cleared when GridApplied is received.
    bool GridAwaitingApplied_{false};

    /// The SessionEnd to send once the stream is acknowledged, and whether it
    /// has gone.
    std::optional<wire::SessionEndMessage> PendingEnd_;
};

}  // namespace zet::session
