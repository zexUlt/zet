#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/session/error.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/receive_cursor.hpp"
#include "zet/session/send_buffer.hpp"

/// The part of a session that outlives its connections, per docs/design.md §9:
/// two reliable byte streams by absolute offset, and the life cycle around
/// them, `Bootstrapping → Live → Detached → (Live | Reaped)`.
namespace zet::session {

/// What a session is given to hold its outgoing stream in, and the policy it
/// runs under. The storage is borrowed, as everywhere in the core: the I/O
/// layer allocates it once per session and the session never grows it. It
/// must outlive the session: a buffer that dies first leaves the session
/// writing into freed memory.
struct SessionStreams {
    MutableByteSpan Storage;
    /// When the session came to be. The orphan timeout runs from here until
    /// the first connection attaches.
    Timestamp Now;
    SessionLimits Limits;
};

enum class ESessionState : std::uint8_t {
    /// No connection carries the session. Output piles up for the resend.
    Detached,
    /// A connection that authenticated is attached.
    Live,
    /// Ended for good, with a reason. Nothing changes it any more.
    Reaped,
};

/// What overflow while detached does to a session, which is where the agent
/// and the client part ways. On the agent the shell keeps writing while
/// nobody is there, and §9 has the session end with a reason rather than drop
/// a byte. On the client the writer is a person at a keyboard, and the answer
/// to "too much typed while offline" is to stop reading the keyboard.
///
/// M6 adds Resync (ADR-035): for a ver=2 + GRID_CAPABLE session the ring
/// fills and ring_overflowed is set; the pty continues feeding the grid
/// state machine, but the ring accepts no new bytes. On the next reconnect
/// the agent must choose path B regardless of replay cost.
enum class EDetachedOverflow : std::uint8_t {
    Reap,
    Backpressure,
    Resync,
};

/// Streams and life cycle, shared by AgentSession and ClientSession.
///
/// Every method is public, and that is safe only because the class is: it is
/// a private member of both sessions, which forward the local writer's calls
/// and leave the rest to their handles. Nothing that has not authenticated
/// can name it.
class StreamState {
public:
    StreamState(SessionStreams streams, EDetachedOverflow overflow) noexcept;

    StreamState(const StreamState&) = delete;
    StreamState& operator=(const StreamState&) = delete;
    StreamState(StreamState&&) = delete;
    StreamState& operator=(StreamState&&) = delete;
    ~StreamState() = default;

    // ── The local writer: the pty on the agent, the keyboard on the client.

    /// Takes output for the peer and reports how much of it was taken.
    ///
    /// While live, at most ResendMax may wait unacknowledged; less than all of
    /// `bytes` is backpressure, and the writer stops reading its source until
    /// AcceptsWrites() says otherwise (§9). While detached the ceiling is
    /// DisconnectMax, and crossing it either reaps the session with
    /// DisconnectBufferOverflow or holds back like the live case, by
    /// EDetachedOverflow. A reaped session takes nothing: SessionReaped.
    ///
    /// Resync mode after ring_overflowed: returns bytes.size() (all bytes
    /// "accepted") so the pty loop does not retry stale data. The bytes are
    /// discarded because the ring is full and path B will deliver a snapshot
    /// of everything that matters; flushing old bytes would corrupt the stream.
    [[nodiscard]] SessionResult<std::size_t> Write(ByteSpan bytes) noexcept;

    /// Whether reading the local source is worth it right now. A detached
    /// session that reaps on overflow always says yes: the shell must not
    /// block on a client that may never return, and the Write that crosses
    /// DisconnectMax is what ends it.
    [[nodiscard]] bool AcceptsWrites() const noexcept;

    [[nodiscard]] ESessionState State() const noexcept { return State_; }

    /// Why the session ended, once it has.
    [[nodiscard]] std::optional<ESessionError> ReapReason() const noexcept {
        return ReapReason_;
    }

    /// The orphan deadline while detached, nothing otherwise. No ticks: the
    /// caller's timer queue asks this and sleeps until then.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    /// Reaps the session if its orphan deadline has passed.
    void OnTimer(Timestamp now) noexcept;

    /// How much of the peer's stream this end holds. Read before
    /// authentication, for the offset AuthOk and Resume carry.
    [[nodiscard]] std::uint64_t ReceiveOffset() const noexcept {
        return Input_.RecvOffset();
    }

    // ── M6: path B helpers (§9 ADR-030). ────────────────────────────────────

    /// Update the overflow behavior after a successfully authenticated
    /// connection. Called from SessionHandle::SetGridInfo once the negotiated
    /// version and GRID_CAPABLE TLV are known (trusted only post-auth).
    ///
    /// ver=2 + gridCapable → Resync mode; anything else keeps Reap.
    void SetOverflowMode(std::uint16_t negotiatedVer,
                         bool gridCapable) noexcept;

    /// True when the ring overflowed in Resync mode. Path B is mandatory on
    /// the next reconnect.
    [[nodiscard]] bool RingOverflowed() const noexcept {
        return Output_.RingOverflowed();
    }

    /// Current output generation. Incremented by NewGeneration().
    [[nodiscard]] std::uint32_t SessionGen() const noexcept {
        return Output_.SessionGen();
    }

    /// Open a new output generation. Delegates to SendBuffer::NewGeneration().
    void NewGeneration() noexcept { Output_.NewGeneration(); }

    /// Attach for path B: like Attach(), but skips ResetSendNext, because the
    /// caller will follow immediately with NewGeneration() which clears the
    /// ring. Returns the connection generation on success.
    [[nodiscard]] SessionResult<std::uint64_t> AttachPathB() noexcept;

    // ── Behind a handle only.

    /// Makes the connection that just authenticated the live one: resending
    /// starts at what the peer says it holds, and the returned generation is
    /// what that connection detaches with. A connection already attached is
    /// superseded, not detached — it finds out through IsCurrent().
    [[nodiscard]] SessionResult<std::uint64_t> Attach(
        std::uint64_t peerReceiveOffset) noexcept;

    /// Detaches the connection of `generation`, if it is still the live one.
    /// A connection that was superseded has nothing to detach, and must not
    /// detach the one that replaced it.
    void Detach(std::uint64_t generation, Timestamp now) noexcept;

    [[nodiscard]] bool IsCurrent(std::uint64_t generation) const noexcept {
        return State_ == ESessionState::Live && generation == Generation_;
    }

    /// Ends the session for a reason a connection learned, such as an error
    /// whose disposition is KillSession.
    void Kill(ESessionError reason) noexcept;

    [[nodiscard]] SendBuffer& Output() noexcept { return Output_; }

    /// The policy the session was given.
    [[nodiscard]] const SessionLimits& Limits() const noexcept {
        return Limits_;
    }
    [[nodiscard]] ReceiveCursor& Input() noexcept { return Input_; }

private:
    void Reap(ESessionError reason) noexcept;

    SendBuffer Output_;
    ReceiveCursor Input_;
    SessionLimits Limits_;
    EDetachedOverflow Overflow_;
    ESessionState State_{ESessionState::Detached};
    std::optional<ESessionError> ReapReason_;
    Timestamp DetachedSince_;
    std::uint64_t Generation_{0};
};

}  // namespace zet::session
