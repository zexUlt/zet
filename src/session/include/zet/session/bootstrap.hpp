#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/wire/armor.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

/// The bootstrap of docs/design.md §4, both ends, sans-I/O.
///
///     client ── Offer  (c_pub, c_nonce, versions) ──▶ agent stdin
///     client ◀── Accept (a_pub, c_nonce, a_nonce, version, port) ── agent
///     stdout
///
/// Each message is a cleartext frame in armor. X25519 over a channel ssh has
/// already authenticated both ways needs no signatures; what it needs is to
/// pick the right Accept out of a stdout full of noise, and the echoed nonce
/// is how. Every key and nonce is handed in: nothing here reads a clock or a
/// random generator, which is what lets the fuzzer and the tests fix them.
namespace zet::session {

/// Body sizes: the type byte and the fields of §7. An Offer's is the most
/// it can be, with every variable at its longest.
inline constexpr std::size_t BOOTSTRAP_OFFER_SIZE =
    sizeof(std::uint8_t) + 2 * sizeof(std::uint16_t) + crypto::PUBLIC_KEY_SIZE +
    wire::BOOTSTRAP_NONCE_SIZE + sizeof(std::uint8_t) +
    wire::MAX_BOOTSTRAP_ENV *
        (2 * sizeof(std::uint8_t) + wire::MAX_ENV_NAME + wire::MAX_ENV_VALUE);
inline constexpr std::size_t BOOTSTRAP_ACCEPT_SIZE =
    sizeof(std::uint8_t) + sizeof(std::uint16_t) + crypto::PUBLIC_KEY_SIZE +
    2 * wire::BOOTSTRAP_NONCE_SIZE + sizeof(std::uint16_t) +
    sizeof(std::uint32_t);

/// Room WriteOffer and WriteAccept need: the whole frame, armored.
inline constexpr std::size_t BOOTSTRAP_OFFER_TEXT_SIZE =
    wire::ArmoredSize(wire::HEADER_SIZE + BOOTSTRAP_OFFER_SIZE);
inline constexpr std::size_t BOOTSTRAP_ACCEPT_TEXT_SIZE =
    wire::ArmoredSize(wire::HEADER_SIZE + BOOTSTRAP_ACCEPT_SIZE);

static_assert(wire::HEADER_SIZE + BOOTSTRAP_ACCEPT_SIZE <=
              wire::MAX_ARMORED_FRAME);
static_assert(wire::HEADER_SIZE + BOOTSTRAP_OFFER_SIZE <=
              wire::MAX_ARMORED_FRAME);

/// What a finished bootstrap leaves either end with.
struct BootstrapResult {
    wire::SessionSecrets Secrets;
    std::uint16_t Version{0};
    /// Where the agent listens. The agent knew it all along; the client
    /// learns it from the Accept.
    std::uint16_t Port{0};
    /// How long the agent keeps the session without a client; 0 is for ever.
    /// Again the agent's to set and the client's to learn.
    std::uint32_t OrphanTimeoutSeconds{0};
    /// What the agent's shell is to get: the client's TERM and locale. The
    /// client's own, on the client.
    wire::BootstrapEnvironment Environment{};
};

/// What one Feed did: how much of the chunk it read, and the result if this
/// chunk finished the bootstrap. Bytes past Consumed are not the bootstrap's.
struct BootstrapStep {
    std::size_t Consumed{0};
    std::optional<BootstrapResult> Result;
};

/// The client's end: writes the Offer, then reads the agent's stdout.
class BootstrapClient {
public:
    /// `seed` determines the ephemeral key pair; draw it fresh for every
    /// bootstrap, like `nonce`.
    /// `environment` is what the agent's shell will be given (§4).
    BootstrapClient(
        const crypto::Key& seed, const wire::BootstrapNonce& nonce,
        const wire::BootstrapEnvironment& environment = {}) noexcept;

    /// The armored Offer, BOOTSTRAP_OFFER_TEXT_SIZE characters.
    [[nodiscard]] ProtoResult<std::size_t> WriteOffer(
        std::span<char> out) const noexcept;

    /// Reads agent output until an Accept that echoes our nonce.
    ///
    /// Blocks that are not that — banners shaped like armor, an Accept from
    /// another run, frames of other types — are skipped and counted, never an
    /// error: §4 expects them. Errors come only from the Accept that is ours
    /// and still cannot be used: a version we do not speak, or a key that
    /// does not exchange.
    [[nodiscard]] ChannelResult<BootstrapStep> Feed(ByteSpan chunk) noexcept;

    [[nodiscard]] bool IsDone() const noexcept { return Done_; }

    /// Blocks that were found and set aside, malformed ones included.
    [[nodiscard]] std::uint64_t IgnoredBlocks() const noexcept {
        return Scanner_.SkippedBlocks() + Ignored_;
    }

private:
    [[nodiscard]] ChannelResult<std::optional<BootstrapResult>> Consider(
        ByteSpan frame) noexcept;

    crypto::KeyPair Keys_;
    wire::BootstrapNonce Nonce_;
    wire::BootstrapEnvironment Environment_;
    wire::ArmorScanner Scanner_;
    std::uint64_t Ignored_{0};
    bool Done_{false};
};

/// The agent's end: reads its stdin for the Offer, then writes the Accept.
class BootstrapAgent {
public:
    /// `port` is the one the agent has already bound for the session, and
    /// `orphanTimeoutSeconds` how long it will keep the session unattended.
    BootstrapAgent(const crypto::Key& seed, const wire::BootstrapNonce& nonce,
                   std::uint16_t port,
                   std::uint32_t orphanTimeoutSeconds = 0) noexcept;

    /// Reads stdin until an Offer. Other blocks are skipped as on the client;
    /// an Offer that cannot be used is an error, because on stdin nothing but
    /// the client writes and there is no second Offer to wait for.
    [[nodiscard]] ChannelResult<BootstrapStep> Feed(ByteSpan chunk) noexcept;

    /// Names the port the Accept announces, in place of the one the agent
    /// was built with. Behind zet-muxd the port is the mux's, and the agent
    /// learns it by registering — which takes the session's R, which only a
    /// read Offer gives (design.md §6). Call before WriteAccept.
    void AnnouncePort(std::uint16_t port) noexcept;

    /// The armored Accept, once Feed has produced a result.
    [[nodiscard]] ProtoResult<std::size_t> WriteAccept(
        std::span<char> out) const noexcept;

    [[nodiscard]] bool IsDone() const noexcept { return Accept_.has_value(); }

    [[nodiscard]] std::uint64_t IgnoredBlocks() const noexcept {
        return Scanner_.SkippedBlocks() + Ignored_;
    }

private:
    [[nodiscard]] ChannelResult<std::optional<BootstrapResult>> Consider(
        ByteSpan frame) noexcept;

    crypto::KeyPair Keys_;
    wire::BootstrapNonce Nonce_;
    std::uint16_t Port_;
    std::uint32_t OrphanTimeoutSeconds_;
    wire::ArmorScanner Scanner_;
    std::optional<wire::BootstrapAcceptMessage> Accept_;
    std::uint64_t Ignored_{0};
};

}  // namespace zet::session
