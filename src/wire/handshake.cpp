#include "zet/wire/handshake.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"

namespace zet::wire {
namespace {

template <std::size_t tSize>
[[nodiscard]] constexpr std::array<std::byte, tSize - 1> AsBytes(
    const char (&text)[tSize]) noexcept {
    std::array<std::byte, tSize - 1> out{};
    std::ranges::transform(
        std::span{text}.first(tSize - 1), out.begin(),
        [](char letter) { return static_cast<std::byte>(letter); });
    return out;
}

// h0 of the chain. Fixed bytes, so both ends start alike and a transcript from
// some other protocol built the same way cannot collide with ours.
constexpr auto TRANSCRIPT_LABEL = AsBytes("zet-transcript-1");

// The direction labels of §5. Distinct so that a tag cannot be reflected back
// at the peer that produced it.
constexpr auto CLIENT_TAG_LABEL = AsBytes("zet-c");
constexpr auto SERVER_TAG_LABEL = AsBytes("zet-s");

// The wider of the two labels above. Both tags are computed over label ‖ h, and
// that message is built on the stack.
constexpr std::size_t MAX_TAG_LABEL_SIZE = 5;
constexpr std::size_t TAG_MESSAGE_SIZE = MAX_TAG_LABEL_SIZE + crypto::KEY_SIZE;

static_assert(CLIENT_TAG_LABEL.size() <= MAX_TAG_LABEL_SIZE);
static_assert(SERVER_TAG_LABEL.size() <= MAX_TAG_LABEL_SIZE);

[[nodiscard]] PreAuthError Fail(EProtoError error) noexcept {
    return PreAuthError{error};
}

/// Folds the offset AuthOk carries into the transcript before tag_s is taken.
///
/// Rule five of §5 wants the offset authenticated, and AuthOk cannot do it by
/// carrying the tag over itself. Putting it into h instead binds it just as
/// tightly: an observer who rewrites it to zero, forcing the whole history to
/// be replayed, leaves the two ends with different h and fails the tag.
void AbsorbReceiveOffset(Transcript& script, std::uint64_t offset) noexcept {
    // Eight bytes wide, exactly what the write needs, so it cannot fail.
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    ByteWriter writer{MutableByteSpan{bytes}};
    std::ignore = writer.WriteU64BE(offset);
    script.Absorb(ByteSpan{bytes});
}

}  // namespace

bool NonceMemory::Seen(const HandshakeNonce& nonce) const noexcept {
    return std::ranges::any_of(
        std::span{Items_}.first(Count_),
        [&nonce](const HandshakeNonce& known) { return known == nonce; });
}

void NonceMemory::Remember(const HandshakeNonce& nonce) noexcept {
    // Through a span rather than an index: the slot is ours and Next_ never
    // came off the wire, but nothing in this library reaches into storage by
    // number, and the rule is worth more than the exception would be.
    std::span{Items_}.subspan(Next_, 1).front() = nonce;
    Next_ = (Next_ + 1) % MAX_REMEMBERED_NONCES;
    Count_ = std::min(Count_ + 1, MAX_REMEMBERED_NONCES);
}

Transcript::Transcript() noexcept
    : State_(
          crypto::DeriveFromInfo(crypto::Key{}, ByteSpan{TRANSCRIPT_LABEL})) {}

void Transcript::Absorb(ByteSpan message) noexcept {
    State_ = crypto::DeriveFromInfo(State_, message);
}

std::size_t Transcript::Bind(MutableByteSpan out,
                             ByteSpan label) const noexcept {
    // The buffer is sized for the widest label plus the state, so neither write
    // can fail and their results are discarded rather than checked.
    ByteWriter writer{out};
    std::ignore = writer.WriteBytes(label);
    std::ignore = writer.WriteBytes(ByteSpan{State_.Expose()});
    return writer.Size();
}

crypto::Mac Transcript::Tag(const crypto::Key& auth,
                            ByteSpan label) const noexcept {
    std::array<std::byte, TAG_MESSAGE_SIZE> message{};
    const std::size_t size = Bind(MutableByteSpan{message}, label);
    return crypto::Authenticate(ByteSpan{message}.first(size), auth);
}

bool Transcript::Verify(const crypto::Key& auth, ByteSpan label,
                        const crypto::Mac& mac) const noexcept {
    std::array<std::byte, TAG_MESSAGE_SIZE> message{};
    const std::size_t size = Bind(MutableByteSpan{message}, label);
    return crypto::VerifyMac(mac, ByteSpan{message}.first(size), auth);
}

PreAuthResult<std::uint16_t> NegotiateVersion(std::uint16_t peerMin,
                                              std::uint16_t peerMax) noexcept {
    if (peerMin > peerMax) {
        return std::unexpected(Fail(EProtoError::MalformedField));
    }
    if (peerMin > PROTOCOL_VERSION_MAX || PROTOCOL_VERSION_MIN > peerMax) {
        return std::unexpected(Fail(EProtoError::VersionMismatch));
    }
    // The lower of the two maxima, never an equality test: a build that speaks
    // 1..3 and one that speaks 1..2 have to meet at 2 rather than refuse.
    return std::min(peerMax, PROTOCOL_VERSION_MAX);
}

PreAuthResult<SessionId> PeekHelloSessionId(ByteSpan body) noexcept {
    ByteReader reader{body};
    const auto type = ParseMessageType(reader);
    if (!type || *type != EMessageType::Hello) {
        return std::unexpected(Fail(EProtoError::UnexpectedMessage));
    }
    const auto hello = ParseHello(reader);
    if (!hello) {
        return std::unexpected(Fail(hello.error()));
    }
    return hello->Id;
}

ClientHandshake::ClientHandshake(SessionId id, crypto::Key auth,
                                 crypto::Key connectionSeed,
                                 HandshakeNonce clientNonce,
                                 NonceMemory& seen) noexcept
    : Id_(id),
      Auth_(std::move(auth)),
      ConnectionSeed_(std::move(connectionSeed)),
      ClientNonce_(clientNonce),
      Seen_(seen) {}

PreAuthResult<std::size_t> ClientHandshake::Start(
    MutableByteSpan out) noexcept {
    if (Stage_ != EStage::Fresh) {
        return std::unexpected(Fail(EProtoError::UnexpectedMessage));
    }
    // Our own nonce is checked as well as the peer's: a generator that has
    // begun repeating is caught here, before the keys it would collide with
    // are derived.
    if (Seen_.Seen(ClientNonce_)) {
        return std::unexpected(Fail(EProtoError::NonceReused));
    }

    HelloMessage hello{};
    hello.VersionMin = PROTOCOL_VERSION_MIN;
    hello.VersionMax = PROTOCOL_VERSION_MAX;
    hello.Id = Id_;
    hello.ClientNonce = ClientNonce_;

    ByteWriter writer{out};
    if (auto written = WriteHello(writer, hello); !written) {
        return std::unexpected(Fail(written.error()));
    }
    Script_.Absorb(writer.Written());
    Stage_ = EStage::SentHello;
    return writer.Size();
}

PreAuthResult<HandshakeStep> ClientHandshake::Handle(
    ByteSpan body, MutableByteSpan out) noexcept {
    ByteReader reader{body};
    const auto type = ParseMessageType(reader);
    // An unknown type before authentication hangs up rather than being skipped:
    // §7 lets a newer peer be ignored only once it has proved itself.
    if (!type) {
        return std::unexpected(Fail(EProtoError::UnexpectedMessage));
    }

    if (Stage_ == EStage::SentHello && *type == EMessageType::Challenge) {
        return OnChallenge(reader, body, out);
    }
    if (Stage_ == EStage::SentAuth && *type == EMessageType::AuthOk) {
        return OnAuthOk(reader);
    }
    return std::unexpected(Fail(EProtoError::UnexpectedMessage));
}

PreAuthResult<HandshakeStep> ClientHandshake::OnChallenge(
    ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept {
    const auto challenge = ParseChallenge(reader);
    if (!challenge) {
        return std::unexpected(Fail(challenge.error()));
    }
    if (challenge->Version < PROTOCOL_VERSION_MIN ||
        challenge->Version > PROTOCOL_VERSION_MAX) {
        return std::unexpected(Fail(EProtoError::VersionMismatch));
    }
    if (Seen_.Seen(challenge->ServerNonce)) {
        return std::unexpected(Fail(EProtoError::NonceReused));
    }

    ServerNonce_ = challenge->ServerNonce;
    Version_ = challenge->Version;
    Script_.Absorb(body);

    const AuthMessage auth{.ClientTag =
                               Script_.Tag(Auth_, ByteSpan{CLIENT_TAG_LABEL})};
    ByteWriter writer{out};
    if (auto written = WriteAuth(writer, auth); !written) {
        return std::unexpected(Fail(written.error()));
    }
    Script_.Absorb(writer.Written());
    Stage_ = EStage::SentAuth;
    return HandshakeStep{.Written = writer.Size(), .Complete = false};
}

PreAuthResult<HandshakeStep> ClientHandshake::OnAuthOk(
    ByteReader& reader) noexcept {
    const auto ok = ParseAuthOk(reader);
    if (!ok) {
        return std::unexpected(Fail(ok.error()));
    }
    AbsorbReceiveOffset(Script_, ok->ServerReceiveOffset);

    // Rule four of §5: without this the client would hand its send buffer —
    // everything typed, passwords among it — to whoever answered first.
    if (!Script_.Verify(Auth_, ByteSpan{SERVER_TAG_LABEL}, ok->ServerTag)) {
        return std::unexpected(Fail(EProtoError::AuthenticationFailed));
    }

    Keys_ = DeriveConnectionKeys(ConnectionSeed_, ClientNonce_, ServerNonce_);
    PeerReceiveOffset_ = ok->ServerReceiveOffset;
    Seen_.Remember(ClientNonce_);
    Seen_.Remember(ServerNonce_);
    Stage_ = EStage::Done;
    return HandshakeStep{.Written = 0, .Complete = true};
}

std::optional<HandshakeResult> ClientHandshake::TakeResult() noexcept {
    if (Stage_ != EStage::Done) {
        return std::nullopt;
    }
    Stage_ = EStage::Spent;
    return HandshakeResult{.Keys = std::move(Keys_),
                           .PeerReceiveOffset = PeerReceiveOffset_};
}

ServerHandshake::ServerHandshake(crypto::Key auth, crypto::Key connectionSeed,
                                 HandshakeNonce serverNonce,
                                 std::uint64_t receiveOffset,
                                 NonceMemory& seen) noexcept
    : Auth_(std::move(auth)),
      ConnectionSeed_(std::move(connectionSeed)),
      ServerNonce_(serverNonce),
      ReceiveOffset_(receiveOffset),
      Seen_(seen) {}

PreAuthResult<HandshakeStep> ServerHandshake::Handle(
    ByteSpan body, MutableByteSpan out) noexcept {
    ByteReader reader{body};
    const auto type = ParseMessageType(reader);
    if (!type) {
        return std::unexpected(Fail(EProtoError::UnexpectedMessage));
    }

    if (Stage_ == EStage::Fresh && *type == EMessageType::Hello) {
        return OnHello(reader, body, out);
    }
    if (Stage_ == EStage::SentChallenge && *type == EMessageType::Auth) {
        return OnAuth(reader, body, out);
    }
    return std::unexpected(Fail(EProtoError::UnexpectedMessage));
}

PreAuthResult<HandshakeStep> ServerHandshake::OnHello(
    ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept {
    const auto hello = ParseHello(reader);
    if (!hello) {
        return std::unexpected(Fail(hello.error()));
    }
    const auto version = NegotiateVersion(hello->VersionMin, hello->VersionMax);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (Seen_.Seen(hello->ClientNonce) || Seen_.Seen(ServerNonce_)) {
        return std::unexpected(Fail(EProtoError::NonceReused));
    }

    // The sid is not looked at. Whoever built this object already used it to
    // pick the secrets, real or decoy, and comparing it here would be the
    // branch §5 rule two exists to remove.
    ClientNonce_ = hello->ClientNonce;
    Version_ = *version;
    Script_.Absorb(body);

    ChallengeMessage challenge{};
    challenge.Version = *version;
    challenge.ServerNonce = ServerNonce_;

    ByteWriter writer{out};
    if (auto written = WriteChallenge(writer, challenge); !written) {
        return std::unexpected(Fail(written.error()));
    }
    Script_.Absorb(writer.Written());
    Stage_ = EStage::SentChallenge;
    return HandshakeStep{.Written = writer.Size(), .Complete = false};
}

PreAuthResult<HandshakeStep> ServerHandshake::OnAuth(
    ByteReader& reader, ByteSpan body, MutableByteSpan out) noexcept {
    const auto auth = ParseAuth(reader);
    if (!auth) {
        return std::unexpected(Fail(auth.error()));
    }
    // Nothing above this line touched anything but our own bytes, and nothing
    // below it belongs to the session either: rule one of §5 holds because the
    // session is not reachable from here at all.
    if (!Script_.Verify(Auth_, ByteSpan{CLIENT_TAG_LABEL}, auth->ClientTag)) {
        return std::unexpected(Fail(EProtoError::AuthenticationFailed));
    }
    Script_.Absorb(body);
    AbsorbReceiveOffset(Script_, ReceiveOffset_);

    Keys_ = DeriveConnectionKeys(ConnectionSeed_, ClientNonce_, ServerNonce_);

    const AuthOkMessage ok{
        .ServerTag = Script_.Tag(Auth_, ByteSpan{SERVER_TAG_LABEL}),
        .ServerReceiveOffset = ReceiveOffset_};
    ByteWriter writer{out};
    if (auto written = WriteAuthOk(writer, ok); !written) {
        return std::unexpected(Fail(written.error()));
    }

    Seen_.Remember(ClientNonce_);
    Seen_.Remember(ServerNonce_);
    Stage_ = EStage::Done;
    return HandshakeStep{.Written = writer.Size(), .Complete = true};
}

std::optional<HandshakeResult> ServerHandshake::TakeResult() noexcept {
    if (Stage_ != EStage::Done) {
        return std::nullopt;
    }
    Stage_ = EStage::Spent;
    // The client's offset rides in Resume, which is sealed and therefore the
    // session's to open, not this automaton's.
    return HandshakeResult{.Keys = std::move(Keys_), .PeerReceiveOffset = 0};
}

}  // namespace zet::wire
