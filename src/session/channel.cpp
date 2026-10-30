#include "zet/session/channel.hpp"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

#include "zet/crypto/crypto.hpp"

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace zet::session {
namespace {

/// A second copy of a direction key for the sealer or opener, which own what
/// they are given. Written straight into the new Secret: its value constructor
/// takes the array by value, and that parameter is a stack copy of the key
/// nobody wipes.
[[nodiscard]] crypto::Key Clone(const crypto::Key& key) noexcept {
    crypto::Key copy;
    copy.Expose() = key.Expose();
    return copy;
}

}  // namespace

DirectionFrames MakeDirectionFrames(const wire::ConnectionKeys& keys,
                                    ERole role, std::uint64_t frameLimit,
                                    std::uint64_t byteLimit) noexcept {
    const bool client = role == ERole::Client;
    return DirectionFrames{
        .Sealer =
            wire::FrameSealer{
                Clone(client ? keys.ClientToServer : keys.ServerToClient),
                client ? keys.SaltClientToServer : keys.SaltServerToClient,
                frameLimit, byteLimit},
        .Opener =
            wire::FrameOpener{
                Clone(client ? keys.ServerToClient : keys.ClientToServer),
                client ? keys.SaltServerToClient : keys.SaltClientToServer,
                frameLimit},
        .Role = role};
}

Channel::Channel(DirectionFrames frames, SendBuffer& sendBuf,
                 ReceiveCursor& recvCursor, Timestamp now,
                 LivenessLimits liveness, std::uint16_t version,
                 MutableByteSpan snapshotBuf) noexcept
    : SendBuf_{sendBuf},
      RecvCursor_{recvCursor},
      EpochStart_{now},
      LastAckedOffset_{recvCursor.RecvOffset()},
      LastHeard_{now},
      LastSent_{now},
      Liveness_{liveness},
      SnapshotBuf_{snapshotBuf},
      Sealer_{std::move(frames.Sealer)},
      Opener_{std::move(frames.Opener)},
      Version_{version},
      Role_{frames.Role} {}

Channel::Channel(const wire::ConnectionKeys& keys, ERole role,
                 SendBuffer& sendBuf, ReceiveCursor& recvCursor, Timestamp now,
                 std::uint64_t frameLimit, std::uint64_t byteLimit,
                 LivenessLimits liveness, std::uint16_t version,
                 MutableByteSpan snapshotBuf) noexcept
    : Channel(MakeDirectionFrames(keys, role, frameLimit, byteLimit), sendBuf,
              recvCursor, now, liveness, version, snapshotBuf) {}

ChannelResult<IncomingEvent> Channel::Receive(
    Timestamp now, const wire::FrameView& frame,
    MutableByteSpan scratch) noexcept {
    // Open the frame. A tag failure does not move the opener's counter per §8:
    // "A frame that fails to open leaves the counter untouched". This means a
    // bit flip from the network closes the connection but the session survives.
    const auto opened = Opener_.Open(scratch, frame);
    if (!opened) {
        return std::unexpected(SessionError{opened.error()});
    }
    // It opened, so the peer is there, whatever the frame turns out to say.
    LastHeard_ = now;

    // The plaintext starts with a type byte (§7).
    ByteReader reader{*opened};
    const auto typeResult = wire::ParseMessageType(reader);
    if (!typeResult) {
        // UnknownMessageType disposition is Ignore per error.hpp; all others
        // are CloseConnection. In the sealed stream, an unrecognised type
        // byte is counted and ignored per §7 rather than closing. We check
        // the disposition to distinguish them.
        if (typeResult.error() == EProtoError::UnknownMessageType) {
            ++UnknownTypeCount_;
            return DeliveredNothing{};
        }
        return std::unexpected(SessionError{typeResult.error()});
    }
    const wire::EMessageType type = *typeResult;

    switch (type) {
        case wire::EMessageType::Data: {
            // Client side: Data from the agent while grid reception is in
            // progress is a protocol violation. §9 violation table.
            if (Role_ == ERole::Client && GridReceiving_.has_value()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            const auto msg = wire::ParseData(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Process the piggybacked Ack before the payload: an ack error
            // means the peer's state is inconsistent, and we should not
            // advance our receive cursor on data from a peer we can no longer
            // trust. CloseConnection per error.hpp / DispositionOf. §9.
            //
            // Agent side: ignore output ACKs while GridApplied is pending:
            // these are stale ACKs from the old generation (ring was cleared).
            if (!GridAwaitingApplied_) {
                if (const auto ackRes = SendBuf_.Acknowledge(msg->Ack);
                    !ackRes) {
                    return std::unexpected(SessionError{ackRes.error()});
                }
            }
            // A resend may straddle recv_offset: the cursor hands back only
            // the new tail, which starts here and not at msg->Offset.
            const std::uint64_t tailOffset = RecvCursor_.RecvOffset();
            // Deliver through the cursor. Returns a sub-span of *opened (which
            // is a sub-span of scratch), so no copy. §9.
            const auto delivered =
                RecvCursor_.Receive(msg->Offset, msg->Payload);
            if (!delivered) {
                return std::unexpected(SessionError{delivered.error()});
            }
            // An empty result is a complete duplicate; harmless per §9:
            // "целиком ниже recv_offset → отбрасывается как дубль досылки".
            if (delivered->empty()) {
                return DeliveredNothing{};
            }
            return DeliveredData{*delivered, tailOffset};
        }

        case wire::EMessageType::Ack: {
            const auto msg = wire::ParseAck(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Agent side: ignore stale output ACKs while awaiting GridApplied.
            if (!GridAwaitingApplied_) {
                if (const auto ackRes = SendBuf_.Acknowledge(msg->Ack);
                    !ackRes) {
                    return std::unexpected(SessionError{ackRes.error()});
                }
            }
            return DeliveredNothing{};
        }

        case wire::EMessageType::Resize: {
            const auto msg = wire::ParseResize(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            return DeliveredResize{*msg};
        }

        case wire::EMessageType::Ping: {
            const auto msg = wire::ParsePing(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Queue a Pong, idempotent: a later Ping replaces an unanswered
            // one. §9: "управляющие сообщения ... обязаны быть идемпотентными".
            PendingPong_ = wire::PongMessage{msg->Token};
            return DeliveredNothing{};
        }

        case wire::EMessageType::Pong: {
            const auto msg = wire::ParsePong(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // We sent a Ping; the peer echoed it. Hearing it is the point,
            // and Receive has noted that already.
            return DeliveredNothing{};
        }

        case wire::EMessageType::SessionEnd: {
            // The agent's to say; an agent hearing it from a client is
            // talking to something out of step.
            if (Role_ != ERole::Client) {
                break;
            }
            const auto msg = wire::ParseSessionEnd(reader, Version_);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            return DeliveredSessionEnd{*msg};
        }

            // ── M6 grid messages (§9 ADR-030)
            // ─────────────────────────────────

        case wire::EMessageType::GridBegin: {
            // Client side only; agent should never receive this direction.
            if (Role_ != ERole::Client) {
                break;
            }
            const auto msg = wire::ParseGridBegin(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Violation: second GridBegin before GridEnd.
            if (GridReceiving_.has_value()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: total_len > SNAPSHOT_MAX_BYTES or exceeds the
            // caller-provided buffer. §1: no allocation in the core; the
            // caller supplies SnapshotBuf_ sized to wire::SNAPSHOT_MAX_BYTES.
            if (msg->TotalLen > wire::SNAPSHOT_MAX_BYTES ||
                msg->TotalLen > SnapshotBuf_.size()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: scrollback_len > total_len.
            if (msg->ScrollbackLen > msg->TotalLen) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: stream_gen ≤ current agent stream gen (generation
            // rollback). §9 violation table.
            if (msg->StreamGen <= AgentStreamGen_) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            GridReceiving_.emplace();
            GridReceiving_->StreamGen = msg->StreamGen;
            GridReceiving_->TotalLen = msg->TotalLen;
            GridReceiving_->ScrollbackLen = msg->ScrollbackLen;
            GridReceiving_->Flags = msg->Flags;
            GridReceiving_->BytesReceived = 0;
            return DeliveredNothing{};
        }

        case wire::EMessageType::GridChunk: {
            if (Role_ != ERole::Client) {
                break;
            }
            const auto msg = wire::ParseGridChunk(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Violation: GridChunk without preceding GridBegin.
            if (!GridReceiving_.has_value()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: empty chunk data.
            if (msg->Data.empty()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: chunk_offset does not match running total.
            if (msg->ChunkOffset != GridReceiving_->BytesReceived) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: chunk would exceed total_len.
            const std::uint32_t remaining =
                GridReceiving_->TotalLen - GridReceiving_->BytesReceived;
            if (msg->Data.size() > remaining) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            std::ranges::copy(
                msg->Data, SnapshotBuf_.data() + GridReceiving_->BytesReceived);
            GridReceiving_->BytesReceived +=
                static_cast<std::uint32_t>(msg->Data.size());
            return DeliveredNothing{};
        }

        case wire::EMessageType::GridEnd: {
            if (Role_ != ERole::Client) {
                break;
            }
            // Violation: GridEnd without GridBegin.
            if (!GridReceiving_.has_value()) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Violation: received_bytes ≠ total_len.
            if (GridReceiving_->BytesReceived != GridReceiving_->TotalLen) {
                return std::unexpected(
                    SessionError{EProtoError::UnexpectedMessage});
            }
            // Snapshot validated. Apply atomically: reset input cursor to 0
            // so Resume carries c_recv_offset = 0 in the new generation. §9.
            RecvCursor_.Reset(0);
            LastAckedOffset_ = 0;
            AckPendingSince_.reset();
            // SnapshotBuf_ already holds the received bytes; just record the
            // stream generation and return a span into it. The span is valid
            // until the next GridBegin (§1: buffer is caller-owned). §9.
            GridReception rec = std::move(*GridReceiving_);
            GridReceiving_.reset();
            AgentStreamGen_ = rec.StreamGen;
            return DeliveredGridSnapshot{
                .Bytes = ByteSpan{SnapshotBuf_.first(rec.TotalLen)},
                .StreamGen = rec.StreamGen,
                .ScrollbackLen = rec.ScrollbackLen,
                .Flags = rec.Flags,
            };
        }

        case wire::EMessageType::GridApplied: {
            // Agent side only.
            if (Role_ != ERole::Agent) {
                break;
            }
            const auto msg = wire::ParseGridApplied(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Clear the grid-in-flight window.
            GridAwaitingApplied_ = false;
            return DeliveredGridApplied{.StreamGen = msg->StreamGen};
        }

        // Resume is the one sealed message that is not the channel's: the
        // connection opens it before a Channel exists. The rest are handshake
        // messages, which belong to cleartext frames.
        case wire::EMessageType::Hello:
        case wire::EMessageType::Challenge:
        case wire::EMessageType::Auth:
        case wire::EMessageType::AuthOk:
        case wire::EMessageType::Resume:
        case wire::EMessageType::BootstrapOffer:
        case wire::EMessageType::BootstrapAccept:
        case wire::EMessageType::MuxRegister:
        case wire::EMessageType::MuxRegistered:
        case wire::EMessageType::MuxRefused:
        case wire::EMessageType::MuxHandoff:
            break;
    }

    // Known, but not allowed here. Unlike an unknown type this is not a newer
    // peer talking past us; it is a peer out of step with the protocol.
    return std::unexpected(SessionError{EProtoError::UnexpectedMessage});
}

ChannelResult<std::size_t> Channel::Poll(Timestamp now, MutableByteSpan out,
                                         MutableByteSpan scratch) noexcept {
    // §9: a path that has gone quiet in both directions, keepalives included,
    // is gone. Checked first so a dead connection does not go on sealing.
    if (Liveness_.PeerSilenceTimeout > Duration::zero() &&
        now >= LastHeard_ + Liveness_.PeerSilenceTimeout) {
        return std::unexpected(SessionError{ESessionError::PeerSilent});
    }
    auto written = PollOnce(now, out, scratch);
    if (written && *written > 0) {
        LastSent_ = now;
    }
    return written;
}

template <std::size_t tSize, typename TWrite>
ChannelResult<std::size_t> Channel::SealControl(MutableByteSpan out,
                                                TWrite&& write) noexcept {
    std::array<std::byte, tSize> plain{};
    ByteWriter writer{MutableByteSpan{plain}};
    if (auto encoded = std::forward<TWrite>(write)(writer); !encoded) {
        return std::unexpected(SessionError{encoded.error()});
    }
    return SealInto(out, writer.Written());
}

ChannelResult<std::size_t> Channel::PollOnce(Timestamp now, MutableByteSpan out,
                                             MutableByteSpan scratch) noexcept {
    // Rekey before sending if the sealer has hit its limits. §8: "by 2^24
    // frames, 1 GiB or 1 hour". A failed rekey (EpochsExhausted) means this
    // connection cannot produce another frame, and the caller has to hear
    // that rather than a zero it would read as "nothing to send".
    if (auto rekeyed = RekeyIfNeeded(now); !rekeyed) {
        return std::unexpected(rekeyed.error());
    }

    // The smallest frame there is: header, type byte, tag. A buffer below it
    // is a defect in the caller, and saying so beats silently sending nothing.
    static constexpr std::size_t MIN_FRAME =
        wire::HEADER_SIZE + 1 + crypto::TAG_SIZE;
    if (out.size() < MIN_FRAME) {
        return std::unexpected(SessionError{EProtoError::BufferTooSmall});
    }

    // Priority 1: a Pong we owe the peer. §9: "управляющие сообщения ...
    // обязаны быть идемпотентными".  The peer's Ping may arrive while we have
    // data to send; the Pong still goes first so we don't leave the peer
    // waiting for a round-trip to learn we are alive.
    if (PendingPong_.has_value()) {
        // Encode type(1) + token(8) = 9 bytes.
        std::array<std::byte, 1 + sizeof(std::uint64_t)> plain{};
        ByteWriter writer{MutableByteSpan{plain}};
        if (wire::WritePong(writer, *PendingPong_)) {
            const auto n = SealInto(out, ByteSpan{plain}.first(writer.Size()));
            if (!n) {
                return std::unexpected(n.error());
            }
            PendingPong_.reset();
            return *n;
        }
    }

    // Priority 2: (Client) GridApplied — sent before Data so the agent can
    // unblock its output ring as quickly as possible. §9 ADR-030.
    if (Role_ == ERole::Client && PendingGridApplied_.has_value()) {
        // type(1) + stream_gen(4) = 5 bytes.
        std::array<std::byte, 1 + sizeof(std::uint32_t)> plain{};
        ByteWriter writer{MutableByteSpan{plain}};
        if (wire::WriteGridApplied(
                writer,
                wire::GridAppliedMessage{.StreamGen = *PendingGridApplied_})) {
            const auto n = SealInto(out, ByteSpan{plain}.first(writer.Size()));
            if (!n) {
                return std::unexpected(n.error());
            }
            PendingGridApplied_.reset();
            return *n;
        }
    }

    // Priority 2: (Agent) Grid snapshot emission — GridBegin → GridChunk* →
    // GridEnd. Sent before Data so the client can apply the new output
    // generation before more data arrives. §9 ADR-030.
    if (Role_ == ERole::Agent && GridEmitting_.has_value()) {
        GridEmission& ge = *GridEmitting_;
        if (!ge.BeginSent) {
            // type(1) + flags(1) + stream_gen(4) + scrollback_len(4) +
            // total_len(4) = 14 bytes.
            std::array<std::byte, 14> plain{};
            ByteWriter writer{MutableByteSpan{plain}};
            if (wire::WriteGridBegin(
                    writer,
                    wire::GridBeginMessage{.Flags = ge.Flags,
                                           .StreamGen = ge.StreamGen,
                                           .ScrollbackLen = ge.ScrollbackLen,
                                           .TotalLen = ge.TotalLen})) {
                const auto n =
                    SealInto(out, ByteSpan{plain}.first(writer.Size()));
                if (!n) {
                    return std::unexpected(n.error());
                }
                ge.BeginSent = true;
                return *n;
            }
        }
        if (ge.BeginSent && ge.BytesSent < ge.TotalLen && !ge.EndSent) {
            // Stage the GridChunk plaintext in scratch.
            // plain = type(1) + offset(4) + data(N); max size =
            // POLL_SCRATCH_SIZE.
            static constexpr std::size_t CHUNK_HDR = 1 + sizeof(std::uint32_t);
            if (scratch.size() < CHUNK_HDR + 1) {
                return std::unexpected(
                    SessionError{EProtoError::BufferTooSmall});
            }
            const std::size_t maxData =
                std::min(wire::GRID_CHUNK_MAX_DATA, scratch.size() - CHUNK_HDR);
            const std::size_t remaining = ge.TotalLen - ge.BytesSent;
            const std::size_t chunkSize = std::min(maxData, remaining);
            ByteWriter writer{scratch.first(CHUNK_HDR + chunkSize)};
            const auto enc = wire::WriteGridChunk(
                writer, wire::GridChunkMessage{.ChunkOffset = ge.BytesSent,
                                               .Data = ge.Snapshot.subspan(
                                                   ge.BytesSent, chunkSize)});
            if (!enc) {
                return std::unexpected(SessionError{enc.error()});
            }
            const auto n =
                SealInto(out, ByteSpan{scratch}.first(writer.Size()));
            if (!n) {
                return std::unexpected(n.error());
            }
            ge.BytesSent += static_cast<std::uint32_t>(chunkSize);
            return *n;
        }
        if (ge.BeginSent && ge.BytesSent == ge.TotalLen && !ge.EndSent) {
            // GridEnd: type(1) = 1 byte.
            std::array<std::byte, 1> plain{};
            ByteWriter writer{MutableByteSpan{plain}};
            if (wire::WriteGridEnd(writer, wire::GridEndMessage{})) {
                const auto n =
                    SealInto(out, ByteSpan{plain}.first(writer.Size()));
                if (!n) {
                    return std::unexpected(n.error());
                }
                ge.EndSent = true;
                GridEmitting_.reset();
                GridAwaitingApplied_ = true;
                return *n;
            }
        }
    }

    // Priority 3: Data from the send cursor. A Data frame always carries the
    // current receive offset as a piggybacked Ack, so sending Data also
    // acknowledges the peer's stream. §9.
    //
    // Agent: suppressed during grid emission window (GridBegin sent but
    // GridApplied not yet received). §9 ADR-030.
    if (SendBuf_.Unsent() > 0 && !(Role_ == ERole::Agent && GridInFlight())) {
        // §1: no allocation in the protocol core — scratch is the caller's
        // buffer. Plaintext layout: type(1) | ack:u64be(8) | offset:u64be(8)
        // | payload(N). We write the 17-byte header into scratch[0..17) with
        // ByteWriter, then copy payload directly into scratch[17..17+N).
        // The two writes are to adjacent regions so the final plaintext is
        // contiguous in scratch.
        static constexpr std::size_t DATA_HEADER =
            1 + 2 * sizeof(std::uint64_t);
        if (scratch.size() < DATA_HEADER + 1) {
            return std::unexpected(SessionError{EProtoError::BufferTooSmall});
        }
        const std::size_t payloadCap =
            std::min(wire::MAX_DATA_PAYLOAD, scratch.size() - DATA_HEADER);
        const std::size_t payloadSize = std::min(payloadCap, SendBuf_.Unsent());
        if (payloadSize > 0) {
            // The writer spans exactly DATA_HEADER bytes, which is what these
            // three writes take, so none of them can fail.
            ByteWriter hdrWriter{scratch.first(DATA_HEADER)};
            std::ignore = hdrWriter.WriteU8(
                static_cast<std::uint8_t>(wire::EMessageType::Data));
            std::ignore = hdrWriter.WriteU64BE(RecvCursor_.RecvOffset());
            std::ignore = hdrWriter.WriteU64BE(SendBuf_.SendNext());
            // Copy payload after the 17-byte header.
            const MutableByteSpan payloadDest =
                scratch.subspan(DATA_HEADER, payloadSize);
            const auto payloadOut =
                SendBuf_.CopyOut(SendBuf_.SendNext(), payloadDest);
            // SendNext lies in [Base, End) whenever Unsent() > 0, so the copy
            // returns exactly payloadSize bytes.
            const std::size_t actualPayload = payloadOut.size();
            const ByteSpan plaintext =
                ByteSpan{scratch}.first(DATA_HEADER + actualPayload);
            const auto n = SealInto(out, plaintext);
            if (!n) {
                return std::unexpected(n.error());
            }
            SendBuf_.AdvanceSendNext(actualPayload);
            // The Data frame carried a piggybacked Ack. §9.
            LastAckedOffset_ = RecvCursor_.RecvOffset();
            AckPendingSince_.reset();
            return *n;
        }
    }

    // Priority 2b: SessionEnd, and only once every byte of the stream is
    // acknowledged — what the client holds is then the whole of it, and the
    // message cannot arrive ahead of a resend. §9.
    if (PendingEnd_ && !EndSent_ && SendBuf_.Size() == 0) {
        // type(1) + kind(1) + value(1).
        const auto n = SealControl<3>(out, [this](ByteWriter& writer) {
            return wire::WriteSessionEnd(writer, *PendingEnd_, Version_);
        });
        if (!n) {
            return std::unexpected(n.error());
        }
        EndSent_ = true;
        return *n;
    }

    // Priority 3: a Resize queued by the client. Sent after Data so that Data
    // carries the piggybacked Ack promptly; Resize is idempotent and the agent
    // will apply whatever dimensions arrive. §9.
    if (PendingResize_.has_value()) {
        // Encode type(1) + rows(2) + cols(2) + xpixel(2) + ypixel(2) = 9 bytes.
        std::array<std::byte, 1 + 4 * sizeof(std::uint16_t)> plain{};
        ByteWriter writer{MutableByteSpan{plain}};
        if (wire::WriteResize(writer, *PendingResize_)) {
            const auto n = SealInto(out, ByteSpan{plain}.first(writer.Size()));
            if (!n) {
                return std::unexpected(n.error());
            }
            PendingResize_.reset();
            return *n;
        }
    }

    // Priority 4: a bare Ack after ACK_IDLE_DELAY of idleness. §9: "отдельным
    // Ack при простое дольше 200 мс". Only sent when there is a receive-offset
    // gap the peer has not heard about.
    const std::uint64_t recvOff = RecvCursor_.RecvOffset();
    if (recvOff > LastAckedOffset_) {
        if (!AckPendingSince_.has_value()) {
            // First Poll that sees un-acked bytes with nothing to send:
            // arm the idle-Ack timer. The Ack goes when the deadline fires.
            AckPendingSince_ = now;
        } else if (now >= *AckPendingSince_ + ACK_IDLE_DELAY) {
            // Encode type(1) + ack(8) = 9 bytes.
            std::array<std::byte, 1 + sizeof(std::uint64_t)> plain{};
            ByteWriter writer{MutableByteSpan{plain}};
            const wire::AckMessage msg{.Ack = recvOff};
            if (wire::WriteAck(writer, msg)) {
                const auto n =
                    SealInto(out, ByteSpan{plain}.first(writer.Size()));
                if (!n) {
                    return std::unexpected(n.error());
                }
                LastAckedOffset_ = recvOff;
                AckPendingSince_.reset();
                return *n;
            }
        }
    }

    // Priority 5: a keepalive once nothing has gone out for a while, so that
    // the peer's silence timer hears us through an idle session. §9.
    if (Liveness_.KeepaliveInterval > Duration::zero() &&
        now >= LastSent_ + Liveness_.KeepaliveInterval) {
        // type(1) + token(8).
        const wire::PingMessage ping{.Token = NextPingToken_};
        const auto n = SealControl<1 + sizeof(std::uint64_t)>(
            out, [&ping](ByteWriter& writer) {
                return wire::WritePing(writer, ping);
            });
        if (!n) {
            return std::unexpected(n.error());
        }
        ++NextPingToken_;
        return *n;
    }

    return 0;
}

Deadline Channel::NextDeadline() const noexcept {
    // Idle-Ack deadline: ACK_IDLE_DELAY after AckPendingSince_. §9.
    const Deadline ackDeadline =
        AckPendingSince_.has_value()
            ? Deadline{*AckPendingSince_ + ACK_IDLE_DELAY}
            : Deadline{};

    // Time-rekey deadline: REKEY_INTERVAL after the epoch began. §8.
    const Deadline rekeyDeadline{EpochStart_ + REKEY_INTERVAL};

    // Liveness, §9: when a keepalive is due, and when silence ends the
    // connection.
    const Deadline keepaliveDeadline =
        Liveness_.KeepaliveInterval > Duration::zero()
            ? Deadline{LastSent_ + Liveness_.KeepaliveInterval}
            : Deadline{};
    const Deadline silenceDeadline =
        Liveness_.PeerSilenceTimeout > Duration::zero()
            ? Deadline{LastHeard_ + Liveness_.PeerSilenceTimeout}
            : Deadline{};

    return Earliest(Earliest(ackDeadline, rekeyDeadline),
                    Earliest(keepaliveDeadline, silenceDeadline));
}

void Channel::QueueResize(wire::ResizeMessage resize) noexcept {
    PendingResize_ = resize;
}

void Channel::QueueSessionEnd(wire::SessionEndMessage end) noexcept {
    if (!PendingEnd_) {
        PendingEnd_ = end;
    }
}

ChannelResult<void> Channel::RekeyIfNeeded(Timestamp now) noexcept {
    const bool timeExpired = now >= EpochStart_ + REKEY_INTERVAL;
    if (!Sealer_.NeedsRekey() && !timeExpired) {
        return {};
    }
    // EpochsExhausted: the connection has used every key epoch. §8: "only a
    // fresh handshake, which derives new keys, gets the session moving
    // again", which is what CloseConnection leads to.
    if (const auto result = Sealer_.Rekey(); !result) {
        return std::unexpected(SessionError{result.error()});
    }
    EpochStart_ = now;
    return {};
}

ChannelResult<std::size_t> Channel::SealInto(MutableByteSpan out,
                                             ByteSpan plaintext) noexcept {
    const auto result = Sealer_.Seal(out, plaintext);
    if (!result) {
        return std::unexpected(SessionError{result.error()});
    }
    return *result;
}

void Channel::StartGrid(ByteSpan snapshot, std::uint8_t flags,
                        std::uint32_t streamGen,
                        std::uint32_t scrollbackLen) noexcept {
    if (Role_ != ERole::Agent) {
        return;
    }
    if (GridEmitting_.has_value() || GridAwaitingApplied_) {
        // Emission already in progress — caller error, ignore.
        return;
    }
    GridEmitting_.emplace();
    GridEmitting_->Snapshot = snapshot;
    GridEmitting_->StreamGen = streamGen;
    GridEmitting_->TotalLen = static_cast<std::uint32_t>(snapshot.size());
    GridEmitting_->ScrollbackLen = scrollbackLen;
    GridEmitting_->Flags = flags;
    GridEmitting_->BytesSent = 0;
    GridEmitting_->BeginSent = false;
    GridEmitting_->EndSent = false;
}

void Channel::QueueGridApplied(std::uint32_t streamGen) noexcept {
    if (Role_ != ERole::Client) {
        return;
    }
    PendingGridApplied_ = streamGen;
}

}  // namespace zet::session
