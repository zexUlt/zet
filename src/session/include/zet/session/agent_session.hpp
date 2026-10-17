#pragma once

#include <cstdint>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/error.hpp"
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
    explicit AgentSession(wire::SessionSecrets secrets) noexcept;

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    AgentSession(AgentSession&&) = delete;
    AgentSession& operator=(AgentSession&&) = delete;
    ~AgentSession() = default;

    /// The view an unauthenticated connection is allowed.
    [[nodiscard]] SessionRef Ref() noexcept;

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

private:
    friend class ResumeAttempt;

    explicit SessionHandle(AgentSession& session) noexcept;

    AgentSession* Session_;
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
    /// `receiveOffset` is how much of the client's stream the agent holds; it
    /// goes into AuthOk under the transcript MAC.
    ResumeAttempt(SessionRef session, wire::HandshakeNonce serverNonce,
                  std::uint64_t receiveOffset) noexcept;

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
