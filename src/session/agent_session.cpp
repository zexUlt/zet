#include "zet/session/agent_session.hpp"

#include <utility>

namespace zet::session {
namespace {

/// A second copy of a key for the handshake, which owns what it is given.
///
/// Written straight into the new Secret rather than through its value
/// constructor: that one takes the array by value, and the parameter is a
/// stack copy of the key that nobody wipes.
[[nodiscard]] crypto::Key Clone(const crypto::Key& key) noexcept {
    crypto::Key copy;
    copy.Expose() = key.Expose();
    return copy;
}

[[nodiscard]] std::optional<crypto::Key> Clone(
    const std::optional<crypto::Key>& key) noexcept {
    if (!key) {
        return std::nullopt;
    }
    return Clone(*key);
}

}  // namespace

AgentSession::AgentSession(wire::SessionSecrets secrets) noexcept
    : Id_(secrets.Id),
      Auth_(std::move(secrets.Auth)),
      Seed_(std::move(secrets.ConnectionSeed)) {}

SessionRef AgentSession::Ref() noexcept { return SessionRef{*this}; }

SessionRef::SessionRef(AgentSession& session) noexcept : Session_(&session) {}

const wire::SessionId& SessionRef::Id() const noexcept { return Session_->Id_; }

std::size_t SessionRef::RememberedNonces() const noexcept {
    return Session_->Seen_.Size();
}

SessionHandle::SessionHandle(AgentSession& session) noexcept
    : Session_(&session) {}

const wire::SessionId& SessionHandle::Id() const noexcept {
    return Session_->Id_;
}

SessionResult<void> SessionHandle::ConfirmResume(EResumeKeys used) noexcept {
    AgentSession& session = *Session_;
    if (used == EResumeKeys::Previous) {
        if (!session.PreviousSeed_) {
            return std::unexpected(ESessionError::UnknownSeed);
        }
        // The client is a seed behind. What we had moved on to was never
        // used and is replaced by the very same derivation from the seed the
        // client is on, so the two ends meet again.
        session.Seed_ = std::move(*session.PreviousSeed_);
    }
    // Assigning wipes: Secret zeroes the value it replaces, which is the point
    // of the ratchet — the seed two steps back must not survive this call.
    crypto::Key next = wire::AdvanceConnectionSeed(session.Seed_);
    session.PreviousSeed_ = std::move(session.Seed_);
    session.Seed_ = std::move(next);
    return {};
}

ResumeAttempt::ResumeAttempt(SessionRef session,
                             wire::HandshakeNonce serverNonce,
                             std::uint64_t receiveOffset) noexcept
    : ResumeAttempt(session.Session_, Clone(session.Session_->Auth_),
                    Clone(session.Session_->Seed_),
                    Clone(session.Session_->PreviousSeed_), serverNonce,
                    receiveOffset) {}

ResumeAttempt ResumeAttempt::ForUnknown(
    const crypto::Key& localSecret, const wire::SessionId& id,
    wire::HandshakeNonce serverNonce) noexcept {
    auto decoy = wire::DeriveDecoySecrets(localSecret, id);
    // Offset zero, as for a session that has received nothing yet. It would
    // only reach the wire in AuthOk, and a decoy never gets that far.
    return ResumeAttempt{
        nullptr,      std::move(decoy.Auth), std::move(decoy.ConnectionSeed),
        std::nullopt, serverNonce,           0};
}

ResumeAttempt::ResumeAttempt(AgentSession* session, crypto::Key auth,
                             crypto::Key seed,
                             std::optional<crypto::Key> previousSeed,
                             wire::HandshakeNonce serverNonce,
                             std::uint64_t receiveOffset) noexcept
    : Session_(session),
      Shake_(std::move(auth), std::move(seed), serverNonce, receiveOffset,
             session != nullptr ? session->Seen_ : DecoySeen_,
             std::move(previousSeed)) {}

wire::PreAuthResult<wire::HandshakeStep> ResumeAttempt::Handle(
    ByteSpan body, MutableByteSpan out) noexcept {
    return Shake_.Handle(body, out);
}

SessionResult<Resumed> ResumeAttempt::Finish() && noexcept {
    auto result = Shake_.TakeResult();
    // A decoy cannot finish: its handshake fails at tag_c. The second check is
    // here so that if it ever did, it would be handed nothing to change.
    if (!result || Session_ == nullptr) {
        return std::unexpected(ESessionError::NotAuthenticated);
    }
    // The first write into the session, and it happens only here, after the
    // peer has proved itself (§5 rules one and three).
    Session_->Seen_.Remember(result->ClientNonce);
    Session_->Seen_.Remember(result->ServerNonce);
    return Resumed{.Handle = SessionHandle{*Session_},
                   .Result = std::move(*result)};
}

}  // namespace zet::session
