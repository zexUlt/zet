#pragma once

#include <cstdint>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/error.hpp"
#include "zet/session/stream_state.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/key_schedule.hpp"

/// The agent's side of a session and the one way to change it, per
/// docs/design.md §5 rule one: until tag_c verifies, not one byte of session
/// state moves.
///
/// The rule is carried by types rather than by care:
///
///     AgentSession ──Ref()──▶ SessionRef ──▶ ResumeAttempt ──Finish()&&──▶
///     SessionHandle
///       no public              read-only       owns the          the only
///       holder of mutators               view            handshake mutating
///       methods
///
/// Code that has only read a sid off the wire holds a SessionRef, and there is
/// nothing on it to call. The handshake reads the session through const
/// references. A SessionHandle cannot be constructed, only received from a
/// finished attempt, and tests/compile_fail checks each of these doors stays
/// shut.
namespace zet::session {

class SessionRef;
class SessionHandle;
class ResumeAttempt;

/// Which keys opened the client's Resume. §8 has the agent try the keys of its
/// current seed first and those of the seed before it second.
enum class EResumeKeys : std::uint8_t {
    /// The client moved on with us last time.
    Current,
    /// Our answer to the last Resume was cut and the client is a seed behind.
    Previous,
};

/// What the agent holds for one session across all of its connections.
///
/// Everything that changes it is private and reachable only through a
/// SessionHandle. The class is neither copyable nor movable: a SessionRef
/// points into it, and the table that owns sessions keeps them in place.
class AgentSession {
public:
    AgentSession(wire::SessionSecrets secrets, SessionStreams streams) noexcept;

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    AgentSession(AgentSession&&) = delete;
    AgentSession& operator=(AgentSession&&) = delete;
    ~AgentSession() = default;

    /// The view an unauthenticated connection is allowed.
    [[nodiscard]] SessionRef Ref() noexcept;

    // ── The pty's door. Writing the shell's output into the stream is not a
    // change any peer causes, and the code holding a connection never has the
    // session itself, only a SessionRef — so this is no way around §5.

    /// See StreamState::Write. The agent reaps on overflow while detached.
    [[nodiscard]] SessionResult<std::size_t> Write(ByteSpan output) noexcept {
        return Streams_.Write(output);
    }
    [[nodiscard]] bool AcceptsWrites() const noexcept {
        return Streams_.AcceptsWrites();
    }
    [[nodiscard]] ESessionState State() const noexcept {
        return Streams_.State();
    }
    [[nodiscard]] std::optional<ESessionError> ReapReason() const noexcept {
        return Streams_.ReapReason();
    }
    [[nodiscard]] Deadline NextDeadline() const noexcept {
        return Streams_.NextDeadline();
    }
    void OnTimer(Timestamp now) noexcept { Streams_.OnTimer(now); }

private:
    friend class SessionRef;
    friend class SessionHandle;
    friend class ResumeAttempt;

    wire::SessionId Id_;
    crypto::Key Auth_;
    crypto::Key Seed_;
    /// The seed §8 keeps while the client may not have moved past it. Empty
    /// until the first connection is confirmed.
    std::optional<crypto::Key> PreviousSeed_;
    wire::NonceMemory Seen_;
    StreamState Streams_;
};

/// A session as seen before its peer has proved anything.
///
/// Only reads. It exists so that the code routing a Hello by its sid has
/// something to hand to ResumeAttempt without having the session in its hands.
class SessionRef {
public:
    [[nodiscard]] const wire::SessionId& Id() const noexcept;

    /// How many handshake nonces the session remembers. For tests and for
    /// zet-dump; reading it changes nothing.
    [[nodiscard]] std::size_t RememberedNonces() const noexcept;

    /// How much of the client's stream the session holds: what AuthOk will
    /// tell the client to resend from.
    [[nodiscard]] std::uint64_t ReceiveOffset() const noexcept;

private:
    friend class AgentSession;
    friend class ResumeAttempt;

    explicit SessionRef(AgentSession& session) noexcept;

    AgentSession* Session_;
};

/// Proof that the peer of this connection holds K_auth for this session, and
/// the methods that may only be called with it.
///
/// Move-only: a handle stands for one authenticated connection and copying it
/// would let a stale connection keep the right to change the session.
class SessionHandle {
public:
    SessionHandle(const SessionHandle&) = delete;
    SessionHandle& operator=(const SessionHandle&) = delete;
    SessionHandle(SessionHandle&&) noexcept = default;
    SessionHandle& operator=(SessionHandle&&) noexcept = default;
    ~SessionHandle() = default;

    [[nodiscard]] const wire::SessionId& Id() const noexcept;

    /// The two-phase seed change of §8, called once Resume has opened.
    ///
    /// Whichever seed the client used becomes the previous one and its
    /// successor the current; anything else is wiped. That is what makes the
    /// keys of connection n underivable once connection n+1 has been
    /// confirmed: by then the seed of n has been replaced twice.
    [[nodiscard]] SessionResult<void> ConfirmResume(EResumeKeys used) noexcept;

    /// Makes this connection the live one once Resume has opened, resending
    /// from what the client says it holds. A connection that was live until
    /// now is superseded rather than detached; it learns so from IsCurrent().
    [[nodiscard]] SessionResult<void> Attach(
        std::uint64_t clientReceiveOffset) noexcept;

    /// Lets go of the session when this connection closes. Does nothing if a
    /// newer connection has taken over — it is that one's to detach.
    void Detach(Timestamp now) noexcept;

    /// Whether this connection is still the one attached. A superseded
    /// connection must stop touching the streams.
    [[nodiscard]] bool IsCurrent() const noexcept;

    /// Ends the session, for an error whose disposition says so.
    void Kill(ESessionError reason) noexcept;

    [[nodiscard]] SendBuffer& Output() noexcept;
    [[nodiscard]] ReceiveCursor& Input() noexcept;

private:
    friend class ResumeAttempt;

    explicit SessionHandle(AgentSession& session) noexcept;

    AgentSession* Session_;
    /// Which attachment this handle made; zero until Attach.
    std::uint64_t Generation_{0};
};

/// What a finished attempt yields: the right to change the session, and the
/// keys and offsets the connection goes on with.
struct Resumed {
    SessionHandle Handle;
    wire::HandshakeResult Result;
};

/// One resume handshake on the agent, from Hello to AuthOk.
///
/// Built for a session that was found or for a decoy, and the handshake inside
/// cannot tell which (§5 rule two): both get secrets, and the decoy's nonce
/// memory is an empty one of its own.
class ResumeAttempt {
public:
    /// How much of the client's stream the agent holds goes into AuthOk under
    /// the transcript MAC; it is read from the session, so that it cannot
    /// disagree with the cursor the resend will meet.
    ResumeAttempt(SessionRef session,
                  wire::HandshakeNonce serverNonce) noexcept;

    /// For a sid no session answers to. The secrets come from
    /// DeriveDecoySecrets, so two probes for the same sid meet the same
    /// answer, and the attempt can never finish.
    [[nodiscard]] static ResumeAttempt ForUnknown(
        const crypto::Key& localSecret, const wire::SessionId& id,
        wire::HandshakeNonce serverNonce) noexcept;

    ResumeAttempt(const ResumeAttempt&) = delete;
    ResumeAttempt& operator=(const ResumeAttempt&) = delete;
    ResumeAttempt(ResumeAttempt&&) = delete;
    ResumeAttempt& operator=(ResumeAttempt&&) = delete;
    ~ResumeAttempt() = default;

    /// Feeds one handshake message, as ServerHandshake::Handle.
    [[nodiscard]] wire::PreAuthResult<wire::HandshakeStep> Handle(
        ByteSpan body, MutableByteSpan out) noexcept;

    /// The handle, once and only once the handshake finished.
    ///
    /// Rvalue-qualified so that the attempt is visibly spent by asking:
    /// `std::move(attempt).Finish()`. This is the first point at which the
    /// session changes — the nonces the handshake used are recorded here.
    [[nodiscard]] SessionResult<Resumed> Finish() && noexcept;

private:
    ResumeAttempt(AgentSession* session, crypto::Key auth, crypto::Key seed,
                  std::optional<crypto::Key> previousSeed,
                  wire::HandshakeNonce serverNonce,
                  std::uint64_t receiveOffset) noexcept;

    /// Null for a decoy. Never followed before Finish.
    AgentSession* Session_;
    /// What a decoy's handshake checks nonces against: nothing, like a session
    /// that has just been bootstrapped.
    wire::NonceMemory DecoySeen_;
    wire::ServerHandshake Shake_;
};

}  // namespace zet::session
