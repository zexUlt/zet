#include "zet/wire/frame.hpp"

namespace zet::wire {

ProtoResult<FrameHeader> ReadFrameHeader(ByteReader& reader) noexcept {
    auto length = reader.ReadU32BE();
    if (!length) {
        return std::unexpected(length.error());
    }
    auto epoch = reader.ReadU8();
    if (!epoch) {
        return std::unexpected(epoch.error());
    }
    return FrameHeader{.Length = *length, .Epoch = *epoch};
}

ProtoResult<void> WriteFrameHeader(ByteWriter& writer,
                                   FrameHeader header) noexcept {
    auto length = writer.WriteU32BE(header.Length);
    if (!length) {
        return std::unexpected(length.error());
    }
    return writer.WriteU8(header.Epoch);
}

ProtoResult<FrameView> ReadFrame(ByteSpan buffer,
                                 std::uint32_t maxBodyLength) noexcept {
    ByteReader reader{buffer};
    auto header = ReadFrameHeader(reader);
    if (!header) {
        return std::unexpected(header.error());
    }

    // До выделения памяти и до попытки прочитать тело. Порядок здесь и есть
    // защита: пир объявляет длину, мы сверяем её с потолком стадии, и только
    // потом смотрим, сколько байт пришло.
    if (header->Length > maxBodyLength) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }

    // Пустое тело не бывает валидным: в нём нет места даже под тег AEAD.
    // Отвергаем на этом слое, чтобы кадр нулевой длины не дошёл до разбора —
    // однобайтовый пакет ровно так уронил роутер EternalTerminal.
    if (header->Length == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }

    auto body = reader.ReadBytes(header->Length);
    if (!body) {
        return std::unexpected(body.error());
    }

    return FrameView{
        .Header = *header,
        .Body = *body,
        .Consumed = HEADER_SIZE + header->Length,
    };
}

}  // namespace zet::wire
