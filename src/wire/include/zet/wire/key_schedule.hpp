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

/// Secrets for a `sid` no session answers to, per §5 rule two.
///
/// Deterministic on purpose: a decoy drawn afresh each time is itself the
/// oracle it was meant to remove, because a real session answers two identical
/// probes alike and a random one does not. `localSecret` is made when the agent
/// starts and dies with it — there is nothing worth remembering here between
/// runs. The returned `Id` is the sid asked about, so that what the automaton
/// receives is the same shape either way.
[[nodiscard]] SessionSecrets DeriveDecoySecrets(const crypto::Key& localSecret,
                                                const SessionId& id) noexcept;

/// Both nonces go in, and only they: thirty-two bytes neither side chooses
/// alone are what keeps two connections of a session apart.
[[nodiscard]] ConnectionKeys DeriveConnectionKeys(
    const crypto::Key& connectionSeed, const HandshakeNonce& clientNonce,
    const HandshakeNonce& serverNonce) noexcept;

/// The seed for the next connection, per §8. Whoever reads the returned key out
/// of memory cannot walk back to the connections that came before it, which is
/// the whole point: sid and both nonces travel in the clear, so a seed that
/// outlived the session would decrypt every recorded connection of it.
///
/// The caller wipes the old seed only once the new generation is confirmed —
/// the two-phase change of §8. Deriving it is this function; deciding when it
/// becomes the only one is session state and lives with the session.
[[nodiscard]] crypto::Key AdvanceConnectionSeed(
    const crypto::Key& connectionSeed) noexcept;

}  // namespace zet::wire
