#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/limits.hpp"

namespace zet::wire {

/// Открытая часть кадра. Оба поля покрыты AD, поэтому подмена любого из них
/// проваливает тег, а не приводит к разбору чужой длины.
struct FrameHeader {
    /// Длина тела: ciphertext вместе с тегом.
    std::uint32_t Length{0};
    /// Поколение ключа. Растёт при rekey.
    std::uint8_t Epoch{0};
};

/// Кадр, найденный в буфере. Тело — вид на входные байты, копий не делается.
struct FrameView {
    FrameHeader Header;
    ByteSpan Body;
    /// Сколько байт буфера занял кадр целиком, вместе с заголовком.
    std::size_t Consumed{0};
};

[[nodiscard]] ProtoResult<FrameHeader> ReadFrameHeader(
    ByteReader& reader) noexcept;

[[nodiscard]] ProtoResult<void> WriteFrameHeader(ByteWriter& writer,
                                                 FrameHeader header) noexcept;

/// Выделяет один кадр из начала буфера.
///
/// `maxBodyLength` — потолок стадии: MAX_HANDSHAKE_FRAME до аутентификации,
/// MAX_FRAME после. Длина сверяется с ним до того, как кто-либо решит выделять
/// под неё память.
///
/// `Truncated` означает «данных пока мало, дочитай и позови снова», а не порчу.
[[nodiscard]] ProtoResult<FrameView> ReadFrame(
    ByteSpan buffer, std::uint32_t maxBodyLength) noexcept;

}  // namespace zet::wire
