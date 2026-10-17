#include "zet/session/agent_session.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace zet;
using namespace zet::session;
using zet::wire::ClientHandshake;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;
using zet::wire::NonceMemory;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

/// Room for any handshake message several times over.
constexpr std::size_t SCRATCH = 256;

crypto::Key MakeKey(std::uint8_t seed) {
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

/// The client end of a session: what it was bootstrapped with, and the seed
/// it is on now. It moves its seed itself, as §8 has it do once the agent's
/// first sealed frame opens.
struct Client {
    wire::SessionId Id{};
    crypto::Key Auth;
    crypto::Key Seed;
    NonceMemory Seen;

    explicit Client(std::uint8_t master) {
        auto secrets = DeriveSessionSecrets(MakeKey(master));
        Id = secrets.Id;
        Auth = std::move(secrets.Auth);
        Seed = std::move(secrets.ConnectionSeed);
    }

    void MoveOn() { Seed = wire::AdvanceConnectionSeed(Seed); }
};

/// What one connection attempt came to on both ends.
struct Outcome {
    std::optional<wire::HandshakeResult> FromClient;
    std::optional<SessionResult<Resumed>> FromAgent;
    /// RememberedNonces() read after the whole handshake, before Finish.
    std::size_t RememberedBeforeFinish{0};
};

/// Drives Hello to AuthOk between `client` and `attempt`, then finishes the
/// attempt. Stops at the first refusal, as the real connection would.
Outcome Connect(Client& client, ResumeAttempt& attempt, SessionRef ref,
                std::uint8_t nonceSeed) {
    Outcome out;
    ClientHandshake shake{client.Id, Clone(client.Auth), Clone(client.Seed),
                          MakeNonce(nonceSeed), client.Seen};
    std::vector<std::byte> a(SCRATCH);
    std::vector<std::byte> b(SCRATCH);

    const auto hello = shake.Start(MutableByteSpan{a});
    REQUIRE(hello.has_value());
    const auto challenge =
        attempt.Handle(ByteSpan{a}.first(*hello), MutableByteSpan{b});
    if (challenge) {
        const auto auth = shake.Handle(ByteSpan{b}.first(challenge->Written),
                                       MutableByteSpan{a});
        REQUIRE(auth.has_value());
        const auto ok = attempt.Handle(ByteSpan{a}.first(auth->Written),
                                       MutableByteSpan{b});
        if (ok) {
            REQUIRE(
                shake.Handle(ByteSpan{b}.first(ok->Written), MutableByteSpan{a})
                    .has_value());
            out.FromClient = shake.TakeResult();
        }
    }
    out.RememberedBeforeFinish = ref.RememberedNonces();
    out.FromAgent = std::move(attempt).Finish();
    return out;
}

/// One whole resume of `session` by `client`, with a fresh nonce pair.
Outcome Resume(AgentSession& session, Client& client, std::uint8_t nonceSeed) {
    ResumeAttempt attempt{session.Ref(),
                          MakeNonce(static_cast<std::uint8_t>(nonceSeed + 1))};
    return Connect(client, attempt, session.Ref(), nonceSeed);
}

bool SameKeys(const wire::ConnectionKeys& left,
              const wire::ConnectionKeys& right) {
    return Bytes(left.ClientToServer) == Bytes(right.ClientToServer) &&
           Bytes(left.ServerToClient) == Bytes(right.ServerToClient) &&
           left.SaltClientToServer == right.SaltClientToServer &&
           left.SaltServerToClient == right.SaltServerToClient;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client holding K_auth gets a handle to its session") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    auto outcome = Resume(session, client, 10);
    REQUIRE(outcome.FromClient.has_value());
    REQUIRE(outcome.FromAgent.has_value());
    REQUIRE(outcome.FromAgent->has_value());

    auto& resumed = **outcome.FromAgent;
    CHECK(resumed.Handle.Id() == client.Id);
    CHECK(SameKeys(resumed.Result.Keys, outcome.FromClient->Keys));
    CHECK(outcome.FromClient->PeerReceiveOffset == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the session changes at Finish and not one step earlier") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    const auto outcome = Resume(session, client, 10);
    REQUIRE(outcome.FromAgent.has_value());
    REQUIRE(outcome.FromAgent->has_value());

    // AuthOk is written, the handshake is done, and still nothing is recorded:
    // the nonces go in only when the handle comes out.
    CHECK(outcome.RememberedBeforeFinish == 0);
    CHECK(session.Ref().RememberedNonces() == 2);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a peer without K_auth gets no handle and changes nothing") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client stranger{2};
    // The stranger read the sid off the wire; that is all it has.
    stranger.Id = session.Ref().Id();

    const auto outcome = Resume(session, stranger, 10);
    REQUIRE(outcome.FromAgent.has_value());
    REQUIRE_FALSE(outcome.FromAgent->has_value());
    CHECK(outcome.FromAgent->error() == ESessionError::NotAuthenticated);
    CHECK(session.Ref().RememberedNonces() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an attempt that never finished yields nothing") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ResumeAttempt attempt{session.Ref(), MakeNonce(1)};

    const auto result = std::move(attempt).Finish();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ESessionError::NotAuthenticated);
    CHECK(session.Ref().RememberedNonces() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an attempt for an unknown sid never finishes") {
    // §5 rule two: the decoy answers Hello like a session would, and fails
    // where a stranger fails, at tag_c.
    const crypto::Key localSecret = MakeKey(7);
    Client client{1};
    auto attempt =
        ResumeAttempt::ForUnknown(localSecret, client.Id, MakeNonce(1));

    AgentSession unrelated{DeriveSessionSecrets(MakeKey(3)), SessionStreams{}};
    const auto outcome = Connect(client, attempt, unrelated.Ref(), 10);
    REQUIRE(outcome.FromAgent.has_value());
    REQUIRE_FALSE(outcome.FromAgent->has_value());
    CHECK(outcome.FromAgent->error() == ESessionError::NotAuthenticated);
    CHECK_FALSE(outcome.FromClient.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the first connection of a session has no fallback keys") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    const auto outcome = Resume(session, client, 10);
    REQUIRE(outcome.FromAgent->has_value());
    CHECK_FALSE((*outcome.FromAgent)->Result.FallbackKeys.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "after a confirmed resume both ends meet on the next seed") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    auto first = Resume(session, client, 10);
    REQUIRE(first.FromAgent->has_value());
    REQUIRE((*first.FromAgent)->Handle.ConfirmResume(EResumeKeys::Current));
    client.MoveOn();

    const auto second = Resume(session, client, 20);
    REQUIRE(second.FromAgent->has_value());
    REQUIRE(second.FromClient.has_value());
    CHECK(SameKeys((*second.FromAgent)->Result.Keys, second.FromClient->Keys));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client whose answer was cut is met on the seed it kept") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    auto first = Resume(session, client, 10);
    REQUIRE(first.FromAgent->has_value());
    REQUIRE((*first.FromAgent)->Handle.ConfirmResume(EResumeKeys::Current));
    // The agent's first sealed frame never arrived, so the client stays put.

    auto second = Resume(session, client, 20);
    REQUIRE(second.FromAgent->has_value());
    REQUIRE(second.FromClient.has_value());
    auto& resumed = **second.FromAgent;
    CHECK_FALSE(SameKeys(resumed.Result.Keys, second.FromClient->Keys));
    REQUIRE(resumed.Result.FallbackKeys.has_value());
    CHECK(SameKeys(*resumed.Result.FallbackKeys, second.FromClient->Keys));

    // Resume opened under the fallback keys. This time the answer arrives.
    REQUIRE(resumed.Handle.ConfirmResume(EResumeKeys::Previous));
    client.MoveOn();

    const auto third = Resume(session, client, 30);
    REQUIRE(third.FromAgent->has_value());
    REQUIRE(third.FromClient.has_value());
    CHECK(SameKeys((*third.FromAgent)->Result.Keys, third.FromClient->Keys));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the previous seed cannot be confirmed when there is none") {
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};

    auto outcome = Resume(session, client, 10);
    REQUIRE(outcome.FromAgent->has_value());
    const auto confirmed =
        (*outcome.FromAgent)->Handle.ConfirmResume(EResumeKeys::Previous);
    REQUIRE_FALSE(confirmed.has_value());
    CHECK(confirmed.error() == ESessionError::UnknownSeed);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "keys of connection n are gone once n+1 is confirmed") {
    // The M2 criterion from §17, read from the outside: after two confirmed
    // resumes, nothing the session can derive any more opens connection n.
    AgentSession session{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    Client client{1};
    const crypto::Key seedOfFirst = Clone(client.Seed);

    auto first = Resume(session, client, 10);
    REQUIRE(first.FromAgent->has_value());
    REQUIRE((*first.FromAgent)->Handle.ConfirmResume(EResumeKeys::Current));
    client.MoveOn();

    auto second = Resume(session, client, 20);
    REQUIRE(second.FromAgent->has_value());
    REQUIRE((*second.FromAgent)->Handle.ConfirmResume(EResumeKeys::Current));
    client.MoveOn();

    // A client that somehow came back on the very first seed.
    Client stale{1};
    stale.Seed = Clone(seedOfFirst);
    auto probe = Resume(session, stale, 30);
    REQUIRE(probe.FromAgent->has_value());
    REQUIRE(probe.FromClient.has_value());
    auto& resumed = **probe.FromAgent;
    CHECK_FALSE(SameKeys(resumed.Result.Keys, probe.FromClient->Keys));
    REQUIRE(resumed.Result.FallbackKeys.has_value());
    CHECK_FALSE(SameKeys(*resumed.Result.FallbackKeys, probe.FromClient->Keys));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the second resume takes the session over from the first") {
    std::vector<std::byte> storage(64);
    AgentSession session{DeriveSessionSecrets(MakeKey(1)),
                         SessionStreams{.Storage = MutableByteSpan{storage},
                                        .Now = Timestamp{},
                                        .Limits = SessionLimits{}}};
    Client client{1};

    // The shell wrote while nobody was there; the first connection asks for
    // all of it.
    const std::vector<std::byte> output(12, std::byte{0x41});
    REQUIRE(session.Write(ByteSpan{output}) == output.size());

    auto first = Resume(session, client, 10);
    REQUIRE(first.FromAgent->has_value());
    auto& one = (*first.FromAgent)->Handle;
    REQUIRE(one.ConfirmResume(EResumeKeys::Current));
    REQUIRE(one.Attach(0));
    CHECK(session.State() == ESessionState::Live);
    CHECK(one.Output().SendNext() == 0);
    client.MoveOn();

    // The client changed networks; the old socket is still half-open.
    auto second = Resume(session, client, 20);
    REQUIRE(second.FromAgent->has_value());
    auto& two = (*second.FromAgent)->Handle;
    REQUIRE(two.ConfirmResume(EResumeKeys::Current));
    REQUIRE(two.Attach(7));
    CHECK(two.Output().SendNext() == 7);

    CHECK_FALSE(one.IsCurrent());
    CHECK(two.IsCurrent());
    // The half-open socket finally times out. It must not detach the session
    // from the connection that replaced it.
    one.Detach(Timestamp::FromNanos(1));
    CHECK(session.State() == ESessionState::Live);

    two.Detach(Timestamp::FromNanos(2));
    CHECK(session.State() == ESessionState::Detached);
}
