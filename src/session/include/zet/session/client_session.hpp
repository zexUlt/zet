#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/error.hpp"
#include "zet/session/stream_state.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/message.hpp"

/// The client's side of a session and the one way to change it, per
/// docs/design.md §5 rule one: until tag_s verifies (AuthOk), not one byte of
/// session state moves.
///
/// The rule is carried by types rather than by care:
///
///     ClientSession ──ClientResumeAttempt──▶ Finish() && ──▶
///     ClientSessionHandle
///       no public          Start() / Handle()                  the only
///       mutating mutators           own the handshake                   path;
///       changes the
///                                                              session via
///                                                              ConfirmAgentFrame()
///
/// There is no ClientSessionRef here. AgentSession needs SessionRef because its
/// routing layer (find-by-sid table) must pass something to ResumeAttempt
/// without handing over the session itself. The client initiates and always has
/// the session in its hands; a read-only view between ClientSession and
/// ClientResumeAttempt would exist for symmetry, not for structural need.
/// Symmetry without purpose is noise.
namespace zet::session {

class ClientSessionHandle;
class ClientResumeAttempt;

/// What the client holds for one session across all of its connections.
///
/// Everything that changes it is private and reachable only through a
/// ClientSessionHandle. The class is neither copyable nor movable.
class ClientSession {
public:
    ClientSession(wire::SessionSecrets secrets,
                  SessionStreams streams) noexcept;

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    ClientSession(ClientSession&&) = delete;
    ClientSession& operator=(ClientSession&&) = delete;
    ~ClientSession() = default;

    [[nodiscard]] const wire::SessionId& Id() const noexcept;

    /// How many handshake nonces the session remembers. For tests and for
    /// zet-dump; reading it changes nothing.
    [[nodiscard]] std::size_t RememberedNonces() const noexcept;

    // ── The keyboard's door: what the user types goes into the stream. Not a
    // change the agent can cause, so not behind the handle.

    /// See StreamState::Write. Overflow while detached is backpressure: the
    /// client stops reading the keyboard rather than ending anything.
    [[nodiscard]] SessionResult<std::size_t> Write(ByteSpan input) noexcept {
        return Streams_.Write(input);
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

    /// How much of the agent's stream the client holds: what Resume tells the
    /// agent to resend from.
    [[nodiscard]] std::uint64_t ReceiveOffset() const noexcept {
        return Streams_.ReceiveOffset();
    }

    /// How the agent's shell ended, once a SessionEnd has arrived. The client
    /// holds the whole of the agent's stream by then (§9), so there is
    /// nothing left to reconnect for.
    [[nodiscard]] std::optional<wire::SessionEndMessage> EndedWith()
        const noexcept {
        return End_;
    }

    /// The last output generation the client has applied (via GridApplied).
    /// Sent in Resume at ver=2 as stream_gen so the agent knows whether path
    /// B is needed. §9 ADR-030.
    [[nodiscard]] std::uint32_t StreamGen() const noexcept {
        return StreamGen_;
    }

private:
    friend class ClientSessionHandle;
    friend class ClientResumeAttempt;

    wire::SessionId Id_;
    crypto::Key Auth_;
    crypto::Key Seed_;
    wire::NonceMemory Seen_;
    StreamState Streams_;
    std::optional<wire::SessionEndMessage> End_;
    /// See StreamGen(). Zero until the first GridApplied is sent.
    std::uint32_t StreamGen_{0};
};

/// Proof that the handshake succeeded with an agent that knows K_auth, and the
/// key to the §8 seed ratchet on the client.
///
/// Move-only: a handle stands for one authenticated connection. Copying it
/// would let a stale handle keep the right to advance the session's seed.
class ClientSessionHandle {
public:
    ClientSessionHandle(const ClientSessionHandle&) = delete;
    ClientSessionHandle& operator=(const ClientSessionHandle&) = delete;
    ClientSessionHandle(ClientSessionHandle&&) noexcept = default;
    ClientSessionHandle& operator=(ClientSessionHandle&&) noexcept = default;
    ~ClientSessionHandle() = default;

    [[nodiscard]] const wire::SessionId& Id() const noexcept;

    /// Advances the session to the next seed, per §8, called once the agent's
    /// first sealed frame of this connection has been verified.
    ///
    /// Idempotent: a second call for the same handle changes nothing. This is
    /// a deliberate choice over an error: the frame-opening callback may fire
    /// more than once due to a caller bug, and silently corrupting the session
    /// state (double-advancing the seed) is worse than ignoring the repeat.
    void ConfirmAgentFrame() noexcept;

    /// Makes this connection the live one, resending from what AuthOk said
    /// the agent holds. See SessionHandle::Attach.
    [[nodiscard]] SessionResult<void> Attach(
        std::uint64_t agentReceiveOffset) noexcept;
    /// See SessionHandle::Detach.
    void Detach(Timestamp now) noexcept;
    [[nodiscard]] bool IsCurrent() const noexcept;
    /// Ends the session, for an error whose disposition says so — the agent
    /// reaped it, or the client gave up on it.
    void Kill(ESessionError reason) noexcept;

    [[nodiscard]] SendBuffer& Output() noexcept;
    [[nodiscard]] ReceiveCursor& Input() noexcept;

    /// See SessionHandle::Liveness.
    [[nodiscard]] LivenessLimits Liveness() const noexcept;

    /// Records the agent's SessionEnd. The first one decides: the agent sends
    /// the same message on every connection.
    void RecordEnd(wire::SessionEndMessage end) noexcept;

    /// Update StreamGen after the client applies a grid snapshot and sends
    /// GridApplied. The session then sends stream_gen in the next Resume. §9.
    void SetStreamGen(std::uint32_t gen) noexcept;

private:
    friend class ClientResumeAttempt;

    explicit ClientSessionHandle(ClientSession& session) noexcept;

    ClientSession* Session_;
    std::uint64_t Generation_{0};
    bool Confirmed_{false};
};

/// What a finished client attempt yields.
struct ClientResumed {
    ClientSessionHandle Handle;
    wire::HandshakeResult Result;
};

/// One resume handshake on the client, from Hello to AuthOk.
///
/// Built directly from a ClientSession reference: the client knows which
/// session it wants and there is no routing step between them.
class ClientResumeAttempt {
public:
    ClientResumeAttempt(ClientSession& session,
                        wire::HandshakeNonce clientNonce) noexcept;

    ClientResumeAttempt(const ClientResumeAttempt&) = delete;
    ClientResumeAttempt& operator=(const ClientResumeAttempt&) = delete;
    ClientResumeAttempt(ClientResumeAttempt&&) = delete;
    ClientResumeAttempt& operator=(ClientResumeAttempt&&) = delete;
    ~ClientResumeAttempt() = default;

    /// Writes Hello. Valid once, before Handle is called.
    ///
    /// A refusal from this or from Handle ends the handshake for good: a step
    /// that failed part way has already folded its message into the transcript,
    /// so retrying it with a wider buffer would fold it twice and leave the two
    /// ends computing different tags.
    [[nodiscard]] wire::PreAuthResult<std::size_t> Start(
        MutableByteSpan out) noexcept;

    /// Takes one handshake message body and writes whatever answers it.
    [[nodiscard]] wire::PreAuthResult<wire::HandshakeStep> Handle(
        ByteSpan body, MutableByteSpan out) noexcept;

    /// The handle, once and only once the handshake finished.
    ///
    /// Rvalue-qualified so that the attempt is visibly spent by asking:
    /// `std::move(attempt).Finish()`. This is the first point at which the
    /// session changes — the nonces the handshake used are recorded here, after
    /// the agent has proved itself via tag_s (§5 rule four).
    [[nodiscard]] SessionResult<ClientResumed> Finish() && noexcept;

private:
    ClientSession* Session_;
    wire::ClientHandshake Shake_;
};

}  // namespace zet::session
