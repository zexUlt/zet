#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/sealed_frame.hpp"

/// The key tree from docs/design.md §8.
///
///     X25519 shared → K_master → { sid, K_auth, K_conn_seed }
///     per connection:
///         K_conn = KDF(K_conn_seed, seq ‖ c_nonce ‖ s_nonce)
///                → { K_c2s, K_s2c, salt_c2s, salt_s2c }
///
/// Everything below the master is derived, never transmitted. The master itself
/// leaves neither end of the connection.
namespace zet::wire {

/// Handshake nonce, per §5. Sixteen bytes from each side.
inline constexpr std::size_t HANDSHAKE_NONCE_SIZE = 16;
using HandshakeNonce = std::array<std::byte, HANDSHAKE_NONCE_SIZE>;

/// Session identifier: a routing label, not a credential. It travels in the
/// clear, and §5 turns on it being useless to whoever reads it.
inline constexpr std::size_t SESSION_ID_SIZE = 16;
using SessionId = std::array<std::byte, SESSION_ID_SIZE>;

/// What a session keeps for its whole life, derived once at bootstrap.
struct SessionSecrets {
    SessionId Id{};
    /// Proves knowledge of the session when resuming, per §5. Never used to
    /// encrypt anything.
    crypto::Key Auth;
    /// The root every connection's keys grow from.
    crypto::Key ConnectionSeed;
};

/// What one TCP connection encrypts with. Fresh for every connection, which is
/// what removes the whole class of nonce-reuse problems across reconnects.
struct ConnectionKeys {
    crypto::Key ClientToServer;
    crypto::Key ServerToClient;
    DirectionSalt SaltClientToServer{};
    DirectionSalt SaltServerToClient{};
};

/// Neither derivation can fail: every length involved is fixed by a type, and
/// the labels are literals the signature checks at compile time. Returning
/// expected here would add branches no caller could ever take.
[[nodiscard]] SessionSecrets DeriveSessionSecrets(
    const crypto::Key& master) noexcept;

/// `sequence` counts connections within the session and never repeats: it is
/// what keeps two connections that happened to exchange the same nonces from
/// arriving at the same keys.
[[nodiscard]] ConnectionKeys DeriveConnectionKeys(
    const crypto::Key& connectionSeed, std::uint64_t sequence,
    const HandshakeNonce& clientNonce,
    const HandshakeNonce& serverNonce) noexcept;

}  // namespace zet::wire
