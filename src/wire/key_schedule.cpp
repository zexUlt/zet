#include "zet/wire/key_schedule.hpp"

#include <array>

#include "zet/core/byte_writer.hpp"

namespace zet::wire {
namespace {

// Eight bytes each, as crypto_kdf demands. Distinct labels are what keep the
// branches of the tree independent: the same master under two of these yields
// keys that say nothing about each other.
constexpr std::string_view SESSION_CONTEXT = "zet-sess";
constexpr std::string_view CONNECTION_CONTEXT = "zet-conn";

static_assert(SESSION_CONTEXT.size() == crypto::CONTEXT_SIZE);
static_assert(CONNECTION_CONTEXT.size() == crypto::CONTEXT_SIZE);

// Subkey numbers within a context. They are part of the wire contract in the
// same way the labels are: change one and both ends stop agreeing.
constexpr std::uint64_t SESSION_ID_SUBKEY = 1;
constexpr std::uint64_t AUTH_SUBKEY = 2;
constexpr std::uint64_t CONNECTION_SEED_SUBKEY = 3;

constexpr std::uint64_t CLIENT_TO_SERVER_SUBKEY = 1;
constexpr std::uint64_t SERVER_TO_CLIENT_SUBKEY = 2;
constexpr std::uint64_t CLIENT_SALT_SUBKEY = 3;
constexpr std::uint64_t SERVER_SALT_SUBKEY = 4;

}  // namespace

ProtoResult<SessionSecrets> DeriveSessionSecrets(
    const crypto::Key& master) noexcept {
    SessionSecrets secrets;

    if (auto derived = crypto::DeriveBytes(MutableByteSpan{secrets.Id}, master,
                                           SESSION_ID_SUBKEY, SESSION_CONTEXT);
        !derived) {
        return std::unexpected(derived.error());
    }

    auto auth = crypto::DeriveSubkey(master, AUTH_SUBKEY, SESSION_CONTEXT);
    if (!auth) {
        return std::unexpected(auth.error());
    }
    secrets.Auth = std::move(*auth);

    auto seed =
        crypto::DeriveSubkey(master, CONNECTION_SEED_SUBKEY, SESSION_CONTEXT);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    secrets.ConnectionSeed = std::move(*seed);

    return secrets;
}

ProtoResult<ConnectionKeys> DeriveConnectionKeys(
    const crypto::Key& connectionSeed, std::uint64_t sequence,
    const HandshakeNonce& clientNonce,
    const HandshakeNonce& serverNonce) noexcept {
    // Both nonces go in, so neither side alone decides what the connection key
    // will be. The sequence number is what separates two connections that
    // happened to exchange identical nonces.
    std::array<std::byte, sizeof(std::uint64_t) + 2 * HANDSHAKE_NONCE_SIZE>
        info{};
    ByteWriter writer{MutableByteSpan{info}};
    if (auto written = writer.WriteU64BE(sequence); !written) {
        return std::unexpected(written.error());
    }
    if (auto written = writer.WriteBytes(ByteSpan{clientNonce}); !written) {
        return std::unexpected(written.error());
    }
    if (auto written = writer.WriteBytes(ByteSpan{serverNonce}); !written) {
        return std::unexpected(written.error());
    }

    const crypto::Key connection =
        crypto::DeriveFromInfo(connectionSeed, ByteSpan{info});

    ConnectionKeys keys;

    auto clientToServer = crypto::DeriveSubkey(
        connection, CLIENT_TO_SERVER_SUBKEY, CONNECTION_CONTEXT);
    if (!clientToServer) {
        return std::unexpected(clientToServer.error());
    }
    keys.ClientToServer = std::move(*clientToServer);

    auto serverToClient = crypto::DeriveSubkey(
        connection, SERVER_TO_CLIENT_SUBKEY, CONNECTION_CONTEXT);
    if (!serverToClient) {
        return std::unexpected(serverToClient.error());
    }
    keys.ServerToClient = std::move(*serverToClient);

    if (auto derived = crypto::DeriveBytes(
            MutableByteSpan{keys.SaltClientToServer}, connection,
            CLIENT_SALT_SUBKEY, CONNECTION_CONTEXT);
        !derived) {
        return std::unexpected(derived.error());
    }
    if (auto derived = crypto::DeriveBytes(
            MutableByteSpan{keys.SaltServerToClient}, connection,
            SERVER_SALT_SUBKEY, CONNECTION_CONTEXT);
        !derived) {
        return std::unexpected(derived.error());
    }

    return keys;
}

}  // namespace zet::wire
