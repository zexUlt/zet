#include "zet/wire/key_schedule.hpp"

#include <array>
#include <tuple>

#include "zet/core/byte_writer.hpp"

namespace zet::wire {
namespace {

// Eight characters each, which the derivation signature enforces at compile
// time. Distinct labels are what keep the branches of the tree independent: the
// same master under two of these yields keys that say nothing about each other.
constexpr char SESSION_CONTEXT[] = "zet-sess";
constexpr char CONNECTION_CONTEXT[] = "zet-conn";

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

ConnectionKeys DeriveConnectionKeys(
    const crypto::Key& connectionSeed, std::uint64_t sequence,
    const HandshakeNonce& clientNonce,
    const HandshakeNonce& serverNonce) noexcept {
    // Both nonces go in, so neither side alone decides what the connection key
    // will be, and they go in a fixed order: folded symmetrically, a peer could
    // hand back what it received and land on the same key. The sequence number
    // separates two connections that happened to exchange identical nonces.
    //
    // The buffer is exactly as wide as what goes into it, so the writes cannot
    // fail and their results are discarded rather than checked.
    std::array<std::byte, sizeof(std::uint64_t) + 2 * HANDSHAKE_NONCE_SIZE>
        info{};
    ByteWriter writer{MutableByteSpan{info}};
    std::ignore = writer.WriteU64BE(sequence);
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

}  // namespace zet::wire
