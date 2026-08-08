#pragma once

#include <cstddef>
#include <cstdint>

namespace zet::wire {

/// Открытая часть кадра: len:u32be ‖ epoch:u8. Целиком идёт в AD.
inline constexpr std::size_t HEADER_SIZE = 5;

/// Потолок длины тела кадра после аутентификации.
///
/// 256 КиБ выбраны так, чтобы вывод даже очень болтливой команды укладывался в
/// единицы кадров, а пир не мог заставить нас держать больше этого на кадр.
inline constexpr std::uint32_t MAX_FRAME = 256U * 1024U;

/// Потолок до аутентификации.
///
/// Хендшейк состоит из коротких сообщений, и до проверки MAC пир не заслужил
/// права занимать память: 4 КиБ хватает с запасом на любой из них вместе с TLV.
inline constexpr std::uint32_t MAX_HANDSHAKE_FRAME = 4U * 1024U;

}  // namespace zet::wire
