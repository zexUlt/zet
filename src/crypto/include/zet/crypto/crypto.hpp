#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/core/secret.hpp"

/// Every bit of cryptography in the project goes through here and nowhere else.
///
/// libsodium is called from this module only, so swapping it for another
/// library is work inside one implementation file rather than across the whole
/// tree. The sizes are restated as our own constants so that libsodium headers
/// do not leak into the rest of the code.
namespace zet::crypto {

inline constexpr std::size_t KEY_SIZE = 32;
inline constexpr std::size_t PUBLIC_KEY_SIZE = 32;
inline constexpr std::size_t NONCE_SIZE = 24;
inline constexpr std::size_t TAG_SIZE = 16;
inline constexpr std::size_t MAC_SIZE = 32;

/// KDF context: exactly eight bytes, as crypto_kdf_derive_from_key demands.
inline constexpr std::size_t CONTEXT_SIZE = 8;

using KeyBytes = std::array<std::byte, KEY_SIZE>;
using Key = Secret<KeyBytes>;
using PublicKey = std::array<std::byte, PUBLIC_KEY_SIZE>;
using Mac = std::array<std::byte, MAC_SIZE>;

struct KeyPair {
    PublicKey Public{};
    Key Private;
};

/// The directions mirror: what the client encrypts with, the server decrypts.
struct SessionKeys {
    Key Receive;
    Key Transmit;
};

/// Initialises libsodium. Must be called before anything else in this module
/// and before a second thread exists.
[[nodiscard]] bool Init() noexcept;

void RandomBytes(MutableByteSpan out) noexcept;

[[nodiscard]] KeyPair GenerateKeyPair() noexcept;

[[nodiscard]] ProtoResult<SessionKeys> DeriveClientKeys(
    const KeyPair& own, const PublicKey& peer) noexcept;

[[nodiscard]] ProtoResult<SessionKeys> DeriveServerKeys(
    const KeyPair& own, const PublicKey& peer) noexcept;

/// A subkey from a master. `context` is exactly CONTEXT_SIZE characters; it
/// separates key purposes, so the same master under different contexts yields
/// independent subkeys.
[[nodiscard]] ProtoResult<Key> DeriveSubkey(const Key& master,
                                            std::uint64_t subkeyId,
                                            std::string_view context) noexcept;

/// Encrypts and authenticates. `out` must be TAG_SIZE longer than the
/// plaintext; returns the actual ciphertext length, tag included.
[[nodiscard]] ProtoResult<std::size_t> AeadSeal(MutableByteSpan out,
                                                ByteSpan plaintext,
                                                ByteSpan associatedData,
                                                const Key& key,
                                                ByteSpan nonce) noexcept;

/// Verifies the tag and decrypts. A failed tag is `AuthenticationFailed`: close
/// the connection, not the session, and certainly not the process.
[[nodiscard]] ProtoResult<std::size_t> AeadOpen(MutableByteSpan out,
                                                ByteSpan ciphertext,
                                                ByteSpan associatedData,
                                                const Key& key,
                                                ByteSpan nonce) noexcept;

[[nodiscard]] Mac Authenticate(ByteSpan message, const Key& key) noexcept;

/// Constant-time comparison — otherwise the tag is guessable by timing.
[[nodiscard]] bool VerifyMac(const Mac& mac, ByteSpan message,
                             const Key& key) noexcept;

}  // namespace zet::crypto
