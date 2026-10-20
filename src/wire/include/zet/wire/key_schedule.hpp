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
///         K_conn = KDF(K_conn_seed, c_nonce ‖ s_nonce)
///                → { K_c2s, K_s2c, salt_c2s, salt_s2c }
///
/// Everything below the master is derived, never transmitted. The master itself
/// leaves neither end of the connection.
namespace zet::wire {

/// Handshake nonce, per §5. Sixteen bytes from each side.
inline constexpr std::size_t HANDSHAKE_NONCE_SIZE = 16;
using HandshakeNonce = std::array<std::byte, HANDSHAKE_NONCE_SIZE>;

/// Bootstrap nonce, per §4: thirty-two bytes from each side. Wider than the
/// handshake's, because these two are all that separates one bootstrap from
/// any other the same machine ever ran — there is no session secret yet to
/// lean on.
inline constexpr std::size_t BOOTSTRAP_NONCE_SIZE = 32;
using BootstrapNonce = std::array<std::byte, BOOTSTRAP_NONCE_SIZE>;

/// Session identifier: a routing label, not a credential. It travels in the
/// clear, and §5 turns on it being useless to whoever reads it.
inline constexpr std::size_t SESSION_ID_SIZE = 16;
using SessionId = std::array<std::byte, SESSION_ID_SIZE>;

/// What a session keeps for its whole life, derived once at bootstrap.
struct SessionSecrets {
    /// SessionIdOf(Registration): public, and checkable by whoever is shown
    /// the registration secret.
    SessionId Id{};
    /// R of §6 and §8: what the agent shows zet-muxd to claim this sid. It
    /// proves ownership of the sid and nothing more — the other secrets come
    /// from the master under other subkeys, not from it.
    crypto::Key Registration;
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

/// The session master out of a bootstrap, per §4 and §8.
///
/// `exchanged` is what X25519 gave both ends — libsodium's kx folds the two
/// public keys into it — and the version and both nonces go in on top, so an
/// Accept from some earlier run, even one that a stale terminal replays word
/// for word, cannot produce the master of this one.
[[nodiscard]] crypto::Key DeriveBootstrapMaster(
    const crypto::Key& exchanged, std::uint16_t version,
    const BootstrapNonce& clientNonce,
    const BootstrapNonce& agentNonce) noexcept;

/// Neither derivation can fail: every length involved is fixed by a type, and
/// the labels are literals the signature checks at compile time. Returning
/// expected here would add branches no caller could ever take.
[[nodiscard]] SessionSecrets DeriveSessionSecrets(
    const crypto::Key& master) noexcept;

/// The sid a registration secret stands for: BLAKE2b-128 of it (§8). What
/// zet-muxd computes to check a registration, with no state of its own.
[[nodiscard]] SessionId SessionIdOf(const crypto::Key& registration) noexcept;

/// Secrets for a `sid` no session answers to, per §5 rule two.
///
/// Deterministic on purpose: a decoy drawn afresh each time is itself the
/// oracle it was meant to remove, because a real session answers two identical
/// probes alike and a random one does not. `localSecret` is made when the agent
/// starts and dies with it — there is nothing worth remembering here between
/// runs. The returned `Id` is the sid asked about, so that what the automaton
/// receives is the same shape either way; its Registration does not hash to
/// that Id, and nothing registers a decoy.
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
