#include "zet/session/client_session.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "zet/session/agent_session.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;
using zet::wire::NonceMemory;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

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

std::vector<std::byte> Bytes(const crypto::Key& key) {
    return {key.Expose().begin(), key.Expose().end()};
}

bool SameKeys(const wire::ConnectionKeys& left,
              const wire::ConnectionKeys& right) {
    return Bytes(left.ClientToServer) == Bytes(right.ClientToServer) &&
           Bytes(left.ServerToClient) == Bytes(right.ServerToClient) &&
           left.SaltClientToServer == right.SaltClientToServer &&
           left.SaltServerToClient == right.SaltServerToClient;
}

/// Drives one full handshake from the client's Hello to the client's receipt of
/// AuthOk, then calls Finish() on both sides and returns both results.
/// Always finishes both attempts even when an intermediate step fails.
struct ConnectResult {
    SessionResult<ClientResumed> FromClient{
        std::unexpected(ESessionError::NotAuthenticated)};
    SessionResult<Resumed> FromAgent{
        std::unexpected(ESessionError::NotAuthenticated)};
    std::size_t ClientNoncesBeforeFinish{0};
};

ConnectResult Connect(ClientSession& client, AgentSession& agent,
                      std::uint8_t clientNonceSeed,
                      std::uint8_t serverNonceSeed) {
    ConnectResult out;
    ClientResumeAttempt clientAttempt{client, MakeNonce(clientNonceSeed)};
    ResumeAttempt agentAttempt{agent.Ref(), MakeNonce(serverNonceSeed)};

    std::vector<std::byte> a(SCRATCH);
    std::vector<std::byte> b(SCRATCH);

    const auto helloSize = clientAttempt.Start(MutableByteSpan{a});
    REQUIRE(helloSize.has_value());

    const auto challenge =
        agentAttempt.Handle(ByteSpan{a}.first(*helloSize), MutableByteSpan{b});
    if (challenge) {
        const auto auth = clientAttempt.Handle(
            ByteSpan{b}.first(challenge->Written), MutableByteSpan{a});
        REQUIRE(auth.has_value());

        const auto authOk = agentAttempt.Handle(
            ByteSpan{a}.first(auth->Written), MutableByteSpan{b});
        if (authOk) {
            const auto done = clientAttempt.Handle(
                ByteSpan{b}.first(authOk->Written), MutableByteSpan{a});
            REQUIRE(done.has_value());
        }
    }

    out.ClientNoncesBeforeFinish = client.RememberedNonces();
    out.FromClient = std::move(clientAttempt).Finish();
    out.FromAgent = std::move(agentAttempt).Finish();
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "a client and an agent complete a resume and agree on keys") {
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    auto out = Connect(client, agent, 10, 20);
    REQUIRE(out.FromClient.has_value());
    REQUIRE(out.FromAgent.has_value());

    auto& clientResumed = *out.FromClient;
    auto& agentResumed = *out.FromAgent;
    CHECK(clientResumed.Handle.Id() == client.Id());
    CHECK(SameKeys(clientResumed.Result.Keys, agentResumed.Result.Keys));
    CHECK(clientResumed.Result.PeerReceiveOffset == 0);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a fake agent without K_auth never yields a client handle and leaves the "
    "client session untouched") {
    // The agent knows the sid (read off the wire) but not K_auth.
    AgentSession realAgent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    // Build a fake agent session from different secrets; give it the right sid
    // so routing would succeed, but the handshake cannot.
    AgentSession fakeAgent{DeriveSessionSecrets(MakeKey(2)), SessionStreams{}};

    auto out = Connect(client, fakeAgent, 10, 20);
    REQUIRE_FALSE(out.FromClient.has_value());
    CHECK(out.FromClient.error() == ESessionError::NotAuthenticated);
    // RememberedNonces stays 0: no mutation before authentication.
    CHECK(client.RememberedNonces() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the client session changes at Finish and not one step "
                  "earlier") {
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    const auto out = Connect(client, agent, 10, 20);
    REQUIRE(out.FromClient.has_value());

    // AuthOk has arrived and the handshake is complete, yet the nonces have
    // not been recorded yet — they go in only when Finish() hands out the
    // handle.
    CHECK(out.ClientNoncesBeforeFinish == 0);
    CHECK(client.RememberedNonces() == 2);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "after ConfirmAgentFrame the next resume against an agent that confirmed "
    "Current agrees on keys") {
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    auto first = Connect(client, agent, 10, 20);
    REQUIRE(first.FromClient.has_value());
    REQUIRE(first.FromAgent.has_value());

    // Both sides confirm the first connection.
    first.FromClient->Handle.ConfirmAgentFrame();
    REQUIRE(first.FromAgent->Handle.ConfirmResume(EResumeKeys::Current));

    const auto second = Connect(client, agent, 30, 40);
    REQUIRE(second.FromClient.has_value());
    REQUIRE(second.FromAgent.has_value());
    CHECK(SameKeys(second.FromClient->Result.Keys,
                   second.FromAgent->Result.Keys));
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "calling ConfirmAgentFrame twice for the same handle is harmless") {
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    auto first = Connect(client, agent, 10, 20);
    REQUIRE(first.FromClient.has_value());
    REQUIRE(first.FromAgent.has_value());

    auto& handle = first.FromClient->Handle;
    handle.ConfirmAgentFrame();
    handle.ConfirmAgentFrame();  // second call must not advance the seed again
    REQUIRE(first.FromAgent->Handle.ConfirmResume(EResumeKeys::Current));

    // If the seed advanced twice, the second connection would fail.
    const auto second = Connect(client, agent, 30, 40);
    REQUIRE(second.FromClient.has_value());
    REQUIRE(second.FromAgent.has_value());
    CHECK(SameKeys(second.FromClient->Result.Keys,
                   second.FromAgent->Result.Keys));
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a client that did not confirm still agrees via FallbackKeys and both "
    "converge after the agent confirms Previous") {
    // End-to-end ADR 024 scenario.
    //
    // Connection 1: Both ends do the handshake. The agent confirms (moves to
    // seed n+1, keeps n as Previous). The client does NOT confirm — the agent's
    // first sealed frame was cut.
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    auto conn1 = Connect(client, agent, 10, 20);
    REQUIRE(conn1.FromClient.has_value());
    REQUIRE(conn1.FromAgent.has_value());
    // Agent confirms. Client stays on seed n.
    REQUIRE(conn1.FromAgent->Handle.ConfirmResume(EResumeKeys::Current));
    // (client does NOT call ConfirmAgentFrame)

    // Connection 2: The client is still on seed n; the agent tries n+1 first
    // (current), fails, then tries n (previous / FallbackKeys) and succeeds.
    auto conn2 = Connect(client, agent, 30, 40);
    REQUIRE(conn2.FromClient.has_value());
    REQUIRE(conn2.FromAgent.has_value());

    auto& clientResumed2 = *conn2.FromClient;
    auto& agentResumed2 = *conn2.FromAgent;

    // The client opened under the fallback keys; the agent's current keys
    // are different.
    CHECK_FALSE(
        SameKeys(clientResumed2.Result.Keys, agentResumed2.Result.Keys));
    REQUIRE(agentResumed2.Result.FallbackKeys.has_value());
    CHECK(SameKeys(clientResumed2.Result.Keys,
                   *agentResumed2.Result.FallbackKeys));

    // Now the agent confirms Previous: it rolls back to seed n, then advances
    // to n+1 again, keeping n as Previous. The client confirms its frame
    // arrived: it advances from n to n+1.
    REQUIRE(agentResumed2.Handle.ConfirmResume(EResumeKeys::Previous));
    clientResumed2.Handle.ConfirmAgentFrame();

    // Connection 3: Both ends are on seed n+1. They must agree on keys.
    const auto conn3 = Connect(client, agent, 50, 60);
    REQUIRE(conn3.FromClient.has_value());
    REQUIRE(conn3.FromAgent.has_value());
    CHECK(
        SameKeys(conn3.FromClient->Result.Keys, conn3.FromAgent->Result.Keys));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an AuthOk the agent did not sign yields no client handle") {
    // §5 rule four, at the session level. The agent above that lacks K_auth
    // fails at tag_c and never answers, so it never tests the client's own
    // check. Here the real agent answers and the answer is bent on the way:
    // every byte after the type — the tag and the offset rule five binds
    // into it — must cost the client its handle and change nothing.
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    // The length of AuthOk, learned from one untouched exchange.
    std::size_t authOkSize = 0;
    {
        ClientSession client{DeriveSessionSecrets(MakeKey(1)),
                             SessionStreams{}};
        ClientResumeAttempt clientAttempt{client, MakeNonce(1)};
        ResumeAttempt agentAttempt{agent.Ref(), MakeNonce(2)};
        std::vector<std::byte> a(SCRATCH);
        std::vector<std::byte> b(SCRATCH);
        const auto hello = clientAttempt.Start(MutableByteSpan{a});
        REQUIRE(hello.has_value());
        const auto challenge =
            agentAttempt.Handle(ByteSpan{a}.first(*hello), MutableByteSpan{b});
        REQUIRE(challenge.has_value());
        const auto auth = clientAttempt.Handle(
            ByteSpan{b}.first(challenge->Written), MutableByteSpan{a});
        REQUIRE(auth.has_value());
        const auto ok = agentAttempt.Handle(ByteSpan{a}.first(auth->Written),
                                            MutableByteSpan{b});
        REQUIRE(ok.has_value());
        authOkSize = ok->Written;
    }
    REQUIRE(authOkSize > 1);

    for (std::size_t offset = 1; offset < authOkSize; ++offset) {
        CAPTURE(offset);
        // A fresh agent session each time: the nonces must not repeat into a
        // NonceReused refusal that would pass this test for the wrong reason.
        AgentSession fresh{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};
        ClientSession client{DeriveSessionSecrets(MakeKey(1)),
                             SessionStreams{}};
        ClientResumeAttempt clientAttempt{client, MakeNonce(3)};
        ResumeAttempt agentAttempt{fresh.Ref(), MakeNonce(4)};
        std::vector<std::byte> a(SCRATCH);
        std::vector<std::byte> b(SCRATCH);

        const auto hello = clientAttempt.Start(MutableByteSpan{a});
        REQUIRE(hello.has_value());
        const auto challenge =
            agentAttempt.Handle(ByteSpan{a}.first(*hello), MutableByteSpan{b});
        REQUIRE(challenge.has_value());
        const auto auth = clientAttempt.Handle(
            ByteSpan{b}.first(challenge->Written), MutableByteSpan{a});
        REQUIRE(auth.has_value());
        const auto ok = agentAttempt.Handle(ByteSpan{a}.first(auth->Written),
                                            MutableByteSpan{b});
        REQUIRE(ok.has_value());

        std::vector<std::byte> bent{
            b.begin(), b.begin() + static_cast<std::ptrdiff_t>(ok->Written)};
        bent.at(offset) ^= std::byte{0x01};
        const auto done =
            clientAttempt.Handle(ByteSpan{bent}, MutableByteSpan{a});
        CHECK_FALSE(done.has_value());

        const auto finished = std::move(clientAttempt).Finish();
        REQUIRE_FALSE(finished.has_value());
        CHECK(finished.error() == ESessionError::NotAuthenticated);
        CHECK(client.RememberedNonces() == 0);
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the keyboard's door and the handle reach the same streams") {
    std::vector<std::byte> storage(64);
    const SessionLimits limits{
        .ResendMax = 8, .DisconnectMax = 16, .OrphanTimeout = Duration::zero()};
    ClientSession client{DeriveSessionSecrets(MakeKey(1)),
                         SessionStreams{.Storage = MutableByteSpan{storage},
                                        .Now = Timestamp{},
                                        .Limits = limits}};
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)), SessionStreams{}};

    CHECK(client.State() == ESessionState::Detached);
    CHECK(client.AcceptsWrites());
    CHECK(client.ReceiveOffset() == 0);

    // Typed while offline: held back at the detached ceiling, never dropped.
    const std::vector<std::byte> typed(20, std::byte{0x61});
    REQUIRE(client.Write(ByteSpan{typed}) == 16);
    CHECK_FALSE(client.AcceptsWrites());

    auto result = Connect(client, agent, 1, 2);
    REQUIRE(result.FromClient.has_value());
    auto& handle = result.FromClient->Handle;
    REQUIRE(handle.Attach(0));
    CHECK(handle.IsCurrent());
    CHECK(client.State() == ESessionState::Live);
    CHECK(handle.Output().End() == 16);
    CHECK(handle.Input().RecvOffset() == 0);

    handle.Detach(Timestamp::FromNanos(1));
    CHECK(client.State() == ESessionState::Detached);
    CHECK_FALSE(handle.IsCurrent());

    handle.Kill(ESessionError::SessionReaped);
    CHECK(client.State() == ESessionState::Reaped);
    const auto late = client.Write(ByteSpan{typed});
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error() == ESessionError::SessionReaped);
}
