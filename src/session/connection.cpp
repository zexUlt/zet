#include "zet/session/connection.hpp"

#include <algorithm>
#include <span>
#include <tuple>
#include <utility>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/message.hpp"

namespace zet::session {
namespace {

/// Resume as it is sealed: the type byte and one offset.
constexpr std::size_t RESUME_PLAINTEXT_SIZE = 1 + sizeof(std::uint64_t);

/// What every cleartext frame must say in its epoch byte (§7).
constexpr std::uint8_t CLEARTEXT_EPOCH = 0;

}  // namespace

// ── ConnectionBase ─────────────────────────────────────────────────────

ConnectionBase::ConnectionBase(MutableByteSpan frameStorage,
                               Timestamp now) noexcept
    // Until the peer has proved itself it has not earned more than
    // MAX_HANDSHAKE_FRAME of our memory (§7); the limit widens once it has.
    : Frames_(frameStorage, wire::MAX_HANDSHAKE_FRAME), Started_(now) {}

ChannelResult<void> ConnectionBase::OnBytes(ByteSpan chunk) noexcept {
    if (Failed_) {
        return std::unexpected(*Failed_);
    }
    if (auto appended = Frames_.Append(chunk); !appended) {
        return std::unexpected(Fail(SessionError{appended.error()}));
    }
    return {};
}

Deadline ConnectionBase::NextDeadline() const noexcept {
    if (Failed_) {
        return Deadline{};
    }
    if (Channel_) {
        return Channel_->NextDeadline();
    }
    return Deadline{Started_ + HANDSHAKE_TIMEOUT};
}

SessionError ConnectionBase::Fail(SessionError error) noexcept {
    if (!Failed_) {
        Failed_ = error;
    }
    return *Failed_;
}

bool ConnectionBase::HandshakeExpired(Timestamp now) const noexcept {
    return !Channel_ && now >= Started_ + HANDSHAKE_TIMEOUT;
}

ChannelResult<void> ConnectionBase::CheckBuffers(
    MutableByteSpan out, MutableByteSpan scratch) noexcept {
    if (out.size() < CONNECTION_OUT_SIZE ||
        scratch.size() < CONNECTION_SCRATCH_SIZE) {
        return std::unexpected(SessionError{EProtoError::BufferTooSmall});
    }
    return {};
}

std::size_t ConnectionBase::FrameCleartext(MutableByteSpan out,
                                           std::size_t bodyLength) noexcept {
    // The writer spans exactly the header, and CheckBuffers made sure `out`
    // has one; the body came out of HandshakeBody, so its length is a u32.
    ByteWriter writer{out.first(wire::HEADER_SIZE)};
    const wire::FrameHeader header{
        .Length = static_cast<std::uint32_t>(bodyLength),
        .Epoch = CLEARTEXT_EPOCH};
    std::ignore = wire::WriteFrameHeader(writer, header);
    return wire::HEADER_SIZE + bodyLength;
}

MutableByteSpan ConnectionBase::HandshakeBody(MutableByteSpan out) noexcept {
    // Nothing in the handshake is wider than MAX_HANDSHAKE_FRAME, and bounding
    // the room keeps the length that goes into the header within a u32 by
    // construction.
    return out.subspan(wire::HEADER_SIZE)
        .first(std::min<std::size_t>(out.size() - wire::HEADER_SIZE,
                                     wire::MAX_HANDSHAKE_FRAME));
}

template <typename TOnOpened>
ConnectionResult ConnectionBase::PumpLive(Timestamp now, MutableByteSpan out,
                                          MutableByteSpan scratch,
                                          TOnOpened&& onOpened) noexcept {
    // Incoming first: it can carry an ack that frees room for what we send.
    // A frame that delivered nothing — a bare Ack, a duplicate, a Ping — is
    // not an event of its own, so look further in the same call.
    while (true) {
        auto next = Frames_.Next();
        if (!next) {
            return std::unexpected(Fail(SessionError{next.error()}));
        }
        if (!*next) {
            break;
        }
        auto event = Channel_->Receive(**next, scratch);
        if (!event) {
            return std::unexpected(Fail(event.error()));
        }
        onOpened();
        if (const auto* data = std::get_if<DeliveredData>(&*event)) {
            return Deliver{.Bytes = data->Bytes};
        }
        if (const auto* resize = std::get_if<DeliveredResize>(&*event)) {
            return Resized{.Size = resize->Resize};
        }
    }

    auto written = Channel_->Poll(now, out, scratch);
    if (!written) {
        return std::unexpected(Fail(written.error()));
    }
    if (*written > 0) {
        return SendFrame{.Written = *written};
    }
    return Idle{};
}

// ── AgentConnection ────────────────────────────────────────────────────

AgentConnection::AgentConnection(MutableByteSpan frameStorage,
                                 Timestamp now) noexcept
    : ConnectionBase(frameStorage, now) {}

void AgentConnection::Route(SessionRef session,
                            wire::HandshakeNonce serverNonce) noexcept {
    if (Stage_ != EStage::AwaitRoute) {
        return;
    }
    Attempt_.emplace(session, serverNonce);
    Stage_ = EStage::Handshake;
}

void AgentConnection::RouteUnknown(const crypto::Key& localSecret,
                                   wire::HandshakeNonce serverNonce) noexcept {
    if (Stage_ != EStage::AwaitRoute) {
        return;
    }
    Attempt_.emplace(UnknownSession{}, localSecret, HelloId_, serverNonce);
    Stage_ = EStage::Handshake;
}

ConnectionResult AgentConnection::Pump(Timestamp now, MutableByteSpan out,
                                       MutableByteSpan scratch) noexcept {
    if (Failed_) {
        return std::unexpected(*Failed_);
    }
    if (auto fits = CheckBuffers(out, scratch); !fits) {
        return std::unexpected(fits.error());
    }
    if (HandshakeExpired(now)) {
        return Refuse(ESessionError::HandshakeTimeout);
    }
    switch (Stage_) {
        case EStage::AwaitHello:
            return PumpHello();
        case EStage::AwaitRoute:
            return NeedsSession{.Id = HelloId_};
        case EStage::Handshake:
            return PumpHandshake(out);
        case EStage::AwaitResume:
            return PumpResume(now, out, scratch);
        case EStage::Live:
            // A newer connection authenticated and took the streams; this one
            // must stop touching them before it sends a byte.
            if (!Handle_->IsCurrent()) {
                return Refuse(ESessionError::Superseded);
            }
            return PumpLive(now, out, scratch, [] {});
    }
    return Idle{};
}

ConnectionResult AgentConnection::PumpHello() noexcept {
    auto next = Frames_.Next();
    if (!next) {
        return Refuse(SessionError{next.error()});
    }
    if (!*next) {
        return Idle{};
    }
    const wire::FrameView& frame = **next;
    if (frame.Header.Epoch != CLEARTEXT_EPOCH) {
        return Refuse(wire::PreAuthError{EProtoError::UnexpectedMessage});
    }
    const auto id = wire::PeekHelloSessionId(frame.Body);
    if (!id) {
        return Refuse(id.error());
    }
    // MAX_HANDSHAKE_FRAME is the stream's limit right now, so the body fits.
    const auto kept = std::span{Hello_}.first(frame.Body.size());
    std::ranges::copy(frame.Body, kept.begin());
    HelloSize_ = frame.Body.size();
    HelloId_ = *id;
    Stage_ = EStage::AwaitRoute;
    return NeedsSession{.Id = HelloId_};
}

ConnectionResult AgentConnection::PumpHandshake(MutableByteSpan out) noexcept {
    ByteSpan body;
    if (!HelloAnswered_) {
        body = ByteSpan{Hello_}.first(HelloSize_);
        HelloAnswered_ = true;
    } else {
        auto next = Frames_.Next();
        if (!next) {
            return Refuse(SessionError{next.error()});
        }
        if (!*next) {
            return Idle{};
        }
        if ((*next)->Header.Epoch != CLEARTEXT_EPOCH) {
            return Refuse(wire::PreAuthError{EProtoError::UnexpectedMessage});
        }
        body = (*next)->Body;
    }

    const auto step = Attempt_->Handle(body, HandshakeBody(out));
    if (!step) {
        return Refuse(step.error());
    }
    if (step->Complete) {
        auto resumed = std::move(*Attempt_).Finish();
        if (!resumed) {
            return Refuse(resumed.error());
        }
        // The peer holds K_auth. It has earned frames of full size, and the
        // keys of both seeds §8 may have it on.
        Frames_.RaiseLimit(wire::MAX_FRAME);
        Current_.emplace(
            MakeDirectionFrames(resumed->Result.Keys, ERole::Agent));
        if (resumed->Result.FallbackKeys) {
            Fallback_.emplace(MakeDirectionFrames(*resumed->Result.FallbackKeys,
                                                  ERole::Agent));
        }
        Handle_.emplace(std::move(resumed->Handle));
        Attempt_.reset();
        Stage_ = EStage::AwaitResume;
    }
    if (step->Written == 0) {
        return Idle{};
    }
    return SendFrame{.Written = FrameCleartext(out, step->Written)};
}

ConnectionResult AgentConnection::PumpResume(Timestamp now, MutableByteSpan out,
                                             MutableByteSpan scratch) noexcept {
    auto next = Frames_.Next();
    if (!next) {
        return Refuse(SessionError{next.error()});
    }
    if (!*next) {
        return Idle{};
    }
    const wire::FrameView& frame = **next;

    // §8: the keys of the current seed first, then those of the seed before
    // it, for a client whose answer to the last Resume was cut. A failed open
    // leaves an opener's counter where it was, so trying both costs nothing
    // but the second AEAD.
    EResumeKeys used = EResumeKeys::Current;
    auto opened = Current_->Opener.Open(scratch, frame);
    if (!opened && opened.error() == EProtoError::AuthenticationFailed &&
        Fallback_) {
        opened = Fallback_->Opener.Open(scratch, frame);
        used = EResumeKeys::Previous;
    }
    if (!opened) {
        return Refuse(SessionError{opened.error()});
    }

    ByteReader reader{*opened};
    const auto type = wire::ParseMessageType(reader);
    if (!type || *type != wire::EMessageType::Resume) {
        return Refuse(SessionError{EProtoError::UnexpectedMessage});
    }
    const auto resume = wire::ParseResume(reader);
    if (!resume) {
        return Refuse(SessionError{resume.error()});
    }
    if (auto confirmed = Handle_->ConfirmResume(used); !confirmed) {
        return Refuse(SessionError{confirmed.error()});
    }
    if (auto attached = Handle_->Attach(resume->ClientReceiveOffset);
        !attached) {
        return Refuse(SessionError{attached.error()});
    }

    DirectionFrames chosen = used == EResumeKeys::Current
                                 ? std::move(*Current_)
                                 : std::move(*Fallback_);
    Current_.reset();
    Fallback_.reset();
    Channel_.emplace(std::move(chosen), Handle_->Output(), Handle_->Input(),
                     now);
    Stage_ = EStage::Live;
    // Straight on: the resend is waiting, and an Idle here would tell the
    // caller there is nothing to do until the client speaks again.
    return PumpLive(now, out, scratch, [] {});
}

ConnectionResult AgentConnection::Refuse(SessionError error) noexcept {
    const SessionError failed = Fail(error);
    // Only an authenticated peer gets this far with KillSession: anything
    // before tag_c came wrapped as PreAuthError and is capped below it.
    if (failed.Disposition() == EDisposition::KillSession && Handle_) {
        if (const auto reason = failed.Session()) {
            Handle_->Kill(*reason);
        }
    }
    return std::unexpected(failed);
}

void AgentConnection::Close(Timestamp now) noexcept {
    if (Handle_) {
        Handle_->Detach(now);
    }
}

// ── ClientConnection ───────────────────────────────────────────────────

ClientConnection::ClientConnection(ClientSession& session,
                                   wire::HandshakeNonce clientNonce,
                                   MutableByteSpan frameStorage,
                                   Timestamp now) noexcept
    : ConnectionBase(frameStorage, now),
      Session_(session),
      Attempt_(session, clientNonce) {}

void ClientConnection::QueueResize(wire::ResizeMessage size) noexcept {
    if (Channel_) {
        Channel_->QueueResize(size);
        return;
    }
    PendingResize_ = size;
}

ConnectionResult ClientConnection::Pump(Timestamp now, MutableByteSpan out,
                                        MutableByteSpan scratch) noexcept {
    if (Failed_) {
        return std::unexpected(*Failed_);
    }
    if (auto fits = CheckBuffers(out, scratch); !fits) {
        return std::unexpected(fits.error());
    }
    if (HandshakeExpired(now)) {
        return Refuse(ESessionError::HandshakeTimeout);
    }
    switch (Stage_) {
        case EStage::SendHello: {
            const auto written = Attempt_.Start(HandshakeBody(out));
            if (!written) {
                return Refuse(written.error());
            }
            Stage_ = EStage::Handshake;
            return SendFrame{.Written = FrameCleartext(out, *written)};
        }
        case EStage::Handshake:
            return PumpHandshake(now, out);
        case EStage::Live:
            if (!Handle_->IsCurrent()) {
                return Refuse(ESessionError::Superseded);
            }
            // §8: the client moves to the next seed once a frame of the
            // agent's opens, which proves the agent opened Resume and moved
            // first. ConfirmAgentFrame does it once per connection.
            return PumpLive(now, out, scratch,
                            [this] { Handle_->ConfirmAgentFrame(); });
    }
    return Idle{};
}

ConnectionResult ClientConnection::PumpHandshake(Timestamp now,
                                                 MutableByteSpan out) noexcept {
    auto next = Frames_.Next();
    if (!next) {
        return Refuse(SessionError{next.error()});
    }
    if (!*next) {
        return Idle{};
    }
    if ((*next)->Header.Epoch != CLEARTEXT_EPOCH) {
        return Refuse(wire::PreAuthError{EProtoError::UnexpectedMessage});
    }
    const auto step = Attempt_.Handle((*next)->Body, HandshakeBody(out));
    if (!step) {
        return Refuse(step.error());
    }
    if (step->Complete) {
        return SendResume(now, out);
    }
    if (step->Written == 0) {
        return Idle{};
    }
    return SendFrame{.Written = FrameCleartext(out, step->Written)};
}

ConnectionResult ClientConnection::SendResume(Timestamp now,
                                              MutableByteSpan out) noexcept {
    // tag_s checked out inside the attempt: this is the first moment the
    // client's streams may move, per §5 rule four.
    auto resumed = std::move(Attempt_).Finish();
    if (!resumed) {
        return Refuse(resumed.error());
    }
    Handle_.emplace(std::move(resumed->Handle));
    if (auto attached = Handle_->Attach(resumed->Result.PeerReceiveOffset);
        !attached) {
        return Refuse(SessionError{attached.error()});
    }
    Frames_.RaiseLimit(wire::MAX_FRAME);

    DirectionFrames frames =
        MakeDirectionFrames(resumed->Result.Keys, ERole::Client);
    // The array is exactly as wide as Resume, so the write cannot fail.
    std::array<std::byte, RESUME_PLAINTEXT_SIZE> plain{};
    ByteWriter writer{MutableByteSpan{plain}};
    const wire::ResumeMessage resume{.ClientReceiveOffset =
                                         Session_.ReceiveOffset()};
    std::ignore = wire::WriteResume(writer, resume);
    // Sealed with the sealer the Channel then takes over, so its first frame
    // goes out under the next nonce rather than this one again.
    const auto sealed = frames.Sealer.Seal(out, writer.Written());
    if (!sealed) {
        return Refuse(SessionError{sealed.error()});
    }
    Channel_.emplace(std::move(frames), Handle_->Output(), Handle_->Input(),
                     now);
    if (PendingResize_) {
        Channel_->QueueResize(*PendingResize_);
        PendingResize_.reset();
    }
    Stage_ = EStage::Live;
    return SendFrame{.Written = *sealed};
}

ConnectionResult ClientConnection::Refuse(SessionError error) noexcept {
    const SessionError failed = Fail(error);
    if (failed.Disposition() == EDisposition::KillSession && Handle_) {
        if (const auto reason = failed.Session()) {
            Handle_->Kill(*reason);
        }
    }
    return std::unexpected(failed);
}

void ClientConnection::Close(Timestamp now) noexcept {
    if (Handle_) {
        Handle_->Detach(now);
    }
}

}  // namespace zet::session
