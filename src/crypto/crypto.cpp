#include "zet/crypto/crypto.hpp"

#include <sodium.h>

#include <tuple>

namespace zet::crypto {
namespace {

// The sizes are duplicated in the header so that libsodium does not leak out.
// This is the only place where the two sets are required to agree.
static_assert(KEY_SIZE == crypto_kx_SESSIONKEYBYTES);
static_assert(KEY_SIZE == crypto_kdf_KEYBYTES);
static_assert(PUBLIC_KEY_SIZE == crypto_kx_PUBLICKEYBYTES);
static_assert(NONCE_SIZE == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(TAG_SIZE == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(MAC_SIZE == crypto_auth_BYTES);
static_assert(CONTEXT_SIZE == crypto_kdf_CONTEXTBYTES);

[[nodiscard]] unsigned char* Raw(MutableByteSpan bytes) noexcept {
    return reinterpret_cast<unsigned char*>(bytes.data());
}

[[nodiscard]] const unsigned char* Raw(ByteSpan bytes) noexcept {
    return reinterpret_cast<const unsigned char*>(bytes.data());
}

[[nodiscard]] const unsigned char* Raw(const KeyBytes& key) noexcept {
    return reinterpret_cast<const unsigned char*>(key.data());
}

[[nodiscard]] unsigned char* Raw(KeyBytes& key) noexcept {
    return reinterpret_cast<unsigned char*>(key.data());
}

}  // namespace

bool Init() noexcept { return sodium_init() >= 0; }

void RandomBytes(MutableByteSpan out) noexcept {
    randombytes_buf(Raw(out), out.size());
}

KeyPair GenerateKeyPair() noexcept {
    KeyPair pair;
    crypto_kx_keypair(reinterpret_cast<unsigned char*>(pair.Public.data()),
                      Raw(pair.Private.Expose()));
    return pair;
}

ProtoResult<SessionKeys> DeriveClientKeys(const KeyPair& own,
                                          const PublicKey& peer) noexcept {
    SessionKeys keys;
    const int rc = crypto_kx_client_session_keys(
        Raw(keys.Receive.Expose()), Raw(keys.Transmit.Expose()),
        reinterpret_cast<const unsigned char*>(own.Public.data()),
        Raw(own.Private.Expose()),
        reinterpret_cast<const unsigned char*>(peer.data()));
    if (rc != 0) {
        // The only way this fails: the peer's public key is not on the curve.
        return std::unexpected(EProtoError::MalformedField);
    }
    return keys;
}

ProtoResult<SessionKeys> DeriveServerKeys(const KeyPair& own,
                                          const PublicKey& peer) noexcept {
    SessionKeys keys;
    const int rc = crypto_kx_server_session_keys(
        Raw(keys.Receive.Expose()), Raw(keys.Transmit.Expose()),
        reinterpret_cast<const unsigned char*>(own.Public.data()),
        Raw(own.Private.Expose()),
        reinterpret_cast<const unsigned char*>(peer.data()));
    if (rc != 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    return keys;
}

Key DeriveSubkey(const Key& master, std::uint64_t subkeyId,
                 const char (&context)[CONTEXT_SIZE + 1]) noexcept {
    Key subkey;
    // The only way crypto_kdf_derive_from_key refuses is a length outside its
    // bounds, and every length here comes from a type: KEY_SIZE is 32, the
    // context is a literal of exactly CONTEXT_SIZE characters.
    std::ignore =
        crypto_kdf_derive_from_key(Raw(subkey.Expose()), KEY_SIZE, subkeyId,
                                   context, Raw(master.Expose()));
    return subkey;
}

void DeriveBytesUnchecked(MutableByteSpan out, const Key& master,
                          std::uint64_t subkeyId,
                          const char (&context)[CONTEXT_SIZE + 1]) noexcept {
    std::ignore = crypto_kdf_derive_from_key(Raw(out), out.size(), subkeyId,
                                             context, Raw(master.Expose()));
}

Key DeriveFromInfo(const Key& master, ByteSpan info) noexcept {
    // Keyed BLAKE2b, the same construction Noise and WireGuard use to fold
    // handshake data into a key. crypto_kdf cannot: its inputs are a counter
    // and a fixed label, neither of which the peers can contribute to.
    Key derived;
    crypto_generichash(Raw(derived.Expose()), KEY_SIZE, Raw(info), info.size(),
                       Raw(master.Expose()), KEY_SIZE);
    return derived;
}

ProtoResult<std::size_t> AeadSeal(MutableByteSpan out, ByteSpan plaintext,
                                  ByteSpan associatedData, const Key& key,
                                  ByteSpan nonce) noexcept {
    if (nonce.size() != NONCE_SIZE) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (out.size() < plaintext.size() + TAG_SIZE) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }

    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        Raw(out), &written, Raw(plaintext), plaintext.size(),
        Raw(associatedData), associatedData.size(), nullptr, Raw(nonce),
        Raw(key.Expose()));
    if (rc != 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    return static_cast<std::size_t>(written);
}

ProtoResult<std::size_t> AeadOpen(MutableByteSpan out, ByteSpan ciphertext,
                                  ByteSpan associatedData, const Key& key,
                                  ByteSpan nonce) noexcept {
    if (nonce.size() != NONCE_SIZE) {
        return std::unexpected(EProtoError::MalformedField);
    }
    // A ciphertext shorter than the tag cannot be valid. Checked before the
    // call, otherwise the length would go into an unsigned subtraction.
    if (ciphertext.size() < TAG_SIZE) {
        return std::unexpected(EProtoError::AuthenticationFailed);
    }
    if (out.size() < ciphertext.size() - TAG_SIZE) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }

    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        Raw(out), &written, nullptr, Raw(ciphertext), ciphertext.size(),
        Raw(associatedData), associatedData.size(), Raw(nonce),
        Raw(key.Expose()));
    if (rc != 0) {
        return std::unexpected(EProtoError::AuthenticationFailed);
    }
    return static_cast<std::size_t>(written);
}

Mac Authenticate(ByteSpan message, const Key& key) noexcept {
    Mac mac{};
    crypto_auth(reinterpret_cast<unsigned char*>(mac.data()), Raw(message),
                message.size(), Raw(key.Expose()));
    return mac;
}

bool VerifyMac(const Mac& mac, ByteSpan message, const Key& key) noexcept {
    return crypto_auth_verify(
               reinterpret_cast<const unsigned char*>(mac.data()), Raw(message),
               message.size(), Raw(key.Expose())) == 0;
}

}  // namespace zet::crypto
