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

/// Maximum Resume plaintext: type byte + c_recv_offset(u64) + stream_gen(u32).
/// v1 uses only the first 9 bytes; v2 uses all 13.
constexpr std::size_t RESUME_PLAINTEXT_MAX =
    sizeof(wire::EMessageType) + sizeof(std::uint64_t) + sizeof(std::uint32_t);

static_assert(RESUME_PLAINTEXT_MAX ==
              sizeof(wire::EMessageType) +
                  sizeof(wire::ResumeMessage::ClientReceiveOffset) +
                  sizeof(wire::ResumeMessage::StreamGen));

/// What every cleartext frame must say in its epoch byte (§7).
constexpr std::uint8_t CLEARTEXT_EPOCH = 0;

}  // namespace

// ── ConnectionBase ─────────────────────────────────────────────────────

ConnectionBase::ConnectionBase(MutableByteSpan frameStorage, Timestamp now,
                               std::uint16_t ceiling,
                               MutableByteSpan snapshotBuf) noexcept
    // Until the peer has proved itself it has not earned more than
    // MAX_HANDSHAKE_FRAME of our memory (§7); the limit widens once it has.
    : Frames_(frameStorage, wire::MAX_HANDSHAKE_FRAME),
      Started_(now),
      SnapshotBuf_(snapshotBuf),
      Ceiling_(ceiling) {}

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

template <typename TOnOpened, typename TOnGridSnap>
ConnectionResult ConnectionBase::PumpLive(Timestamp now, MutableByteSpan out,
                                          MutableByteSpan scratch,
                                          TOnOpened&& onOpened,
                                          TOnGridSnap&& onGridSnap) noexcept {
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
        auto event = Channel_->Receive(now, **next, scratch);
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
        if (const auto* end = std::get_if<DeliveredSessionEnd>(&*event)) {
            // kind=2 (Overflow) is a distinct event: the infrastructure
            // killed the session, not the shell.  Callers must handle it
            // separately so they can display a targeted message (§7).
            if (end->End.Kind == wire::ESessionEndKind::Overflow) {
                return SessionOverflowed{};
            }
            return Ended{.End = end->End};
        }
        if (const auto* snap = std::get_if<DeliveredGridSnapshot>(&*event)) {
            // Agent side: DeliveredGridSnapshot should never arrive; the
            // channel already closed the connection on that violation.
            // Client side: caller queues GridApplied and returns ApplySnapshot.
            if (auto r = std::forward<TOnGridSnap>(onGridSnap)(*snap)) {
                return std::move(*r);
            }
        }
        // DeliveredNothing, DeliveredGridApplied → loop; channel already
        // cleared GridAwaitingApplied_ on the agent side.
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

AgentConnection::AgentConnection(MutableByteSpan frameStorage, Timestamp now,
                                 std::uint16_t ceiling) noexcept
    : ConnectionBase(frameStorage, now, ceiling) {}

void AgentConnection::Route(SessionRef session,
                            wire::HandshakeNonce serverNonce) noexcept {
    if (Stage_ != EStage::AwaitRoute) {
        return;
    }
    Attempt_.emplace(session, serverNonce, Ceiling_);
    Stage_ = EStage::Handshake;
}

void AgentConnection::RouteUnknown(const crypto::Key& localSecret,
                                   wire::HandshakeNonce serverNonce) noexcept {
    if (Stage_ != EStage::AwaitRoute) {
        return;
    }
    Attempt_.emplace(UnknownSession{}, localSecret, HelloId_, serverNonce,
                     Ceiling_);
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
        case EStage::AwaitSnapshot:
            if (!Handle_->IsCurrent()) {
                return Refuse(ESessionError::Superseded);
            }
            return PumpAwaitSnapshot(now, out, scratch);
        case EStage::Live: {
            // A newer connection authenticated and took the streams; this one
            // must stop touching them before it sends a byte.
            if (!Handle_->IsCurrent()) {
                return Refuse(ESessionError::Superseded);
            }
            QueueEnd();
            auto result = PumpLive(
                now, out, scratch, [] {},
                [](const DeliveredGridSnapshot&)
                    -> std::optional<ConnectionResult> {
                    // Agent never legitimately receives a GridSnapshot; channel
                    // already enforces this. Nothing to do here.
                    return std::nullopt;
                });
            // Keep LastKnownSize_ in the session up to date for the next
            // reconnect's path A/B cost comparison.
            if (result) {
                if (const auto* resized = std::get_if<Resized>(&*result)) {
                    Handle_->ReportResize(resized->Size);
                }
            }
            return result;
        }
    }
    return Idle{};
}

bool AgentConnection::DeliveredEnd() const noexcept {
    return Stage_ == EStage::Live && Handle_ && Handle_->IsCurrent() &&
           Channel_ && Channel_->SessionEndSent();
}

void AgentConnection::QueueEnd() noexcept {
    // Every Pump rather than once: the shell may end while this connection
    // is live. The channel sends it once the stream is acknowledged.
    if (const auto end = Handle_->EndedWith()) {
        Channel_->QueueSessionEnd(*end);
    }
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

    // Extract GRID_CAPABLE TLV pre-auth; trusted only after SetGridInfo
    // confirms the handshake (§7, TLV registry). Re-parse the stored Hello
    // so we don't rely on frame.Body staying valid across Pumps.
    {
        ByteReader helloReader{ByteSpan{Hello_}.first(HelloSize_)};
        std::ignore = wire::ParseMessageType(helloReader);  // skip type byte
        if (auto hello = wire::ParseHello(helloReader); hello) {
            for (const auto& opt : hello->Options.Options()) {
                if (opt.Type == wire::TLV_GRID_CAPABLE &&
                    opt.Value.size() == 2) {
                    ByteReader vr{opt.Value};
                    if (auto v = vr.ReadU16BE(); v && *v == 1) {
                        HelloGridCapable_ = true;
                    }
                }
            }
        }
    }

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
        NegotiatedVersion_ = resumed->Result.Version;
        Current_.emplace(
            MakeDirectionFrames(resumed->Result.Keys, ERole::Agent));
        if (resumed->Result.FallbackKeys) {
            Fallback_.emplace(MakeDirectionFrames(*resumed->Result.FallbackKeys,
                                                  ERole::Agent));
        }
        Handle_.emplace(std::move(resumed->Handle));
        // Now trusted: the client proved K_auth, so the TLV it claimed is real.
        Handle_->SetGridInfo(NegotiatedVersion_, HelloGridCapable_);
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
    const auto resume = wire::ParseResume(reader, NegotiatedVersion_);
    if (!resume) {
        return Refuse(SessionError{resume.error()});
    }
    if (auto confirmed = Handle_->ConfirmResume(used); !confirmed) {
        return Refuse(SessionError{confirmed.error()});
    }

    // Violation: client claims a generation the agent has never sent.  This
    // cannot happen in honest operation; close the connection rather than
    // silently accepting a corrupt or misbehaving peer.  §9 violation table.
    if (resume->StreamGen > Handle_->SessionGen()) {
        return Refuse(SessionError{EProtoError::MalformedField});
    }

    // Path A/B decision (§9 ADR-030). Feasibility comes first, then cost.
    //   ring_overflowed: the ring filled in Resync mode; path B is mandatory.
    //   stream_gen < session_gen: the client applied a generation the agent has
    //     already superseded; the client must catch up via a new snapshot.
    const bool mandatoryB =
        Handle_->RingOverflowed() || resume->StreamGen < Handle_->SessionGen();

    // Path B is only valid for clients that advertised GRID_CAPABLE and
    // negotiated the grid protocol version.  A non-capable client cannot
    // receive a grid snapshot, so the cost check must be gated on capability.
    const bool gridCapable =
        NegotiatedVersion_ >= wire::PROTOCOL_VERSION_GRID && HelloGridCapable_;

    bool usePathB = mandatoryB;
    if (!usePathB && gridCapable) {
        // Cost check: prefer path B when the replay is more expensive than
        // building a snapshot. The estimate is coarse (bytes per cell) but
        // always well under SNAPSHOT_MAX_BYTES, so it is a safe sentinel.
        const std::uint64_t replayCost =
            Handle_->Output().End() > resume->ClientReceiveOffset
                ? Handle_->Output().End() - resume->ClientReceiveOffset
                : std::uint64_t{0};
        if (const auto size = Handle_->LastKnownSize(); size.has_value()) {
            const auto estimate = SnapshotEstimateBytes(size->Rows, size->Cols);
            usePathB = (replayCost > estimate);
        }
    }

    // Move the chosen direction frames now, before either attach path.
    DirectionFrames chosen = used == EResumeKeys::Current
                                 ? std::move(*Current_)
                                 : std::move(*Fallback_);
    Current_.reset();
    Fallback_.reset();

    if (usePathB) {
        // Path B: open a new output generation. NewGeneration() (called inside
        // SupplySnapshot) will clear the ring; for now we just become live so
        // incoming frames are accepted during the snapshot-build window.
        if (auto attached = Handle_->AttachPathB(); !attached) {
            return Refuse(SessionError{attached.error()});
        }
        Channel_.emplace(std::move(chosen), Handle_->Output(), Handle_->Input(),
                         now, Handle_->Liveness(), NegotiatedVersion_,
                         SnapshotBuf_);
        Stage_ = EStage::AwaitSnapshot;
        const auto size = Handle_->LastKnownSize();
        return NeedsSnapshot{
            .Rows = size ? size->Rows : std::uint16_t{0},
            .Cols = size ? size->Cols : std::uint16_t{0},
        };
    }

    // Path A: byte-identical replay from what the client reports receiving.
    if (auto attached = Handle_->Attach(resume->ClientReceiveOffset);
        !attached) {
        return Refuse(SessionError{attached.error()});
    }
    Channel_.emplace(std::move(chosen), Handle_->Output(), Handle_->Input(),
                     now, Handle_->Liveness(), NegotiatedVersion_,
                     SnapshotBuf_);
    Stage_ = EStage::Live;
    QueueEnd();
    // Straight on: the resend is waiting, and an Idle here would tell the
    // caller there is nothing to do until the client speaks again.
    return PumpLive(
        now, out, scratch, [] {},
        [](const DeliveredGridSnapshot&) -> std::optional<ConnectionResult> {
            return std::nullopt;
        });
}

ConnectionResult AgentConnection::PumpAwaitSnapshot(
    Timestamp now, MutableByteSpan out, MutableByteSpan scratch) noexcept {
    // Process one incoming frame (Resize, keyboard Data, Ping/Pong, Ack), but
    // do not poll — no output goes out until SupplySnapshot clears the ring
    // and starts the grid emission. §9 ADR-030.
    auto next = Frames_.Next();
    if (!next) {
        return std::unexpected(Fail(SessionError{next.error()}));
    }
    if (*next) {
        auto event = Channel_->Receive(now, **next, scratch);
        if (!event) {
            return std::unexpected(Fail(event.error()));
        }
        if (const auto* data = std::get_if<DeliveredData>(&*event)) {
            return Deliver{.Bytes = data->Bytes};
        }
        if (const auto* resize = std::get_if<DeliveredResize>(&*event)) {
            Handle_->ReportResize(resize->Resize);
            return Resized{.Size = resize->Resize};
        }
        // DeliveredSessionEnd, DeliveredNothing, DeliveredGridApplied → fall
        // through; none of these change the snapshot wait.
    }
    (void)out;
    const auto size = Handle_->LastKnownSize();
    return NeedsSnapshot{
        .Rows = size ? size->Rows : std::uint16_t{0},
        .Cols = size ? size->Cols : std::uint16_t{0},
    };
}

void AgentConnection::SupplySnapshot([[maybe_unused]] Timestamp now,
                                     ByteSpan snapshot,
                                     std::uint32_t scrollbackLen,
                                     std::uint8_t flags) noexcept {
    if (Stage_ != EStage::AwaitSnapshot || !Channel_ || !Handle_) {
        return;
    }
    // A superseded connection must not call NewGeneration (which advances the
    // ring) on behalf of a session whose newer owner did not ask for it.
    if (!Handle_->IsCurrent()) {
        return;
    }
    // Clear the ring and open a new output generation, then queue the snapshot
    // for emission. Everything sent after this point is in the new generation.
    Handle_->NewGeneration();
    Channel_->StartGrid(snapshot, flags, Handle_->SessionGen(), scrollbackLen);
    Stage_ = EStage::Live;
    QueueEnd();
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
                                   MutableByteSpan frameStorage, Timestamp now,
                                   bool gridCapable, std::uint16_t ceiling,
                                   MutableByteSpan snapshotBuf) noexcept
    : ConnectionBase(frameStorage, now, ceiling, snapshotBuf),
      Session_(session),
      Attempt_(session, clientNonce, gridCapable, ceiling) {}

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
            {
                auto event = PumpLive(
                    now, out, scratch, [this] { Handle_->ConfirmAgentFrame(); },
                    [this](const DeliveredGridSnapshot& snap)
                        -> std::optional<ConnectionResult> {
                        // Queue GridApplied (next Poll sends it), update the
                        // session's stream gen so the next Resume carries it.
                        Channel_->QueueGridApplied(snap.StreamGen);
                        Handle_->SetStreamGen(snap.StreamGen);
                        return ApplySnapshot{
                            .Bytes = snap.Bytes,
                            .StreamGen = snap.StreamGen,
                            .ScrollbackLen = snap.ScrollbackLen,
                            .Flags = snap.Flags};
                    });
                if (event) {
                    if (const auto* ended = std::get_if<Ended>(&*event)) {
                        Handle_->RecordEnd(ended->End);
                    } else if (std::holds_alternative<SessionOverflowed>(
                                   *event)) {
                        // The channel sent kind=2; record it so EndedWith()
                        // returns it and the caller does not reconnect.
                        Handle_->RecordEnd(
                            {.Kind = wire::ESessionEndKind::Overflow,
                             .Value = 0});
                    }
                }
                return event;
            }
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
    NegotiatedVersion_ = resumed->Result.Version;

    DirectionFrames frames =
        MakeDirectionFrames(resumed->Result.Keys, ERole::Client);
    // The array is wide enough for the largest Resume (v2), so no write fails.
    std::array<std::byte, RESUME_PLAINTEXT_MAX> plain{};
    ByteWriter writer{MutableByteSpan{plain}};
    // At ver=2, stream_gen tells the agent which output generation the client
    // last applied so the agent can decide between path A and path B. §9.
    const wire::ResumeMessage resume{
        .StreamGen = Session_.StreamGen(),
        .ClientReceiveOffset = Session_.ReceiveOffset()};
    const auto written = wire::WriteResume(writer, resume, NegotiatedVersion_);
    if (!written) {
        return Refuse(SessionError{written.error()});
    }
    // Sealed with the sealer the Channel then takes over, so its first frame
    // goes out under the next nonce rather than this one again.
    const auto sealed = frames.Sealer.Seal(out, writer.Written());
    if (!sealed) {
        return Refuse(SessionError{sealed.error()});
    }
    Channel_.emplace(std::move(frames), Handle_->Output(), Handle_->Input(),
                     now, Handle_->Liveness(), NegotiatedVersion_,
                     SnapshotBuf_);
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
