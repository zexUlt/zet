#include "zet/wire/key_schedule.hpp"

#include <array>
#include <tuple>

#include "zet/core/byte_writer.hpp"

namespace zet::wire {
namespace {

// Eight characters each, which KdfContext enforces at compile time. Distinct
// labels are what keep the branches of the tree independent: the same master
// under two of these yields keys that say nothing about each other.
constexpr crypto::KdfContext SESSION_CONTEXT = std::to_array("zet-sess");
constexpr crypto::KdfContext CONNECTION_CONTEXT = std::to_array("zet-conn");
constexpr crypto::KdfContext SEED_RATCHET_CONTEXT = std::to_array("zet-rtch");

// The ratchet has one output, so its subkey number carries no choice.
constexpr std::uint64_t SEED_RATCHET_SUBKEY = 1;

// Subkey numbers within a context. They are as much part of the wire contract
// as the labels: change one and the two ends stop agreeing.
constexpr std::uint64_t SESSION_ID_SUBKEY = 1;
constexpr std::uint64_t AUTH_SUBKEY = 2;
constexpr std::uint64_t CONNECTION_SEED_SUBKEY = 3;

constexpr std::uint64_t CLIENT_TO_SERVER_SUBKEY = 1;
constexpr std::uint64_t SERVER_TO_CLIENT_SUBKEY = 2;
constexpr std::uint64_t CLIENT_SALT_SUBKEY = 3;
constexpr std::uint64_t SERVER_SALT_SUBKEY = 4;

}  // namespace

SessionSecrets DeriveSessionSecrets(const crypto::Key& master) noexcept {
    SessionSecrets secrets;
    crypto::DeriveBytes(secrets.Id, master, SESSION_ID_SUBKEY, SESSION_CONTEXT);
    secrets.Auth = crypto::DeriveSubkey(master, AUTH_SUBKEY, SESSION_CONTEXT);
    secrets.ConnectionSeed =
        crypto::DeriveSubkey(master, CONNECTION_SEED_SUBKEY, SESSION_CONTEXT);
    return secrets;
}

SessionSecrets DeriveDecoySecrets(const crypto::Key& localSecret,
                                  const SessionId& id) noexcept {
    // The sid is what varies, so it goes in as info rather than as a subkey
    // number: crypto_kdf takes a counter, and sixteen bytes do not fit in one.
    const crypto::Key master =
        crypto::DeriveFromInfo(localSecret, ByteSpan{id});

    SessionSecrets secrets = DeriveSessionSecrets(master);
    secrets.Id = id;
    return secrets;
}

ConnectionKeys DeriveConnectionKeys(
    const crypto::Key& connectionSeed, const HandshakeNonce& clientNonce,
    const HandshakeNonce& serverNonce) noexcept {
    // Both nonces go in, so neither side alone decides what the connection key
    // will be, and they go in a fixed order: folded symmetrically, a peer could
    // hand back what it received and land on the same key.
    //
    // The buffer is exactly as wide as what goes into it, so the writes cannot
    // fail and their results are discarded rather than checked.
    std::array<std::byte, 2 * HANDSHAKE_NONCE_SIZE> info{};
    ByteWriter writer{MutableByteSpan{info}};
    std::ignore = writer.WriteBytes(ByteSpan{clientNonce});
    std::ignore = writer.WriteBytes(ByteSpan{serverNonce});

    const crypto::Key connection =
        crypto::DeriveFromInfo(connectionSeed, ByteSpan{info});

    ConnectionKeys keys;
    keys.ClientToServer = crypto::DeriveSubkey(
        connection, CLIENT_TO_SERVER_SUBKEY, CONNECTION_CONTEXT);
    keys.ServerToClient = crypto::DeriveSubkey(
        connection, SERVER_TO_CLIENT_SUBKEY, CONNECTION_CONTEXT);
    crypto::DeriveBytes(keys.SaltClientToServer, connection, CLIENT_SALT_SUBKEY,
                        CONNECTION_CONTEXT);
    crypto::DeriveBytes(keys.SaltServerToClient, connection, SERVER_SALT_SUBKEY,
                        CONNECTION_CONTEXT);
    return keys;
}

crypto::Key AdvanceConnectionSeed(const crypto::Key& connectionSeed) noexcept {
    return crypto::DeriveSubkey(connectionSeed, SEED_RATCHET_SUBKEY,
                                SEED_RATCHET_CONTEXT);
}

}  // namespace zet::wire
