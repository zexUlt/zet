#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/channel.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/client_session.hpp"
#include "zet/wire/frame_stream.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/limits.hpp"

/// One TCP connection of a session, from the first byte to the last, on either
/// end, per docs/design.md §5 and §9.
///
///     agent:  Hello ▶ route by sid ▶ Challenge … AuthOk ▶ Resume opens ▶
///     Channel client: Hello ……………………………………………▶ Auth ▶ AuthOk ▶ Resume ▶
///     Channel
///
/// Sans-I/O like everything under it. Bytes come in through OnBytes, frames go
/// out through Pump into a buffer the caller provides, and what happened in
/// between comes back as an event. Time and nonces are handed in: the clock
/// and the generator are the two things §16 lets a test replace.
///
/// Neither class does the budget of §5 beyond its own deadline — failed Auth
/// counts per sid, caps per IP — for the reason the handshake does not: those
/// span connections, and live with whoever holds the table of them.
namespace zet::session {

/// Room a connection needs to reassemble the largest frame there is.
inline constexpr std::size_t CONNECTION_FRAME_STORAGE =
    wire::HEADER_SIZE + wire::MAX_FRAME;

/// Room Pump needs in `out` to write the largest frame there is.
inline constexpr std::size_t CONNECTION_OUT_SIZE =
    wire::HEADER_SIZE + wire::MAX_FRAME;

/// Room a connection's queue needs for the largest frame Pump emits: header
/// and a body of SEND_FRAME_MAX. The owners' send gate opens at this, not at
/// CONNECTION_OUT_SIZE, which is only the width of the buffer Pump writes into
/// and covers the receive limit. §9.
inline constexpr std::size_t CONNECTION_SEND_ROOM =
    wire::HEADER_SIZE + wire::SEND_FRAME_MAX;

static_assert(CONNECTION_SEND_ROOM <= CONNECTION_OUT_SIZE);

/// The owners' send gate: whether a queue with `room` bytes free can take the
/// largest frame Pump emits. Both Pumps feed SetSendGate with this. §9.
[[nodiscard]] constexpr bool HasSendRoom(std::size_t room) noexcept {
    return room >= CONNECTION_SEND_ROOM;
}

/// Room Pump needs in `scratch`: the plaintext of the largest frame, which is
/// also more than the widest Data a Channel builds.
inline constexpr std::size_t CONNECTION_SCRATCH_SIZE = wire::MAX_FRAME;

static_assert(CONNECTION_SCRATCH_SIZE >= Channel::POLL_SCRATCH_SIZE);

/// The agent read a Hello and needs the session it names. The caller looks
/// the sid up and answers with Route or RouteUnknown before pumping again.
struct NeedsSession {
    wire::SessionId Id{};
};

/// `Written` bytes at the front of `out` are a frame for the socket.
struct SendFrame {
    std::size_t Written{0};
};

/// Stream bytes for the local side: the pty on the agent, the terminal on the
/// client. They point into `scratch` and are good until the next Pump.
struct Deliver {
    ByteSpan Bytes;
};

/// The client's terminal changed size. Agent only.
struct Resized {
    wire::ResizeMessage Size;
};

/// The agent's shell has ended, and the client holds every byte it wrote.
/// Client only; the session has recorded it too (ClientSession::EndedWith),
/// so the caller restores the terminal, closes the connection and exits
/// rather than reconnecting.
struct Ended {
    wire::SessionEndMessage End;
};

/// The agent's ring overflowed while the client did not have GRID_CAPABLE
/// (ver=2 only, §7). Client only. The session is dead; the caller does not
/// reconnect. Distinct from Ended so callers can display a targeted message
/// rather than mapping it to a signal-exit code.
struct SessionOverflowed {};

/// Nothing to do until more bytes arrive or NextDeadline passes.
struct Idle {};

/// The agent needs a grid snapshot to start path B on reconnect. Agent only.
/// The caller builds a snapshot of size {Rows×Cols} and delivers it via
/// SupplySnapshot before the next Pump. §9 ADR-030.
struct NeedsSnapshot {
    std::uint16_t Rows{0};
    std::uint16_t Cols{0};
};

/// A complete grid snapshot has been received and is ready to apply. Client
/// only. `Bytes` is valid until the next Pump call. §9 ADR-030.
struct ApplySnapshot {
    ByteSpan Bytes;
    std::uint32_t StreamGen{0};
    std::uint32_t ScrollbackLen{0};
    std::uint8_t Flags{0};
};

/// A validated EchoAck arrived (§7, §9). Client only; the checks against the
/// stream are the channel's (DeliveredEchoAck).
struct EchoAcked {
    wire::EchoAckMessage Ack;
};

using ConnectionEvent =
    std::variant<NeedsSession, SendFrame, Deliver, Resized, Ended, Idle,
                 NeedsSnapshot, ApplySnapshot, SessionOverflowed, EchoAcked>;

using ConnectionResult = ChannelResult<ConnectionEvent>;

/// What both ends do alike: reassemble frames, keep a deadline, remember the
/// first error. Not used on its own.
class ConnectionBase {
public:
    ConnectionBase(const ConnectionBase&) = delete;
    ConnectionBase& operator=(const ConnectionBase&) = delete;
    ConnectionBase(ConnectionBase&&) = delete;
    ConnectionBase& operator=(ConnectionBase&&) = delete;

    /// Takes bytes off the socket. A refusal here is the frame layer's, and
    /// ends the connection like any other.
    [[nodiscard]] ChannelResult<void> OnBytes(ByteSpan chunk) noexcept;

    /// Whether Resume has gone through and the session's streams flow.
    [[nodiscard]] bool IsLive() const noexcept { return Channel_.has_value(); }

    /// What the next OnBytes will still take. Reading at most this much off
    /// the socket keeps a caller out of the error path; zero means Pump has
    /// whole frames to work through first.
    [[nodiscard]] std::size_t ReadRoom() const noexcept {
        return Frames_.Room();
    }

    /// The most OnBytes ever holds: one frame of the current stage. Whatever
    /// Pump delivers out of one read is never more than this.
    [[nodiscard]] std::size_t ReadCapacity() const noexcept {
        return Frames_.Capacity();
    }

    /// When Pump next has something to do on its own: the handshake deadline
    /// until the connection is live, the channel's afterwards.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    /// The smoothed round trip of this connection (Channel::Srtt), unset
    /// until the first sample or before the connection is live. Read-only: the
    /// client's prediction engine reads it, §9 «RTT».
    [[nodiscard]] std::optional<Duration> Srtt() const noexcept {
        return Channel_ ? Channel_->Srtt() : std::nullopt;
    }

    /// Says whether the caller is reading the socket: false while it holds
    /// back because its own consumer is full, true again when it resumes. The
    /// peer-silence timer is suspended while the gate is closed and restarts
    /// at `now` on reopening (Channel::SetReadGate, §9). A call before
    /// the connection is live is dropped, there being no silence timer yet;
    /// the caller applies it again on its next pass.
    void SetReadGate(bool open, Timestamp now) noexcept;

    /// Says whether the caller has room in its socket queue for a whole frame
    /// (CONNECTION_SEND_ROOM): false makes Pump hold back every outgoing frame
    /// of a live connection, still taking incoming ones and still failing
    /// with PeerSilent; true lets them go again. The caller applies it before
    /// each Pump, so a resend or snapshot far larger than its queue goes out
    /// as the socket drains instead of overflowing the queue. A call before
    /// the connection is live is dropped; the gate starts open (Channel::
    /// SetSendGate, §9).
    void SetSendGate(bool open) noexcept;

protected:
    ConnectionBase(MutableByteSpan frameStorage, Timestamp now,
                   std::uint16_t ceiling = wire::PROTOCOL_VERSION_MAX,
                   MutableByteSpan snapshotBuf = {}) noexcept;
    ~ConnectionBase() = default;

    /// Remembers the first error and hands it back; every later Pump returns
    /// it again. A connection that failed stays failed.
    [[nodiscard]] SessionError Fail(SessionError error) noexcept;

    /// The handshake deadline, checked at the top of every Pump.
    [[nodiscard]] bool HandshakeExpired(Timestamp now) const noexcept;

    /// Refuses buffers smaller than Pump's contract. A defect in the caller,
    /// not the peer, so it is reported without failing the connection.
    [[nodiscard]] static ChannelResult<void> CheckBuffers(
        MutableByteSpan out, MutableByteSpan scratch) noexcept;

    /// Writes a cleartext frame header in front of `bodyLength` bytes that
    /// the handshake already wrote at out[HEADER_SIZE..], and returns the
    /// length of the whole frame.
    [[nodiscard]] static std::size_t FrameCleartext(
        MutableByteSpan out, std::size_t bodyLength) noexcept;

    /// The room a handshake message may be written into.
    [[nodiscard]] static MutableByteSpan HandshakeBody(
        MutableByteSpan out) noexcept;

    /// The live half, shared by both ends once Resume has gone through: one
    /// incoming frame, else one outgoing one. `onOpened` runs after a frame
    /// opened, which is where the client confirms the seed. A connection that
    /// hears nothing for the session's PeerSilenceTimeout fails here with
    /// PeerSilent (§9).
    template <typename TOnOpened, typename TOnGridSnap>
    [[nodiscard]] ConnectionResult PumpLive(Timestamp now, MutableByteSpan out,
                                            MutableByteSpan scratch,
                                            TOnOpened&& onOpened,
                                            TOnGridSnap&& onGridSnap) noexcept;

    wire::FrameStream Frames_;
    Timestamp Started_;
    std::optional<Channel> Channel_;
    std::optional<SessionError> Failed_;
    MutableByteSpan SnapshotBuf_;
    std::uint16_t NegotiatedVersion_{wire::PROTOCOL_VERSION_MIN};
    std::uint16_t Ceiling_{wire::PROTOCOL_VERSION_MAX};
};

/// The agent's end.
class AgentConnection : public ConnectionBase {
public:
    /// `frameStorage` must hold CONNECTION_FRAME_STORAGE bytes; until the peer
    /// authenticates only MAX_HANDSHAKE_FRAME of it is used.
    /// `ceiling` caps the version negotiated in Hello; default is
    /// PROTOCOL_VERSION_MAX (currently 2). Tests that exercise ver=1 pass
    /// PROTOCOL_VERSION_MIN here.
    AgentConnection(
        MutableByteSpan frameStorage, Timestamp now,
        std::uint16_t ceiling = wire::PROTOCOL_VERSION_MAX) noexcept;

    /// Answers NeedsSession for a sid that was found.
    void Route(SessionRef session, wire::HandshakeNonce serverNonce) noexcept;

    /// Answers NeedsSession for a sid that was not. What happens next looks
    /// the same on the wire, by §5 rule two, and can never end in IsLive.
    void RouteUnknown(const crypto::Key& localSecret,
                      wire::HandshakeNonce serverNonce) noexcept;

    /// Does the next thing there is to do and says what it was. Call until it
    /// returns Idle; `out` needs CONNECTION_OUT_SIZE bytes and `scratch`
    /// CONNECTION_SCRATCH_SIZE.
    ///
    /// An error carries its disposition. KillSession has already been applied
    /// to the session by the time it is returned, and only an authenticated
    /// peer can cause one: everything before tag_c is capped at
    /// CloseConnection.
    [[nodiscard]] ConnectionResult Pump(Timestamp now, MutableByteSpan out,
                                        MutableByteSpan scratch) noexcept;

    /// The socket is gone. Detaches the session if this connection still
    /// carries it.
    void Close(Timestamp now) noexcept;

    /// Test-only: direct access to the live channel. Undefined behaviour if
    /// the connection is not live (Channel_ is empty). Only call from test
    /// harnesses after the connection reaches Stage::Live.
    [[nodiscard]] Channel& ProbeChannel() noexcept { return *Channel_; }

    /// Whether this connection carries the session — live and not superseded
    /// — and has sent it SessionEnd. A client closes its end once it has
    /// that, so a close seen when this is true is the client saying it holds
    /// the whole stream; a close of any other connection says nothing.
    [[nodiscard]] bool DeliveredEnd() const noexcept;

    /// Delivers a grid snapshot after a NeedsSnapshot event. Starts the
    /// GridBegin → GridChunk* → GridEnd sequence on the next Pump and
    /// transitions to Live. `snapshot` is borrowed; the caller must keep it
    /// alive until GridEmissionDone() returns true. §9 ADR-030.
    ///
    /// No-op if Stage is not AwaitSnapshot.
    void SupplySnapshot(Timestamp now, ByteSpan snapshot,
                        std::uint32_t scrollbackLen,
                        std::uint8_t flags) noexcept;

    /// True once GridEnd has been sent and the channel is waiting for
    /// GridApplied from the client.  At this point snapshot bytes passed to
    /// SupplySnapshot are no longer needed. §9 ADR-030.
    [[nodiscard]] bool GridEmissionDone() const noexcept;

    /// Whether the client's Hello carried ECHO_ACK and the version is 2, so
    /// that EchoAck may go out on this connection. False until live. §9.
    [[nodiscard]] bool EchoAckNegotiated() const noexcept {
        return Channel_ && Channel_->EchoAckNegotiated();
    }

    /// Whether the Hello carried a GRID_CAPABLE / ECHO_ACK TLV with the value
    /// 1. Read pre-auth, so only a test looks at them before the handshake
    /// finishes; the channel gets them after Resume has opened.
    [[nodiscard]] bool HelloOffersGridCapable() const noexcept {
        return HelloGridCapable_;
    }
    [[nodiscard]] bool HelloOffersEchoAck() const noexcept {
        return HelloEchoAck_;
    }

    /// Queues an EchoAck for the next Pump, replacing one not yet sent.
    /// Returns false, queueing nothing, unless EchoAckNegotiated. §9 ADR-036.
    [[nodiscard]] bool QueueEchoAck(const wire::EchoAckMessage& ack) noexcept {
        return Channel_ && Channel_->QueueEchoAck(ack);
    }

    // ── M7: the agent's EchoAck rules (§9, «Предиктивное эхо»). ───────────
    //
    // The connection holds the per-connection half: when the last EchoAck was
    // queued, the one waiting for the send cursor, the first one. The history
    // of input writes is the session's (AgentSession::InputWritten). The owner
    // drives it in three steps, all with the time it passes in:
    //
    //   1. EchoDeadline()/NextDeadline() says when the oldest unreported write
    //      is ECHO_TIMEOUT old (and the rate limit allows another EchoAck);
    //   2. EchoDue(now): the owner reads the pty dry (bounded) and moves what
    //      it read into the session's output;
    //   3. SettleEcho(now): input is up to the largest aged write, output is
    //      up to the end of what the session now holds.
    //
    // Pump then sends the EchoAck once the connection's send cursor has
    // reached that output offset. Nothing here wakes or sends on a connection
    // that did not negotiate ECHO_ACK.

    /// Says whether the owner's pty read is open. False while read output
    /// waits to be moved into the session (backpressure, §9): the end of the
    /// session's output then trails what the program has written, and an
    /// EchoAck computed on it would deny correct predictions. While it is
    /// false no EchoAck is settled or sent, and none is due.
    void SetPtyReadOpen(bool open) noexcept { PtyReadOpen_ = open; }

    /// When the owner should next drain the pty and call SettleEcho; unset
    /// when no EchoAck can go (not negotiated, not live, snapshot in flight,
    /// pty read closed, no unreported write).
    [[nodiscard]] Deadline EchoDeadline() const noexcept;
    [[nodiscard]] bool EchoDue(Timestamp now) const noexcept {
        return EchoDeadline().HasExpired(now);
    }

    /// Computes the EchoAck of this moment. The owner has read the pty first
    /// (EchoDue) and set SetPtyReadOpen to what it is now. Replaces one that
    /// was computed but not yet sent; may queue it at once when the send has
    /// already reached its output offset.
    void SettleEcho(Timestamp now) noexcept;

    /// Returns the earliest of the base deadline and the AwaitSnapshot
    /// deadlines: the Resize wait (RESIZE_WAIT_TIMEOUT, set on every path-B
    /// Resume, with or without a known size), SUPPLY_SNAPSHOT_TIMEOUT, and the
    /// GridApplied wait (GRID_APPLIED_TIMEOUT).  A caller that sleeps until
    /// bytes or NextDeadline is woken in time to ask for the snapshot.
    /// Shadows ConnectionBase so callers that hold an AgentConnection directly
    /// see these deadlines.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

private:
    enum class EStage : std::uint8_t {
        AwaitHello,
        AwaitRoute,
        Handshake,
        AwaitResume,
        /// Path B: waiting for the caller to build and supply a grid snapshot
        /// via SupplySnapshot. Incoming frames (Resize, Data) are still
        /// processed; outgoing polling is suspended until SupplySnapshot is
        /// called. §9 ADR-030.
        AwaitSnapshot,
        Live,
    };

    [[nodiscard]] ConnectionResult PumpHello() noexcept;
    [[nodiscard]] ConnectionResult PumpHandshake(MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult PumpResume(Timestamp now,
                                              MutableByteSpan out,
                                              MutableByteSpan scratch) noexcept;
    [[nodiscard]] ConnectionResult PumpAwaitSnapshot(
        Timestamp now, MutableByteSpan out, MutableByteSpan scratch) noexcept;
    [[nodiscard]] ConnectionResult Refuse(SessionError error) noexcept;
    /// Hands the session's SessionEnd, if any, to the live channel.
    void QueueEnd() noexcept;
    /// Whether an EchoAck may be settled and sent now.
    [[nodiscard]] bool EchoActive() const noexcept;
    /// Where the input of the next EchoAck must go past.
    [[nodiscard]] std::uint64_t EchoFloor() const noexcept;
    /// Queues the settled EchoAck once the send cursor has reached its output.
    void ReleaseEcho(Timestamp now) noexcept;
    /// The first EchoAck of the connection, ahead of its first Data. §9.
    void QueueFirstEcho(Timestamp now, std::uint64_t outputOffset) noexcept;

    EStage Stage_{EStage::AwaitHello};
    /// Hello is copied out of the frame buffer: the caller routes it between
    /// two Pumps, and the frame stream reclaims its bytes on the next call.
    std::array<std::byte, wire::MAX_HANDSHAKE_FRAME> Hello_{};
    std::size_t HelloSize_{0};
    wire::SessionId HelloId_{};
    bool HelloAnswered_{false};
    /// Whether the client's Hello carried a valid GRID_CAPABLE TLV. Read
    /// pre-auth but trusted only after SetGridInfo is called post-auth. §7.
    bool HelloGridCapable_{false};
    /// Likewise for ECHO_ACK; handed to the Channel once Resume has opened.
    bool HelloEchoAck_{false};
    /// True once PumpAwaitSnapshot has returned NeedsSnapshot once.  Avoids a
    /// busy-loop: the first call informs the caller a snapshot is needed;
    /// subsequent calls return Idle until the snapshot is supplied. §9.
    bool SnapshotReminderSent_{false};
    /// True from path-B entry until the client's post-Resume Resize arrives or
    /// ResizeWaitDeadline_ expires.  NeedsSnapshot is held back meanwhile, so
    /// its dimensions reflect the current terminal rather than a stale one.
    /// Applies whether or not a size is known. §9, path B, step 1.
    bool WaitForResize_{false};
    /// Cleared once: deadline for WaitForResize_ (START + RESIZE_WAIT_TIMEOUT).
    /// Once expired, WaitForResize_ is cleared so NeedsSnapshot fires with the
    /// last known size (0x0 when none is known).  std::nullopt when
    /// WaitForResize_ is false.
    std::optional<Timestamp> ResizeWaitDeadline_{};
    /// Set on path-B entry; cleared when SupplySnapshot succeeds.  If the
    /// caller has not supplied a snapshot by SUPPLY_SNAPSHOT_TIMEOUT the
    /// connection is closed (CloseConnection; session stays alive).  §9.
    std::optional<Timestamp> SupplySnapshotDeadline_{};
    /// Set when StartGrid succeeds (GridEnd queued); cleared when GridApplied
    /// is received.  If the client does not confirm within GRID_APPLIED_TIMEOUT
    /// the connection is closed (CloseConnection; session stays alive).  §9.
    std::optional<Timestamp> GridAppliedDeadline_{};
    /// The EchoAck computed by SettleEcho that waits for the send cursor.
    std::optional<wire::EchoAckMessage> EchoPending_;
    /// When an EchoAck was last queued on this connection (the first one
    /// included): the rate limit counts from it.
    std::optional<Timestamp> EchoQueuedAt_;
    bool PtyReadOpen_{true};
    std::optional<ResumeAttempt> Attempt_;
    std::optional<SessionHandle> Handle_;
    std::optional<DirectionFrames> Current_;
    std::optional<DirectionFrames> Fallback_;
};

/// The client's end.
class ClientConnection : public ConnectionBase {
public:
    /// `frameStorage` must hold CONNECTION_FRAME_STORAGE bytes.
    /// `gridCapable` includes GRID_CAPABLE in the Hello TLV so the agent can
    /// serve grid snapshots on path B reconnect. §7 ADR-035.
    /// `ceiling` caps the version negotiated in Hello; default is
    /// PROTOCOL_VERSION_MAX (currently 2). Tests that exercise ver=1 pass
    /// PROTOCOL_VERSION_MIN here.
    /// `snapshotBuf` is the caller-owned buffer for incoming grid snapshots
    /// (§1: no allocation in the core). Must be at least
    /// wire::SNAPSHOT_MAX_BYTES when path B may be used; pass {} otherwise.
    /// `echoAck` includes ECHO_ACK in the Hello TLV (only with a ceiling of 2
    /// or more) and makes the connection check the EchoAcks it then gets,
    /// reported as EchoAcked. §9 ADR-036.
    ClientConnection(ClientSession& session, wire::HandshakeNonce clientNonce,
                     MutableByteSpan frameStorage, Timestamp now,
                     bool gridCapable = false,
                     std::uint16_t ceiling = wire::PROTOCOL_VERSION_MAX,
                     MutableByteSpan snapshotBuf = {},
                     bool echoAck = false) noexcept;

    /// As AgentConnection::Pump. The first call writes Hello.
    [[nodiscard]] ConnectionResult Pump(Timestamp now, MutableByteSpan out,
                                        MutableByteSpan scratch) noexcept;

    /// The terminal's size, sent once live and again on every change. Kept
    /// until then: §7 has the client send it first after Resume.
    void QueueResize(wire::ResizeMessage size) noexcept;

    /// As AgentConnection::Close.
    void Close(Timestamp now) noexcept;

    /// The snapshot carried by ApplySnapshot was rendered.  Queues GridApplied
    /// on the wire and advances the session's stream gen so the next Resume
    /// carries the updated gen.  No-op if gen does not match the pending apply.
    void ConfirmApply(std::uint32_t gen) noexcept;

    /// The snapshot could not be applied (allocation failure, etc.).  Clears
    /// the pending apply without sending GridApplied.  The session's stream gen
    /// stays at its current value; the agent will resend the snapshot on the
    /// next reconnect if needed.  No-op if gen does not match the pending
    /// apply.
    void DenyApply(std::uint32_t gen) noexcept;

private:
    enum class EStage : std::uint8_t {
        SendHello,
        Handshake,
        Live,
    };

    [[nodiscard]] ConnectionResult PumpHandshake(Timestamp now,
                                                 MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult SendResume(Timestamp now,
                                              MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult Refuse(SessionError error) noexcept;

    ClientSession& Session_;
    EStage Stage_{EStage::SendHello};
    ClientResumeAttempt Attempt_;
    std::optional<ClientSessionHandle> Handle_;
    std::optional<wire::ResizeMessage> PendingResize_;
    /// Whether Hello carried ECHO_ACK; handed to the Channel when it opens.
    bool EchoAck_{false};
    /// Set when Pump returns ApplySnapshot; cleared by ConfirmApply/DenyApply.
    std::optional<std::uint32_t> PendingApplyGen_{};
};

}  // namespace zet::session
