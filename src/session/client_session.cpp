#include "zet/session/client_session.hpp"

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

}  // namespace

ClientSession::ClientSession(wire::SessionSecrets secrets,
                             SessionStreams streams) noexcept
    : Id_(secrets.Id),
      Auth_(std::move(secrets.Auth)),
      Seed_(std::move(secrets.ConnectionSeed)),
      Streams_(streams, EDetachedOverflow::Backpressure) {}

const wire::SessionId& ClientSession::Id() const noexcept { return Id_; }

std::size_t ClientSession::RememberedNonces() const noexcept {
    return Seen_.Size();
}

ClientSessionHandle::ClientSessionHandle(ClientSession& session) noexcept
    : Session_(&session) {}

const wire::SessionId& ClientSessionHandle::Id() const noexcept {
    return Session_->Id_;
}

void ClientSessionHandle::ConfirmAgentFrame() noexcept {
    if (Confirmed_) {
        return;
    }
    Confirmed_ = true;
    // §8: once the agent's first sealed frame of this connection is verified,
    // the client moves to the next seed. Move-assign wipes the old one:
    // Secret zeroes the value it replaces, so the previous seed cannot be
    // derived from memory after this call — that is the forward-secrecy
    // guarantee for the client side.
    Session_->Seed_ = wire::AdvanceConnectionSeed(Session_->Seed_);
}

SessionResult<void> ClientSessionHandle::Attach(
    std::uint64_t agentReceiveOffset) noexcept {
    auto generation = Session_->Streams_.Attach(agentReceiveOffset);
    if (!generation) {
        return std::unexpected(generation.error());
    }
    Generation_ = *generation;
    return {};
}

void ClientSessionHandle::Detach(Timestamp now) noexcept {
    Session_->Streams_.Detach(Generation_, now);
}

bool ClientSessionHandle::IsCurrent() const noexcept {
    return Session_->Streams_.IsCurrent(Generation_);
}

void ClientSessionHandle::Kill(ESessionError reason) noexcept {
    Session_->Streams_.Kill(reason);
}

SendBuffer& ClientSessionHandle::Output() noexcept {
    return Session_->Streams_.Output();
}

ReceiveCursor& ClientSessionHandle::Input() noexcept {
    return Session_->Streams_.Input();
}

LivenessLimits ClientSessionHandle::Liveness() const noexcept {
    return Session_->Streams_.Limits().Liveness;
}

void ClientSessionHandle::RecordEnd(wire::SessionEndMessage end) noexcept {
    if (!Session_->End_) {
        Session_->End_ = end;
    }
}

ClientResumeAttempt::ClientResumeAttempt(
    ClientSession& session, wire::HandshakeNonce clientNonce) noexcept
    : Session_(&session),
      Shake_(session.Id_, Clone(session.Auth_), Clone(session.Seed_),
             clientNonce, session.Seen_) {}

wire::PreAuthResult<std::size_t> ClientResumeAttempt::Start(
    MutableByteSpan out) noexcept {
    return Shake_.Start(out);
}

wire::PreAuthResult<wire::HandshakeStep> ClientResumeAttempt::Handle(
    ByteSpan body, MutableByteSpan out) noexcept {
    return Shake_.Handle(body, out);
}

SessionResult<ClientResumed> ClientResumeAttempt::Finish() && noexcept {
    auto result = Shake_.TakeResult();
    if (!result) {
        return std::unexpected(ESessionError::NotAuthenticated);
    }
    // The first write into the session, and it happens only here, after the
    // agent has proved itself: tag_s in AuthOk is the mutual-authentication
    // step (§5 rule four). Without it a fake agent would make the client hand
    // over its resend buffer — the typed input, potentially passwords.
    Session_->Seen_.Remember(result->ClientNonce);
    Session_->Seen_.Remember(result->ServerNonce);
    return ClientResumed{.Handle = ClientSessionHandle{*Session_},
                         .Result = std::move(*result)};
}

}  // namespace zet::session
