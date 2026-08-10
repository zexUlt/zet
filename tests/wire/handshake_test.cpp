#include "zet/wire/handshake.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "zet/core/byte_writer.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// Room for any handshake message several times over. The widest is Hello at
/// forty-one bytes.
constexpr std::size_t SCRATCH = 256;

constexpr std::uint64_t SERVER_OFFSET = 0x0102'0304'0506'0708;

crypto::Key MakeMaster(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t value = seed;
    for (auto& byte : bytes) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 5);
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint8_t seed) {
    HandshakeNonce nonce{};
    std::uint8_t value = seed;
    for (auto& byte : nonce) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 11);
    }
    return nonce;
}

crypto::Key Clone(const crypto::Key& key) { return crypto::Key{key.Expose()}; }

std::vector<std::byte> Bytes(const crypto::Key& key) {
    return {key.Expose().begin(), key.Expose().end()};
}

/// Which message on the wire a test wants to interfere with.
enum class EWire : std::uint8_t { Hello, Challenge, Auth, AuthOk };

struct Tamper {
    EWire Message{EWire::Hello};
    std::size_t Offset{0};
};

struct Exchange {
    bool Completed{false};
    std::optional<EProtoError> Error;
    std::optional<EDisposition> Disposition;
    std::vector<std::byte> Hello;
    std::vector<std::byte> Challenge;
    std::vector<std::byte> Auth;
    std::vector<std::byte> AuthOk;
    std::optional<HandshakeResult> FromClient;
    std::optional<HandshakeResult> FromServer;
};

std::vector<std::byte> Taken(const std::vector<std::byte>& buffer,
                             std::size_t size) {
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)};
}

/// Flips one bit of one message, leaving everything else alone.
std::vector<std::byte> Bend(std::vector<std::byte> message,
                            const std::optional<Tamper>& tamper, EWire which) {
    if (!tamper || tamper->Message != which ||
        tamper->Offset >= message.size()) {
        return message;
    }
    auto target = std::span{message}.subspan(tamper->Offset, 1);
    target.front() ^= std::byte{0x01};
    return message;
}

/// Drives one whole handshake and reports what came of it.
///
/// The two ends hold their own copies of the secrets, so a test can hand the
/// agent a different session — or a decoy — without the client noticing.
struct Harness {
    SessionId Id{};
    crypto::Key ClientAuth;
    crypto::Key ClientSeed;
    crypto::Key ServerAuth;
    crypto::Key ServerSeed;
    HandshakeNonce ClientNonce{MakeNonce(0x10)};
    HandshakeNonce ServerNonce{MakeNonce(0x90)};
    NonceMemory ClientSeen;
    NonceMemory ServerSeen;

    explicit Harness(std::uint8_t clientMaster = 1,
                     std::uint8_t serverMaster = 1) {
        const auto mine = DeriveSessionSecrets(MakeMaster(clientMaster));
        const auto theirs = DeriveSessionSecrets(MakeMaster(serverMaster));
        Id = mine.Id;
        ClientAuth = Clone(mine.Auth);
        ClientSeed = Clone(mine.ConnectionSeed);
        ServerAuth = Clone(theirs.Auth);
        ServerSeed = Clone(theirs.ConnectionSeed);
    }

    Exchange Run(std::optional<Tamper> tamper = std::nullopt) {
        Exchange out;
        ClientHandshake client{Id, Clone(ClientAuth), Clone(ClientSeed),
                               ClientNonce, ClientSeen};
        ServerHandshake server{Clone(ServerAuth), Clone(ServerSeed),
                               ServerNonce, SERVER_OFFSET, ServerSeen};

        std::vector<std::byte> scratch(SCRATCH);

        const auto hello = client.Start(MutableByteSpan{scratch});
        if (!hello) {
            out.Error = hello.error().Error();
            out.Disposition = hello.error().Disposition();
            return out;
        }
        out.Hello = Taken(scratch, *hello);

        const auto challenge =
            server.Handle(ByteSpan{Bend(out.Hello, tamper, EWire::Hello)},
                          MutableByteSpan{scratch});
        if (!challenge) {
            out.Error = challenge.error().Error();
            out.Disposition = challenge.error().Disposition();
            return out;
        }
        out.Challenge = Taken(scratch, challenge->Written);

        const auto auth = client.Handle(
            ByteSpan{Bend(out.Challenge, tamper, EWire::Challenge)},
            MutableByteSpan{scratch});
        if (!auth) {
            out.Error = auth.error().Error();
            out.Disposition = auth.error().Disposition();
            return out;
        }
        out.Auth = Taken(scratch, auth->Written);

        const auto authOk =
            server.Handle(ByteSpan{Bend(out.Auth, tamper, EWire::Auth)},
                          MutableByteSpan{scratch});
        if (!authOk) {
            out.Error = authOk.error().Error();
            out.Disposition = authOk.error().Disposition();
            return out;
        }
        out.AuthOk = Taken(scratch, authOk->Written);

        const auto done =
            client.Handle(ByteSpan{Bend(out.AuthOk, tamper, EWire::AuthOk)},
                          MutableByteSpan{scratch});
        if (!done) {
            out.Error = done.error().Error();
            out.Disposition = done.error().Disposition();
            return out;
        }

        out.Completed = done->Complete && authOk->Complete;
        out.FromClient = client.TakeResult();
        out.FromServer = server.TakeResult();
        return out;
    }
};

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture, "a clean handshake leaves both ends keyed") {
    Harness peers;
    const auto run = peers.Run();

    REQUIRE(run.Completed);
    REQUIRE(run.FromClient.has_value());
    REQUIRE(run.FromServer.has_value());

    // Both ends must arrive at the same keys without ever sending one.
    CHECK(Bytes(run.FromClient->Keys.ClientToServer) ==
          Bytes(run.FromServer->Keys.ClientToServer));
    CHECK(Bytes(run.FromClient->Keys.ServerToClient) ==
          Bytes(run.FromServer->Keys.ServerToClient));
    CHECK(run.FromClient->Keys.SaltClientToServer ==
          run.FromServer->Keys.SaltClientToServer);

    // The offset AuthOk carried is what tells the client where to resume.
    CHECK(run.FromClient->PeerReceiveOffset == SERVER_OFFSET);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "no forged byte of the transcript completes a handshake") {
    // The readiness criterion of M1: every field of every message, one bit at a
    // time. Some flips are caught by the parser and some by the tag; what the
    // sweep asserts is that none of them gets through.
    Harness reference;
    const auto clean = reference.Run();
    REQUIRE(clean.Completed);

    const std::array<std::pair<EWire, std::size_t>, 4> messages{
        std::pair{EWire::Hello, clean.Hello.size()},
        std::pair{EWire::Challenge, clean.Challenge.size()},
        std::pair{EWire::Auth, clean.Auth.size()},
        std::pair{EWire::AuthOk, clean.AuthOk.size()},
    };

    std::size_t forged = 0;
    for (const auto& entry : messages) {
        const EWire which = entry.first;
        const std::size_t size = entry.second;
        for (std::size_t offset = 0; offset < size; ++offset) {
            CAPTURE(static_cast<int>(which));
            CAPTURE(offset);
            Harness peers;
            const auto run =
                peers.Run(Tamper{.Message = which, .Offset = offset});
            CHECK_FALSE(run.Completed);
            ++forged;
        }
    }

    // Guards the sweep: a run that produced no messages would pass it silently.
    CHECK(forged > 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the offset in AuthOk cannot be rewritten in flight") {
    // Rule five of §5 by itself: tag_s lives inside AuthOk and cannot cover the
    // rest of it, so the offset is folded into the transcript instead. Without
    // that, an observer rewrites it to zero and forces the whole history to be
    // replayed.
    Harness reference;
    const auto clean = reference.Run();
    REQUIRE(clean.Completed);

    // The offset is the last eight bytes: type, tag, then the offset.
    const std::size_t offsetAt = clean.AuthOk.size() - sizeof(std::uint64_t);
    for (std::size_t i = offsetAt; i < clean.AuthOk.size(); ++i) {
        CAPTURE(i);
        Harness peers;
        const auto run =
            peers.Run(Tamper{.Message = EWire::AuthOk, .Offset = i});
        CHECK_FALSE(run.Completed);
        CHECK(run.Error == EProtoError::AuthenticationFailed);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a session the agent does not have fails the same way") {
    // Rule two of §5. The decoy answers a full Challenge and dies at the tag,
    // exactly where a real session with the wrong key would.
    Harness stranger{1, 2};
    const auto run = stranger.Run();

    CHECK_FALSE(run.Completed);
    CHECK(run.Error == EProtoError::AuthenticationFailed);
    // Up to that point the exchange is indistinguishable, byte count included.
    Harness known;
    const auto good = known.Run();
    CHECK(run.Hello.size() == good.Hello.size());
    CHECK(run.Challenge.size() == good.Challenge.size());
    CHECK(run.Auth.size() == good.Auth.size());
}

TEST_CASE_FIXTURE(SodiumFixture, "the decoy answers alike every time") {
    // A decoy drawn afresh per attempt is itself the oracle: a real session
    // gives one answer to two identical probes, a random one gives two.
    const auto local = MakeMaster(7);
    SessionId unknown{};
    unknown.fill(std::byte{0x5A});

    const auto first = DeriveDecoySecrets(local, unknown);
    const auto second = DeriveDecoySecrets(local, unknown);

    CHECK(Bytes(first.Auth) == Bytes(second.Auth));
    CHECK(Bytes(first.ConnectionSeed) == Bytes(second.ConnectionSeed));
    CHECK(first.Id == unknown);

    SessionId other{};
    other.fill(std::byte{0x5B});
    CHECK(Bytes(DeriveDecoySecrets(local, other).Auth) != Bytes(first.Auth));

    // A different agent gives a different decoy, so two agents cannot be told
    // apart by comparing what they answer to the same unknown sid.
    CHECK(Bytes(DeriveDecoySecrets(MakeMaster(8), unknown).Auth) !=
          Bytes(first.Auth));
}

TEST_CASE("a pre-auth error can never ask for the session") {
    // Everything up to AuthOk is cleartext. If a disposition of KillSession
    // could escape it, reading a sid off the wire would be enough to end the
    // session it names.
    CHECK(DispositionOf(EProtoError::VersionMismatch) ==
          EDisposition::KillSession);
    CHECK(PreAuthError{EProtoError::VersionMismatch}.Disposition() ==
          EDisposition::CloseConnection);

    // Lesser dispositions are left where they are.
    CHECK(PreAuthError{EProtoError::Truncated}.Disposition() ==
          EDisposition::CloseConnection);
    CHECK(PreAuthError{EProtoError::UnknownMessageType}.Disposition() ==
          EDisposition::Ignore);
    CHECK(PreAuthError{EProtoError::Truncated}.Error() ==
          EProtoError::Truncated);
}

TEST_CASE("version negotiation takes the lower maximum, never equality") {
    // A build speaking 1..3 and one speaking 1..2 have to meet at 2.
    const auto met = NegotiateVersion(PROTOCOL_VERSION_MIN, 3);
    REQUIRE(met.has_value());
    CHECK(*met == PROTOCOL_VERSION_MAX);

    const auto exact =
        NegotiateVersion(PROTOCOL_VERSION_MIN, PROTOCOL_VERSION_MAX);
    REQUIRE(exact.has_value());
    CHECK(*exact == PROTOCOL_VERSION_MAX);

    const auto ahead = NegotiateVersion(PROTOCOL_VERSION_MAX + 1, 9);
    REQUIRE_FALSE(ahead.has_value());
    CHECK(ahead.error().Error() == EProtoError::VersionMismatch);
    CHECK(ahead.error().Disposition() == EDisposition::CloseConnection);

    const auto backwards = NegotiateVersion(4, 3);
    REQUIRE_FALSE(backwards.has_value());
    CHECK(backwards.error().Error() == EProtoError::MalformedField);
}

TEST_CASE("nonce memory finds a repeat and forgets the oldest") {
    NonceMemory memory;
    CHECK_FALSE(memory.Seen(MakeNonce(1)));

    memory.Remember(MakeNonce(1));
    CHECK(memory.Seen(MakeNonce(1)));
    CHECK_FALSE(memory.Seen(MakeNonce(2)));
    CHECK(memory.Size() == 1);

    for (std::size_t i = 0; i < MAX_REMEMBERED_NONCES; ++i) {
        memory.Remember(MakeNonce(static_cast<std::uint8_t>(i + 2)));
    }

    // Bounded on purpose: what it defends against is a generator that repeats,
    // not a peer replaying, and a replay earns nothing anyway.
    CHECK(memory.Size() == MAX_REMEMBERED_NONCES);
    CHECK_FALSE(memory.Seen(MakeNonce(1)));
}

TEST_CASE_FIXTURE(SodiumFixture, "a repeated nonce is refused on both sides") {
    Harness peers;

    // The client catches its own generator before a single byte goes out.
    peers.ClientSeen.Remember(peers.ClientNonce);
    const auto ownRepeat = peers.Run();
    CHECK(ownRepeat.Error == EProtoError::NonceReused);

    Harness fromPeer;
    fromPeer.ServerSeen.Remember(fromPeer.ClientNonce);
    const auto theirRepeat = fromPeer.Run();
    CHECK(theirRepeat.Error == EProtoError::NonceReused);

    Harness reflected;
    reflected.ClientSeen.Remember(reflected.ServerNonce);
    const auto serverRepeat = reflected.Run();
    CHECK(serverRepeat.Error == EProtoError::NonceReused);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a finished handshake remembers what it used") {
    Harness peers;
    const auto run = peers.Run();
    REQUIRE(run.Completed);

    CHECK(peers.ClientSeen.Seen(peers.ClientNonce));
    CHECK(peers.ClientSeen.Seen(peers.ServerNonce));
    CHECK(peers.ServerSeen.Seen(peers.ClientNonce));
    CHECK(peers.ServerSeen.Seen(peers.ServerNonce));
}

TEST_CASE_FIXTURE(SodiumFixture, "an unfinished handshake remembers nothing") {
    // Otherwise a stranger could fill the buffer with Hellos and blind it.
    Harness stranger{1, 2};
    const auto run = stranger.Run();
    REQUIRE_FALSE(run.Completed);

    CHECK(stranger.ServerSeen.Size() == 0);
    CHECK(stranger.ClientSeen.Size() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "messages out of order are refused") {
    Harness peers;
    NonceMemory seen;
    std::vector<std::byte> scratch(SCRATCH);

    // Each case gets its own automaton: a refusal is terminal, so chaining
    // them through one object would only ever test the first.

    // Nothing has been sent, so there is nothing an answer could answer.
    {
        ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                               Clone(peers.ClientSeed), peers.ClientNonce,
                               seen};
        const auto early = client.Handle(ByteSpan{}, MutableByteSpan{scratch});
        REQUIRE_FALSE(early.has_value());
        CHECK(early.error().Error() == EProtoError::UnexpectedMessage);
    }

    // Hello twice would restart the transcript under the peer.
    {
        ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                               Clone(peers.ClientSeed), peers.ClientNonce,
                               seen};
        REQUIRE(client.Start(MutableByteSpan{scratch}).has_value());
        const auto again = client.Start(MutableByteSpan{scratch});
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().Error() == EProtoError::UnexpectedMessage);
    }

    // An AuthOk before the Challenge it answers.
    {
        ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                               Clone(peers.ClientSeed), peers.ClientNonce,
                               seen};
        REQUIRE(client.Start(MutableByteSpan{scratch}).has_value());

        std::vector<std::byte> authOk(SCRATCH);
        ByteWriter writer{MutableByteSpan{authOk}};
        REQUIRE(WriteAuthOk(writer, AuthOkMessage{}).has_value());
        const auto skipped = client.Handle(
            ByteSpan{authOk}.first(writer.Size()), MutableByteSpan{scratch});
        REQUIRE_FALSE(skipped.has_value());
        CHECK(skipped.error().Error() == EProtoError::UnexpectedMessage);
    }
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "an unknown message type hangs up rather than being skipped") {
    // §7 lets a newer peer be ignored only once it has proved itself. Before
    // that, Ignore would be an invitation to keep talking for free.
    Harness peers;
    NonceMemory seen;
    std::vector<std::byte> scratch(SCRATCH);

    ServerHandshake server{Clone(peers.ServerAuth), Clone(peers.ServerSeed),
                           peers.ServerNonce, SERVER_OFFSET, seen};

    const std::vector<std::byte> unknown{std::byte{0xEE}, std::byte{0x00}};
    const auto refused =
        server.Handle(ByteSpan{unknown}, MutableByteSpan{scratch});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Error() == EProtoError::UnexpectedMessage);
    CHECK(refused.error().Disposition() == EDisposition::CloseConnection);
}

TEST_CASE_FIXTURE(SodiumFixture, "a tag cannot be reflected back") {
    // The two labels are what stop tag_s from passing as tag_c, which would let
    // whoever recorded one side of an exchange answer the other.
    const std::array<std::byte, 5> clientLabel{std::byte{'z'}, std::byte{'e'},
                                               std::byte{'t'}, std::byte{'-'},
                                               std::byte{'c'}};
    const std::array<std::byte, 5> serverLabel{std::byte{'z'}, std::byte{'e'},
                                               std::byte{'t'}, std::byte{'-'},
                                               std::byte{'s'}};

    const auto key = MakeMaster(4);
    const std::vector<std::byte> message{std::byte{0x11}, std::byte{0x22}};

    Transcript script;
    script.Absorb(ByteSpan{message});

    const auto asClient = script.Tag(key, ByteSpan{clientLabel});
    CHECK(script.Verify(key, ByteSpan{clientLabel}, asClient));
    CHECK_FALSE(script.Verify(key, ByteSpan{serverLabel}, asClient));
}

TEST_CASE_FIXTURE(SodiumFixture, "the transcript moves with every message") {
    Transcript empty;
    Transcript one;
    Transcript two;

    const std::vector<std::byte> first{std::byte{0x01}, std::byte{0x02}};
    const std::vector<std::byte> second{std::byte{0x03}};

    one.Absorb(ByteSpan{first});
    two.Absorb(ByteSpan{first});
    two.Absorb(ByteSpan{second});

    const auto key = MakeMaster(3);
    const auto atZero = empty.Tag(key, ByteSpan{});
    const auto atOne = one.Tag(key, ByteSpan{});
    const auto atTwo = two.Tag(key, ByteSpan{});

    CHECK(atZero != atOne);
    CHECK(atOne != atTwo);

    // Both ends start from the same fixed h0, or nothing would ever agree.
    Transcript alsoEmpty;
    CHECK(alsoEmpty.Tag(key, ByteSpan{}) == atZero);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a buffer too small refuses rather than truncates") {
    Harness peers;
    NonceMemory seen;
    std::vector<std::byte> tiny(4);

    ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                           Clone(peers.ClientSeed), peers.ClientNonce, seen};

    const auto refused = client.Start(MutableByteSpan{tiny});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Error() == EProtoError::BufferTooSmall);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a refusal ends the handshake instead of rewinding it") {
    // A step that failed part way has already folded its message into the
    // transcript. Answering the same message again with a wider buffer would
    // fold it twice, and the two ends would compute different tags for the
    // rest of the exchange.
    Harness peers;
    NonceMemory clientSeen;
    NonceMemory serverSeen;
    std::vector<std::byte> scratch(SCRATCH);
    std::vector<std::byte> tiny(4);

    ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                           Clone(peers.ClientSeed), peers.ClientNonce,
                           clientSeen};
    ServerHandshake server{Clone(peers.ServerAuth), Clone(peers.ServerSeed),
                           peers.ServerNonce, SERVER_OFFSET, serverSeen};

    const auto hello = client.Start(MutableByteSpan{scratch});
    REQUIRE(hello.has_value());
    const auto helloBytes = Taken(scratch, *hello);

    // The agent has nowhere to put the Challenge.
    const auto cramped =
        server.Handle(ByteSpan{helloBytes}, MutableByteSpan{tiny});
    REQUIRE_FALSE(cramped.has_value());
    CHECK(cramped.error().Error() == EProtoError::BufferTooSmall);

    // And it stays dead rather than absorbing the same Hello a second time.
    const auto retried =
        server.Handle(ByteSpan{helloBytes}, MutableByteSpan{scratch});
    REQUIRE_FALSE(retried.has_value());
    CHECK(retried.error().Error() == EProtoError::UnexpectedMessage);
    CHECK_FALSE(server.TakeResult().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the client refuses when it cannot write its answer") {
    Harness peers;
    NonceMemory clientSeen;
    NonceMemory serverSeen;
    std::vector<std::byte> scratch(SCRATCH);
    std::vector<std::byte> tiny(4);

    ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                           Clone(peers.ClientSeed), peers.ClientNonce,
                           clientSeen};
    ServerHandshake server{Clone(peers.ServerAuth), Clone(peers.ServerSeed),
                           peers.ServerNonce, SERVER_OFFSET, serverSeen};

    const auto hello = client.Start(MutableByteSpan{scratch});
    REQUIRE(hello.has_value());
    const auto helloBytes = Taken(scratch, *hello);

    const auto challenge =
        server.Handle(ByteSpan{helloBytes}, MutableByteSpan{scratch});
    REQUIRE(challenge.has_value());
    const auto challengeBytes = Taken(scratch, challenge->Written);

    // Both ends negotiated before anyone failed at anything.
    CHECK(client.Version() == 0);
    CHECK(server.Version() == PROTOCOL_VERSION_MAX);

    const auto cramped =
        client.Handle(ByteSpan{challengeBytes}, MutableByteSpan{tiny});
    REQUIRE_FALSE(cramped.has_value());
    CHECK(cramped.error().Error() == EProtoError::BufferTooSmall);
    CHECK_FALSE(client.TakeResult().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the agent refuses when it cannot write AuthOk") {
    Harness peers;
    NonceMemory clientSeen;
    NonceMemory serverSeen;
    std::vector<std::byte> scratch(SCRATCH);
    std::vector<std::byte> tiny(8);

    ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                           Clone(peers.ClientSeed), peers.ClientNonce,
                           clientSeen};
    ServerHandshake server{Clone(peers.ServerAuth), Clone(peers.ServerSeed),
                           peers.ServerNonce, SERVER_OFFSET, serverSeen};

    const auto hello = client.Start(MutableByteSpan{scratch});
    REQUIRE(hello.has_value());
    const auto helloBytes = Taken(scratch, *hello);

    const auto challenge =
        server.Handle(ByteSpan{helloBytes}, MutableByteSpan{scratch});
    REQUIRE(challenge.has_value());
    const auto challengeBytes = Taken(scratch, challenge->Written);

    const auto auth =
        client.Handle(ByteSpan{challengeBytes}, MutableByteSpan{scratch});
    REQUIRE(auth.has_value());
    CHECK(client.Version() == PROTOCOL_VERSION_MAX);
    const auto authBytes = Taken(scratch, auth->Written);

    const auto cramped =
        server.Handle(ByteSpan{authBytes}, MutableByteSpan{tiny});
    REQUIRE_FALSE(cramped.has_value());
    CHECK(cramped.error().Error() == EProtoError::BufferTooSmall);

    // The tag verified, but nothing was sent, so no session may be handed out.
    CHECK_FALSE(server.TakeResult().has_value());
    CHECK(serverSeen.Size() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "the result is available once and only once") {
    Harness peers;
    NonceMemory seen;
    std::vector<std::byte> scratch(SCRATCH);

    ClientHandshake client{peers.Id, Clone(peers.ClientAuth),
                           Clone(peers.ClientSeed), peers.ClientNonce, seen};

    // Nothing half-derived can be reached by asking early.
    CHECK_FALSE(client.TakeResult().has_value());

    REQUIRE(client.Start(MutableByteSpan{scratch}).has_value());
    CHECK_FALSE(client.TakeResult().has_value());
}

TEST_CASE_FIXTURE(SodiumFixture, "the peek reads the sid and nothing else") {
    Harness peers;
    const auto run = peers.Run();
    REQUIRE(run.Completed);

    const auto id = PeekHelloSessionId(ByteSpan{run.Hello});
    REQUIRE(id.has_value());
    CHECK(*id == peers.Id);

    // The agent routes on a Hello and only on a Hello.
    const auto wrong = PeekHelloSessionId(ByteSpan{run.Challenge});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().Error() == EProtoError::UnexpectedMessage);

    const auto cut = PeekHelloSessionId(ByteSpan{run.Hello}.first(6));
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().Error() == EProtoError::Truncated);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "two sessions of the same agent share no keys") {
    Harness first;
    Harness second{3, 3};

    const auto one = first.Run();
    const auto other = second.Run();
    REQUIRE(one.Completed);
    REQUIRE(other.Completed);

    CHECK(Bytes(one.FromClient->Keys.ClientToServer) !=
          Bytes(other.FromClient->Keys.ClientToServer));
}
