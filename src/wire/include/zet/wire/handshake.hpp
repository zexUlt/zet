#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

/// The resume handshake of docs/design.md §5.
///
///     C→S  Hello      "ZET1", ver_min, ver_max, sid, c_nonce, tlv…
///     S→C  Challenge   ver, s_nonce, tlv…
///     C→S  Auth        tag_c = MAC(K_auth, "zet-c" ‖ h)
///     S→C  AuthOk      tag_s = MAC(K_auth, "zet-s" ‖ h), s_recv_offset
///
/// Both automata are pure: bytes in, bytes out, no clock and no syscall. What
/// §5 calls the budget — five failed attempts per sid, the per-IP cap on
/// unfinished handshakes, the five second deadline — is not here and cannot be:
/// those counters span sessions and connections, and a deadline wants a clock.
/// The caller holding the session table applies them.
namespace zet::wire {

/// An error raised before the peer has proved anything.
///
/// Everything up to AuthOk travels in the clear, so a disposition of
/// KillSession escaping this stage would mean that reading a sid off the wire
/// is enough to end the session it names — exactly what §5 promises a leaked
/// sid cannot do. The cap is applied here rather than trusted to every parser
/// underneath: one of them already returned VersionMismatch, which disposes as
/// KillSession, for an unknown critical option in a cleartext Hello.
class PreAuthError {
public:
    constexpr explicit PreAuthError(EProtoError error) noexcept
        : Error_(error) {}

    [[nodiscard]] constexpr EProtoError Error() const noexcept {
        return Error_;
    }

    [[nodiscard]] constexpr EDisposition Disposition() const noexcept {
        const EDisposition wanted = DispositionOf(Error_);
        return wanted == EDisposition::KillSession
                   ? EDisposition::CloseConnection
                   : wanted;
    }

    [[nodiscard]] friend constexpr bool operator==(PreAuthError,
                                                   PreAuthError) = default;

private:
    EProtoError Error_;
};

template <typename TValue>
using PreAuthResult = std::expected<TValue, PreAuthError>;

/// The handshake nonces a session has already used, per §5 rule three.
///
/// Owned by the session, not by a handshake: what it has to outlive is the
/// connection. Entries are recorded only once a handshake authenticates, so
/// that unauthenticated traffic cannot flush the buffer and blind it.
class NonceMemory {
public:
    [[nodiscard]] bool Seen(const HandshakeNonce& nonce) const noexcept;

    /// Records a nonce, forgetting the oldest when full.
    void Remember(const HandshakeNonce& nonce) noexcept;

    [[nodiscard]] std::size_t Size() const noexcept { return Count_; }

private:
    std::array<HandshakeNonce, MAX_REMEMBERED_NONCES> Items_{};
    std::size_t Count_{0};
    std::size_t Next_{0};
};

/// The running hash of everything said so far, per §5.
///
/// A chain rather than the concatenated bytes: four messages at
/// MAX_HANDSHAKE_FRAME would be sixteen kilobytes held for every unfinished
/// attempt a stranger can start, and thirty-two bytes prove the same thing.
class Transcript {
public:
    Transcript() noexcept;

    /// Folds in one message exactly as it appeared on the wire, type byte
    /// included.
    void Absorb(ByteSpan message) noexcept;

    /// MAC(auth, label ‖ h). The label separates the two directions so that a
    /// tag cannot be reflected back at whoever sent it.
    [[nodiscard]] crypto::Mac Tag(const crypto::Key& auth,
                                  ByteSpan label) const noexcept;

    /// Checks a tag against what we hold. Separate from comparing the output of
    /// Tag so that the comparison stays the constant-time one.
    [[nodiscard]] bool Verify(const crypto::Key& auth, ByteSpan label,
                              const crypto::Mac& mac) const noexcept;

private:
    /// Lays out label ‖ h and reports how much of `out` it used.
    [[nodiscard]] std::size_t Bind(MutableByteSpan out,
                                   ByteSpan label) const noexcept;

    crypto::Key State_;
};

/// What the caller must put on the wire, and whether there is anything left to
/// do after it.
struct HandshakeStep {
    /// Bytes written into `out`. Zero means the message needed no answer.
    std::size_t Written{0};
    bool Complete{false};
};

/// What a finished handshake yields.
struct HandshakeResult {
    ConnectionKeys Keys;
    /// How much of our stream the peer says it already holds, so that the
    /// replay starts there and not from the beginning.
    std::uint64_t PeerReceiveOffset{0};
};

/// Reads a Hello far enough to route it and no further.
///
/// The agent needs the sid to find the session before it can hand secrets to
/// ServerHandshake; everything else in the message is the automaton's business.
[[nodiscard]] PreAuthResult<SessionId> PeekHelloSessionId(
    ByteSpan body) noexcept;

class ClientHandshake {
public:
    ClientHandshake(SessionId id, crypto::Key auth, crypto::Key connectionSeed,
                    HandshakeNonce clientNonce, NonceMemory& seen) noexcept;

    ClientHandshake(const ClientHandshake&) = delete;
    ClientHandshake& operator=(const ClientHandshake&) = delete;

    /// Writes Hello. Valid once, before anything is handled.
    [[nodiscard]] PreAuthResult<std::size_t> Start(
        MutableByteSpan out) noexcept;

    /// Takes one handshake message body and writes whatever answers it.
    [[nodiscard]] PreAuthResult<HandshakeStep> Handle(
        ByteSpan body, MutableByteSpan out) noexcept;

    /// The keys, once and only once the handshake finished. Empty otherwise,
    /// so there is no way to reach a half-derived key by asking early.
    [[nodiscard]] std::optional<HandshakeResult> TakeResult() noexcept;

    [[nodiscard]] std::uint16_t Version() const noexcept { return Version_; }

private:
    enum class EStage : std::uint8_t {
        Fresh,
        SentHello,
        SentAuth,
        Done,
        Spent
    };

    [[nodiscard]] PreAuthResult<HandshakeStep> OnChallenge(
        ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept;
    [[nodiscard]] PreAuthResult<HandshakeStep> OnAuthOk(
        ByteReader& reader) noexcept;

    SessionId Id_;
    crypto::Key Auth_;
    crypto::Key ConnectionSeed_;
    HandshakeNonce ClientNonce_;
    HandshakeNonce ServerNonce_{};
    NonceMemory& Seen_;
    Transcript Script_;
    ConnectionKeys Keys_;
    std::uint64_t PeerReceiveOffset_{0};
    std::uint16_t Version_{0};
    EStage Stage_{EStage::Fresh};
};

class ServerHandshake {
public:
    /// Secrets arrive whether or not the sid was found: an unknown one is given
    /// DeriveDecoySecrets, and this class is never told which it got. That is
    /// what makes §5 rule two structural — the branch that could diverge in
    /// timing or in wording does not exist here to diverge.
    ServerHandshake(crypto::Key auth, crypto::Key connectionSeed,
                    HandshakeNonce serverNonce, std::uint64_t receiveOffset,
                    NonceMemory& seen) noexcept;

    ServerHandshake(const ServerHandshake&) = delete;
    ServerHandshake& operator=(const ServerHandshake&) = delete;

    [[nodiscard]] PreAuthResult<HandshakeStep> Handle(
        ByteSpan body, MutableByteSpan out) noexcept;

    [[nodiscard]] std::optional<HandshakeResult> TakeResult() noexcept;

    [[nodiscard]] std::uint16_t Version() const noexcept { return Version_; }

private:
    enum class EStage : std::uint8_t { Fresh, SentChallenge, Done, Spent };

    [[nodiscard]] PreAuthResult<HandshakeStep> OnHello(
        ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept;
    [[nodiscard]] PreAuthResult<HandshakeStep> OnAuth(
        ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept;

    crypto::Key Auth_;
    crypto::Key ConnectionSeed_;
    HandshakeNonce ServerNonce_;
    HandshakeNonce ClientNonce_{};
    std::uint64_t ReceiveOffset_;
    NonceMemory& Seen_;
    Transcript Script_;
    ConnectionKeys Keys_;
    std::uint16_t Version_{0};
    EStage Stage_{EStage::Fresh};
};

/// The overlap rule from §7: take min of the two maxima whenever the ranges
/// meet at all. Never equality — that is what left EternalTerminal unable to
/// change its handshake without breaking every client it had.
[[nodiscard]] PreAuthResult<std::uint16_t> NegotiateVersion(
    std::uint16_t peerMin, std::uint16_t peerMax) noexcept;

}  // namespace zet::wire
