#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/core/secret.hpp"

/// Вся криптография проекта проходит здесь и больше нигде.
///
/// libsodium вызывается только из этого модуля, поэтому замена её на другую
/// библиотеку — работа в границах одного файла реализации, а не по всему
/// дереву. Размеры продублированы своими константами, чтобы заголовки libsodium
/// не протекали в остальной код.
namespace zet::crypto {

inline constexpr std::size_t KEY_SIZE = 32;
inline constexpr std::size_t PUBLIC_KEY_SIZE = 32;
inline constexpr std::size_t NONCE_SIZE = 24;
inline constexpr std::size_t TAG_SIZE = 16;
inline constexpr std::size_t MAC_SIZE = 32;

/// Контекст KDF: ровно восемь байт, требование crypto_kdf_derive_from_key.
inline constexpr std::size_t CONTEXT_SIZE = 8;

using KeyBytes = std::array<std::byte, KEY_SIZE>;
using Key = Secret<KeyBytes>;
using PublicKey = std::array<std::byte, PUBLIC_KEY_SIZE>;
using Mac = std::array<std::byte, MAC_SIZE>;

struct KeyPair {
    PublicKey Public{};
    Key Private;
};

/// Направления зеркальны: то, чем клиент шифрует, сервер расшифровывает.
struct SessionKeys {
    Key Receive;
    Key Transmit;
};

/// Инициализирует libsodium. Должна быть вызвана до всего остального в этом
/// модуле и до появления второго потока.
[[nodiscard]] bool Init() noexcept;

void RandomBytes(MutableByteSpan out) noexcept;

[[nodiscard]] KeyPair GenerateKeyPair() noexcept;

[[nodiscard]] ProtoResult<SessionKeys> DeriveClientKeys(
    const KeyPair& own, const PublicKey& peer) noexcept;

[[nodiscard]] ProtoResult<SessionKeys> DeriveServerKeys(
    const KeyPair& own, const PublicKey& peer) noexcept;

/// Подключ из мастера. `context` — ровно CONTEXT_SIZE символов; он разделяет
/// назначения ключей, поэтому один и тот же мастер с разными контекстами даёт
/// независимые подключи.
[[nodiscard]] ProtoResult<Key> DeriveSubkey(const Key& master,
                                            std::uint64_t subkeyId,
                                            std::string_view context) noexcept;

/// Шифрует и аутентифицирует. `out` должен быть на TAG_SIZE длиннее открытого
/// текста; возвращается фактическая длина шифротекста вместе с тегом.
[[nodiscard]] ProtoResult<std::size_t> AeadSeal(MutableByteSpan out,
                                                ByteSpan plaintext,
                                                ByteSpan associatedData,
                                                const Key& key,
                                                ByteSpan nonce) noexcept;

/// Проверяет тег и расшифровывает. Провал тега — `AuthenticationFailed`, и это
/// повод закрыть соединение, а не сессию и тем более не процесс.
[[nodiscard]] ProtoResult<std::size_t> AeadOpen(MutableByteSpan out,
                                                ByteSpan ciphertext,
                                                ByteSpan associatedData,
                                                const Key& key,
                                                ByteSpan nonce) noexcept;

[[nodiscard]] Mac Authenticate(ByteSpan message, const Key& key) noexcept;

/// Сравнение постоянного времени — иначе тег подбирается по таймингу.
[[nodiscard]] bool VerifyMac(const Mac& mac, ByteSpan message,
                             const Key& key) noexcept;

}  // namespace zet::crypto
