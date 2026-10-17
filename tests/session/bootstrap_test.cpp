/// The bootstrap of design.md §4 end to end, sans-I/O: what the client and
/// the agent write to each other over ssh, and everything a remote shell may
/// print around it.

#include "zet/session/bootstrap.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/byte_writer.hpp"
#include "zet/wire/frame.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace zet::wire;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

constexpr std::uint16_t PORT = 40'123;

crypto::Key MakeSeed(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(seed + i * 3);
    }
    return crypto::Key{bytes};
}

BootstrapNonce MakeNonce(std::uint8_t seed) {
    BootstrapNonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<std::byte>(seed * 7 + i);
    }
    return nonce;
}

ByteSpan AsBytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string OfferText(const BootstrapClient& client) {
    std::string out(BOOTSTRAP_OFFER_TEXT_SIZE, '\0');
    const auto n = client.WriteOffer(std::span{out});
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

std::string AcceptText(const BootstrapAgent& agent) {
    std::string out(BOOTSTRAP_ACCEPT_TEXT_SIZE, '\0');
    const auto n = agent.WriteAccept(std::span{out});
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

/// Feeds `stream` in chunks of `chunk` until a result or the end. Returns the
/// result and how many bytes of the stream it took.
template <typename TEnd>
std::pair<std::optional<BootstrapResult>, std::size_t> Run(
    TEnd& end, std::string_view stream, std::size_t chunk = 4096) {
    std::size_t taken = 0;
    for (std::size_t from = 0; from < stream.size(); from += chunk) {
        const auto piece = AsBytes(stream.substr(from, chunk));
        auto step = end.Feed(piece);
        REQUIRE(step.has_value());
        taken += step->Consumed;
        if (step->Result) {
            return {std::move(step->Result), taken};
        }
        REQUIRE(step->Consumed == piece.size());
    }
    return {std::nullopt, taken};
}

/// An agent that has read `client`'s Offer, and what it printed back.
struct Answered {
    BootstrapAgent Agent;
    BootstrapResult Result;
    std::string Accept;
};

Answered Answer(const BootstrapClient& client, std::uint8_t seed) {
    BootstrapAgent agent{MakeSeed(seed), MakeNonce(seed), PORT};
    auto [result, taken] = Run(agent, OfferText(client));
    REQUIRE(result.has_value());
    std::string accept = AcceptText(agent);
    return Answered{std::move(agent), std::move(*result), std::move(accept)};
}

bool SameSecrets(const SessionSecrets& a, const SessionSecrets& b) {
    return a.Id == b.Id && a.Auth.Expose() == b.Auth.Expose() &&
           a.ConnectionSeed.Expose() == b.ConnectionSeed.Expose();
}

/// One cleartext frame of whatever `write` puts in it, armored.
template <typename TWrite>
std::string ArmoredFrame(TWrite&& write, std::uint8_t epoch = 0) {
    std::array<std::byte, MAX_ARMORED_FRAME> body{};
    ByteWriter bodyWriter{MutableByteSpan{body}};
    REQUIRE(write(bodyWriter).has_value());
    std::array<std::byte, MAX_ARMORED_FRAME> frame{};
    ByteWriter writer{MutableByteSpan{frame}};
    REQUIRE(WriteFrameHeader(
        writer,
        FrameHeader{.Length = static_cast<std::uint32_t>(bodyWriter.Size()),
                    .Epoch = epoch}));
    REQUIRE(writer.WriteBytes(bodyWriter.Written()));
    std::string out(ArmoredSize(writer.Size()), '\0');
    const auto n = WriteArmor(writer.Written(), std::span{out});
    REQUIRE(n.has_value());
    return out;
}

std::string ArmoredAccept(const BootstrapAcceptMessage& accept) {
    return ArmoredFrame(
        [&](ByteWriter& w) { return WriteBootstrapAccept(w, accept); });
}

std::string ArmoredOffer(const BootstrapOfferMessage& offer) {
    return ArmoredFrame(
        [&](ByteWriter& w) { return WriteBootstrapOffer(w, offer); });
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "both ends of a bootstrap agree on the session") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    auto answered = Answer(client, 2);
    CHECK(answered.Agent.IsDone());
    CHECK(answered.Result.Port == PORT);

    auto [result, taken] = Run(client, answered.Accept);
    REQUIRE(result.has_value());
    CHECK(client.IsDone());
    CHECK(taken == answered.Accept.size());
    CHECK(result->Port == PORT);
    CHECK(result->Version == answered.Result.Version);
    CHECK(SameSecrets(result->Secrets, answered.Result.Secrets));
    CHECK(client.IgnoredBlocks() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "two bootstraps never share a session") {
    BootstrapClient first{MakeSeed(1), MakeNonce(1)};
    BootstrapClient second{MakeSeed(1), MakeNonce(9)};
    const auto a = Answer(first, 2);
    const auto b = Answer(second, 2);
    CHECK(a.Result.Secrets.Id != b.Result.Secrets.Id);
    CHECK(a.Result.Secrets.Auth.Expose() != b.Result.Secrets.Auth.Expose());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a noisy shell does not break it, however it is chunked") {
    BootstrapClient reference{MakeSeed(1), MakeNonce(1)};
    const auto answered = Answer(reference, 2);
    std::string shellOutput =
        "Welcome to Ubuntu\r\nfortune: you will reconnect\n";
    shellOutput += std::string(5000, '#') + "\n";
    shellOutput += "user@host:~$ ";
    shellOutput += answered.Accept;
    shellOutput += "logout\n";

    for (const std::size_t chunk : {1UL, 3UL, 17UL, 512UL, 65536UL}) {
        CAPTURE(chunk);
        BootstrapClient client{MakeSeed(1), MakeNonce(1)};
        auto [result, taken] = Run(client, shellOutput, chunk);
        REQUIRE(result.has_value());
        CHECK(SameSecrets(result->Secrets, answered.Result.Secrets));
        // What follows the Accept is left for the caller.
        CHECK(shellOutput.substr(taken).ends_with("logout\n"));
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an accept from another run is passed over for ours") {
    // The stale terminal of §4: an earlier bootstrap's output, replayed.
    BootstrapClient earlier{MakeSeed(5), MakeNonce(5)};
    const auto stale = Answer(earlier, 6);

    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    const auto answered = Answer(client, 2);

    auto [none, taken] = Run(client, stale.Accept);
    CHECK_FALSE(none.has_value());
    CHECK_FALSE(client.IsDone());
    CHECK(client.IgnoredBlocks() == 1);

    auto [result, rest] = Run(client, answered.Accept);
    REQUIRE(result.has_value());
    CHECK(SameSecrets(result->Secrets, answered.Result.Secrets));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "blocks that are not an accept are passed over") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    const auto answered = Answer(client, 2);

    std::string noise;
    // An Offer is not an Accept, even in perfect armor.
    noise += OfferText(client);
    // Armor around bytes that are no frame at all.
    noise += "\n" + std::string{ARMOR_BEGIN} + "\nZm9vYmFy\n" +
             std::string{ARMOR_END} + "\n";
    // An Accept for us in a frame claiming a sealed epoch.
    BootstrapAcceptMessage forged{.Version = PROTOCOL_VERSION_MAX,
                                  .ClientNonce = MakeNonce(1),
                                  .AgentNonce = MakeNonce(3),
                                  .Port = PORT};
    noise += ArmoredFrame(
        [&](ByteWriter& w) { return WriteBootstrapAccept(w, forged); }, 1);
    // A frame of the right type with a body cut short.
    noise += ArmoredFrame([](ByteWriter& w) {
        return w.WriteU8(
            static_cast<std::uint8_t>(EMessageType::BootstrapAccept));
    });

    auto [none, taken] = Run(client, noise);
    CHECK_FALSE(none.has_value());
    CHECK(client.IgnoredBlocks() == 4);
    auto [result, rest] = Run(client, answered.Accept);
    CHECK(result.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "our accept with a version we do not speak is final") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    BootstrapAcceptMessage accept{
        .Version = static_cast<std::uint16_t>(PROTOCOL_VERSION_MAX + 1),
        .AgentKey = crypto::KeyPairFromSeed(MakeSeed(2)).Public,
        .ClientNonce = MakeNonce(1),
        .AgentNonce = MakeNonce(2),
        .Port = PORT};
    auto step = client.Feed(AsBytes(ArmoredAccept(accept)));
    REQUIRE_FALSE(step.has_value());
    CHECK(step.error().Proto() == EProtoError::VersionMismatch);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "a public key that does not exchange is refused") {
    // All zeros is a low-order point: X25519 yields the zero secret, and
    // libsodium refuses it rather than hand back a key anyone can compute.
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    BootstrapAcceptMessage accept{.Version = PROTOCOL_VERSION_MAX,
                                  .AgentKey = {},
                                  .ClientNonce = MakeNonce(1),
                                  .AgentNonce = MakeNonce(2),
                                  .Port = PORT};
    auto step = client.Feed(AsBytes(ArmoredAccept(accept)));
    REQUIRE_FALSE(step.has_value());
    CHECK(step.error().Proto() == EProtoError::MalformedField);

    BootstrapAgent agent{MakeSeed(2), MakeNonce(2), PORT};
    BootstrapOfferMessage offer{.ClientKey = {}, .ClientNonce = MakeNonce(1)};
    auto refused = agent.Feed(AsBytes(ArmoredOffer(offer)));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Proto() == EProtoError::MalformedField);
    CHECK_FALSE(agent.IsDone());
}

TEST_CASE_FIXTURE(SodiumFixture, "the agent picks the highest common version") {
    BootstrapAgent agent{MakeSeed(2), MakeNonce(2), PORT};
    BootstrapOfferMessage offer{
        .VersionMin = PROTOCOL_VERSION_MIN,
        .VersionMax = static_cast<std::uint16_t>(PROTOCOL_VERSION_MAX + 5),
        .ClientKey = crypto::KeyPairFromSeed(MakeSeed(1)).Public,
        .ClientNonce = MakeNonce(1)};
    auto [result, taken] = Run(agent, ArmoredOffer(offer));
    REQUIRE(result.has_value());
    CHECK(result->Version == PROTOCOL_VERSION_MAX);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "an offer with no version in common is final") {
    BootstrapAgent agent{MakeSeed(2), MakeNonce(2), PORT};
    BootstrapOfferMessage offer{
        .VersionMin = static_cast<std::uint16_t>(PROTOCOL_VERSION_MAX + 1),
        .VersionMax = static_cast<std::uint16_t>(PROTOCOL_VERSION_MAX + 2),
        .ClientKey = crypto::KeyPairFromSeed(MakeSeed(1)).Public,
        .ClientNonce = MakeNonce(1)};
    auto step = agent.Feed(AsBytes(ArmoredOffer(offer)));
    REQUIRE_FALSE(step.has_value());
    CHECK(step.error().Proto() == EProtoError::VersionMismatch);
}

TEST_CASE_FIXTURE(SodiumFixture, "a garbled offer on stdin is final") {
    BootstrapAgent agent{MakeSeed(2), MakeNonce(2), PORT};
    auto step = agent.Feed(AsBytes(ArmoredFrame([](ByteWriter& w) {
        return w.WriteU8(
            static_cast<std::uint8_t>(EMessageType::BootstrapOffer));
    })));
    REQUIRE_FALSE(step.has_value());
    CHECK(step.error().Proto() == EProtoError::Truncated);
}

TEST_CASE_FIXTURE(SodiumFixture, "the agent passes over what is not an offer") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    const auto other = Answer(client, 3);
    BootstrapAgent agent{MakeSeed(2), MakeNonce(2), PORT};
    std::string agentInput = "stray\n" + other.Accept;
    agentInput +=
        ArmoredFrame([](ByteWriter& w) { return w.WriteU8(0x7E); }, 3);
    auto [none, taken] = Run(agent, agentInput);
    CHECK_FALSE(none.has_value());
    CHECK(agent.IgnoredBlocks() == 2);
    // No Accept to write before there is an Offer.
    std::string out(BOOTSTRAP_ACCEPT_TEXT_SIZE, '\0');
    CHECK(agent.WriteAccept(std::span{out}).error() ==
          EProtoError::UnexpectedMessage);

    auto [result, rest] = Run(agent, OfferText(client));
    CHECK(result.has_value());
}

TEST_CASE_FIXTURE(SodiumFixture, "a finished end reads nothing more") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    auto answered = Answer(client, 2);
    auto [result, taken] = Run(client, answered.Accept);
    REQUIRE(result.has_value());

    const auto again = client.Feed(AsBytes(answered.Accept));
    REQUIRE(again.has_value());
    CHECK(again->Consumed == 0);
    CHECK_FALSE(again->Result.has_value());

    const auto agentAgain = answered.Agent.Feed(AsBytes(OfferText(client)));
    REQUIRE(agentAgain.has_value());
    CHECK(agentAgain->Consumed == 0);
}

TEST_CASE_FIXTURE(SodiumFixture, "writing into too small a buffer is refused") {
    BootstrapClient client{MakeSeed(1), MakeNonce(1)};
    std::string small(BOOTSTRAP_OFFER_TEXT_SIZE - 1, '\0');
    CHECK(client.WriteOffer(std::span{small}).error() ==
          EProtoError::BufferTooSmall);
    auto answered = Answer(client, 2);
    std::string tiny(BOOTSTRAP_ACCEPT_TEXT_SIZE - 1, '\0');
    CHECK(answered.Agent.WriteAccept(std::span{tiny}).error() ==
          EProtoError::BufferTooSmall);
}
