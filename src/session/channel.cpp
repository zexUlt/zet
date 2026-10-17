#include "zet/session/channel.hpp"

#include <algorithm>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace zet::session {

Channel::Channel(const wire::ConnectionKeys& keys, ERole role,
                 SendBuffer& sendBuf, ReceiveCursor& recvCursor, Timestamp now,
                 std::uint64_t frameLimit, std::uint64_t byteLimit) noexcept
    // Secret<> is move-only. Extract the key bytes through Expose() and
    // construct independent Secret values for each direction. Expose() is the
    // project's intentional way of reading key material, and constructing a
    // fresh Secret from those bytes for each channel end is the only way to
    // give two objects the same key without copying the wrapper.
    : Sealer_{crypto::Key{role == ERole::Client ? keys.ClientToServer.Expose()
                                                : keys.ServerToClient.Expose()},
              role == ERole::Client ? keys.SaltClientToServer
                                    : keys.SaltServerToClient,
              frameLimit, byteLimit},
      Opener_{crypto::Key{role == ERole::Client ? keys.ServerToClient.Expose()
                                                : keys.ClientToServer.Expose()},
              role == ERole::Client ? keys.SaltServerToClient
                                    : keys.SaltClientToServer,
              frameLimit},
      SendBuf_{sendBuf},
      RecvCursor_{recvCursor},
      EpochStart_{now},
      LastAckedOffset_{recvCursor.RecvOffset()} {}

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
            return DeliveredData{*delivered, msg->Offset};
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

        // Handshake messages inside a sealed frame are a protocol violation:
        // §7 says the receiver "уже перешедший в запечатанный режим, открытый
        // кадр не принимает". These types here mean a peer sending wrong
        // message types in the sealed stream — close the connection.
        case wire::EMessageType::Hello:
        case wire::EMessageType::Challenge:
        case wire::EMessageType::Auth:
        case wire::EMessageType::AuthOk:
        case wire::EMessageType::Resume:
            return std::unexpected(
                SessionError{EProtoError::UnexpectedMessage});
    }

    // ParseMessageType parses the u8 as a known EMessageType or returns
    // UnknownMessageType. The switch above covers every enumerator. This line
    // is unreachable on a conforming compiler, but avoids a missing-return
    // warning on compilers that do not see through the exhaustive switch.
    ++UnknownTypeCount_;
    return DeliveredNothing{};
}

std::size_t Channel::Poll(Timestamp now, MutableByteSpan out,
                          MutableByteSpan scratch) noexcept {
    // Rekey before sending if the sealer has hit its limits. §8: "by 2^24
    // frames, 1 GiB or 1 hour". A failed rekey (EpochsExhausted) means this
    // connection cannot produce another frame and must be torn down by the
    // caller.
    if (!RekeyIfNeeded(now)) {
        return 0;
    }

    // The minimum frame is HEADER_SIZE + type(1) + TAG_SIZE. Callers that pass
    // a tiny buffer get nothing back rather than an error: the buffer size is
    // their contract with the I/O layer, not ours.
    const std::size_t minFrame =
        wire::HEADER_SIZE + 1 + 16 /*crypto::TAG_SIZE*/;
    if (out.size() < minFrame) {
        return 0;
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
            const std::size_t n =
                SealInto(out, ByteSpan{plain}.first(writer.Size()));
            if (n > 0) {
                PendingPong_.reset();
                return n;
            }
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
            return 0;
        }
        const std::size_t payloadCap =
            std::min(wire::MAX_DATA_PAYLOAD, scratch.size() - DATA_HEADER);
        const std::size_t payloadSize = std::min(payloadCap, SendBuf_.Unsent());
        if (payloadSize > 0) {
            ByteWriter hdrWriter{scratch.first(DATA_HEADER)};
            if (!hdrWriter.WriteU8(
                    static_cast<std::uint8_t>(wire::EMessageType::Data))) {
                return 0;
            }
            if (!hdrWriter.WriteU64BE(RecvCursor_.RecvOffset())) {
                return 0;
            }
            if (!hdrWriter.WriteU64BE(SendBuf_.SendNext())) {
                return 0;
            }
            // Copy payload after the 17-byte header.
            const MutableByteSpan payloadDest =
                scratch.subspan(DATA_HEADER, payloadSize);
            const auto payloadOut =
                SendBuf_.CopyOut(SendBuf_.SendNext(), payloadDest);
            const std::size_t actualPayload = payloadOut.size();
            if (actualPayload == 0) {
                return 0;
            }
            const ByteSpan plaintext =
                ByteSpan{scratch}.first(DATA_HEADER + actualPayload);
            const std::size_t n = SealInto(out, plaintext);
            if (n > 0) {
                SendBuf_.AdvanceSendNext(actualPayload);
                // The Data frame carried a piggybacked Ack. §9.
                LastAckedOffset_ = RecvCursor_.RecvOffset();
                AckPendingSince_.reset();
                return n;
            }
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
            const std::size_t n =
                SealInto(out, ByteSpan{plain}.first(writer.Size()));
            if (n > 0) {
                PendingResize_.reset();
                return n;
            }
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
                const std::size_t n =
                    SealInto(out, ByteSpan{plain}.first(writer.Size()));
                if (n > 0) {
                    LastAckedOffset_ = recvOff;
                    AckPendingSince_.reset();
                    return n;
                }
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

bool Channel::RekeyIfNeeded(Timestamp now) noexcept {
    const bool timeExpired = now >= EpochStart_ + REKEY_INTERVAL;
    if (!Sealer_.NeedsRekey() && !timeExpired) {
        return true;
    }
    const auto result = Sealer_.Rekey();
    if (!result) {
        // EpochsExhausted: the connection has used all 256 key epochs. §8:
        // "only a fresh handshake, which derives new keys, gets the session
        // moving again". Signal failure; the caller tears down the connection.
        return false;
    }
    EpochStart_ = now;
    return true;
}

std::size_t Channel::SealInto(MutableByteSpan out,
                              ByteSpan plaintext) noexcept {
    const auto result = Sealer_.Seal(out, plaintext);
    if (!result) {
        return 0;
    }
    return *result;
}

}  // namespace zet::session
