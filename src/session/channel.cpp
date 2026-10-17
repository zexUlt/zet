#include "zet/session/channel.hpp"

#include <algorithm>
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
        .Opener = wire::FrameOpener{
            Clone(client ? keys.ServerToClient : keys.ClientToServer),
            client ? keys.SaltServerToClient : keys.SaltClientToServer,
            frameLimit}};
}

Channel::Channel(DirectionFrames frames, SendBuffer& sendBuf,
                 ReceiveCursor& recvCursor, Timestamp now) noexcept
    : Sealer_{std::move(frames.Sealer)},
      Opener_{std::move(frames.Opener)},
      SendBuf_{sendBuf},
      RecvCursor_{recvCursor},
      EpochStart_{now},
      LastAckedOffset_{recvCursor.RecvOffset()} {}

Channel::Channel(const wire::ConnectionKeys& keys, ERole role,
                 SendBuffer& sendBuf, ReceiveCursor& recvCursor, Timestamp now,
                 std::uint64_t frameLimit, std::uint64_t byteLimit) noexcept
    : Channel(MakeDirectionFrames(keys, role, frameLimit, byteLimit), sendBuf,
              recvCursor, now) {}

ChannelResult<IncomingEvent> Channel::Receive(
    const wire::FrameView& frame, MutableByteSpan scratch) noexcept {
    // Open the frame. A tag failure does not move the opener's counter per §8:
    // "A frame that fails to open leaves the counter untouched". This means a
    // bit flip from the network closes the connection but the session survives.
    const auto opened = Opener_.Open(scratch, frame);
    if (!opened) {
        return std::unexpected(SessionError{opened.error()});
    }

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
            const auto msg = wire::ParseData(reader);
            if (!msg) {
                return std::unexpected(SessionError{msg.error()});
            }
            // Process the piggybacked Ack before the payload: an ack error
            // means the peer's state is inconsistent, and we should not
            // advance our receive cursor on data from a peer we can no longer
            // trust. CloseConnection per error.hpp / DispositionOf. §9.
            if (const auto ackRes = SendBuf_.Acknowledge(msg->Ack); !ackRes) {
                return std::unexpected(SessionError{ackRes.error()});
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
            if (const auto ackRes = SendBuf_.Acknowledge(msg->Ack); !ackRes) {
                return std::unexpected(SessionError{ackRes.error()});
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
            // We sent a Ping; the peer echoed it. Nothing further to deliver.
            return DeliveredNothing{};
        }

        // Resume is the one sealed message that is not the channel's: the
        // connection opens it before a Channel exists. The rest are handshake
        // messages, which belong to cleartext frames.
        case wire::EMessageType::Hello:
        case wire::EMessageType::Challenge:
        case wire::EMessageType::Auth:
        case wire::EMessageType::AuthOk:
        case wire::EMessageType::Resume:
            break;
    }

    // Known, but not allowed here. Unlike an unknown type this is not a newer
    // peer talking past us; it is a peer out of step with the protocol.
    return std::unexpected(SessionError{EProtoError::UnexpectedMessage});
}

ChannelResult<std::size_t> Channel::Poll(Timestamp now, MutableByteSpan out,
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

    // Priority 2: Data from the send cursor. A Data frame always carries the
    // current receive offset as a piggybacked Ack, so sending Data also
    // acknowledges the peer's stream. §9.
    if (SendBuf_.Unsent() > 0) {
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
            // arm the idle-Ack timer.
            AckPendingSince_ = now;
            // Return 0: the Ack will be sent when the deadline fires.
            return 0;
        }
        if (now >= *AckPendingSince_ + ACK_IDLE_DELAY) {
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

    return Earliest(ackDeadline, rekeyDeadline);
}

void Channel::QueueResize(wire::ResizeMessage resize) noexcept {
    PendingResize_ = resize;
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

}  // namespace zet::session
