// The resume handshake of §5, driven by someone who knows nothing.
//
// This is the one automaton in zet that decides who gets in, and it decides it
// from unauthenticated bytes. The input here is exactly what a stranger can
// send: it does not hold K_auth, so it cannot compute tag_c, and every property
// asserted below follows from that. Reaching Complete would mean a tag verified
// against a key the input never had.
//
// Both ends are driven, because both read cleartext from a peer that has proved
// nothing yet: the agent until tag_c, the client until tag_s.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/key_schedule.hpp"

namespace {

// A whole handshake is four messages; the rest are there to poke an automaton
// that has already refused one. Beyond that a longer sequence only spends
// budget re-deriving the same dead state.
constexpr std::size_t MAX_MESSAGES = 8;

// Wider than any handshake message — the widest is Hello at forty-one bytes —
// so that BufferTooSmall never stands in for the refusals this target is after.
constexpr std::size_t SCRATCH_SIZE = 256;

// Fixed, so that a finding reproduces from the artifact alone.
constexpr std::uint8_t SESSION_MASTER_SEED = 0x11;
constexpr std::uint8_t AGENT_LOCAL_SEED = 0x77;
constexpr std::uint8_t CLIENT_NONCE_SEED = 0x10;
constexpr std::uint8_t SERVER_NONCE_SEED = 0x90;
constexpr std::uint64_t SERVER_RECEIVE_OFFSET = 0x0102'0304'0506'0708;

[[nodiscard]] zet::crypto::Key MakeKey(std::uint8_t seed) noexcept {
    zet::crypto::KeyBytes bytes{};
    std::uint8_t value = seed;
    for (auto& byte : bytes) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 5);
    }
    return zet::crypto::Key{bytes};
}

[[nodiscard]] zet::wire::HandshakeNonce MakeNonce(std::uint8_t seed) noexcept {
    zet::wire::HandshakeNonce nonce{};
    std::uint8_t value = seed;
    for (auto& byte : nonce) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 11);
    }
    return nonce;
}

[[nodiscard]] zet::crypto::Key Clone(const zet::crypto::Key& key) noexcept {
    return zet::crypto::Key{key.Expose()};
}

/// The messages one input carries, as views into it.
struct MessageList {
    std::array<zet::ByteSpan, MAX_MESSAGES> Items{};
    std::size_t Count{0};
};

/// Cuts the input into length-prefixed messages: one byte of count, then that
/// many bytes of body, repeated.
///
/// A count larger than what is left takes the remainder rather than ending the
/// list, so that shrinking an input keeps delivering a message where it used
/// to — otherwise every mutation near the end would collapse to the empty case.
[[nodiscard]] MessageList Split(zet::ByteSpan input) noexcept {
    MessageList out;
    zet::ByteReader reader{input};
    while (out.Count < MAX_MESSAGES && !reader.Exhausted()) {
        const auto count = reader.ReadU8();
        if (!count) {
            break;
        }
        const std::size_t wanted =
            std::min(static_cast<std::size_t>(*count), reader.Remaining());
        const auto body = reader.ReadBytes(wanted);
        if (!body) {
            break;
        }
        out.Items[out.Count] = *body;
        ++out.Count;
    }
    return out;
}

/// What one Handle reported, kept so that two runs of the same input can be
/// held against each other.
struct Outcome {
    bool Refused{false};
    std::size_t Written{0};
    zet::EProtoError Error{zet::EProtoError::Truncated};
};

struct Trace {
    std::array<Outcome, MAX_MESSAGES> Items{};
    std::size_t Count{0};
};

template <typename THandshake>
[[nodiscard]] Trace Drive(THandshake& shake, const MessageList& messages,
                          const zet::wire::NonceMemory& seen) noexcept {
    std::array<std::byte, SCRATCH_SIZE> scratch{};
    Trace trace;
    bool refused = false;

    for (std::size_t i = 0; i < messages.Count; ++i) {
        const auto step =
            shake.Handle(messages.Items[i], zet::MutableByteSpan{scratch});

        Outcome outcome;
        if (step) {
            // A refusal is terminal. The step that failed already folded its
            // message into the transcript, so answering a later one would
            // continue an exchange the two ends no longer agree on.
            if (refused) {
                __builtin_trap();
            }
            // Completing means a tag verified against a key the input does not
            // have — that is a stranger being let in, not a lucky guess.
            if (step->Complete) {
                __builtin_trap();
            }
            outcome.Written = step->Written;
        } else {
            // §5: everything before AuthOk is cleartext and proves nothing, so
            // no error out of it may reap the session whose sid it names.
            if (step.error().Disposition() == zet::EDisposition::KillSession) {
                __builtin_trap();
            }
            outcome.Refused = true;
            outcome.Error = step.error().Error();
            refused = true;
        }

        trace.Items[trace.Count] = outcome;
        ++trace.Count;
    }

    // Keys come only from a handshake that finished, and none of these can.
    if (shake.TakeResult().has_value()) {
        __builtin_trap();
    }
    // Rule three of §5 is only worth having while unauthenticated traffic
    // cannot flush the buffer: an unfinished handshake remembers nothing, or a
    // stranger sending Hellos would blind the repeat check.
    if (seen.Size() != 0) {
        __builtin_trap();
    }
    return trace;
}

[[nodiscard]] bool SameShape(const Trace& left, const Trace& right) noexcept {
    if (left.Count != right.Count) {
        return false;
    }
    for (std::size_t i = 0; i < left.Count; ++i) {
        const Outcome& one = left.Items[i];
        const Outcome& other = right.Items[i];
        if (one.Refused != other.Refused || one.Written != other.Written) {
            return false;
        }
        if (one.Refused && one.Error != other.Error) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    // Once per process: libsodium wants to be initialised before a second
    // thread exists, and per-run would show up as the whole cost of fuzzing.
    if (!zet::crypto::Init()) {
        __builtin_trap();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const zet::ByteSpan input{reinterpret_cast<const std::byte*>(data), size};
    const MessageList messages = Split(input);

    const auto session =
        zet::wire::DeriveSessionSecrets(MakeKey(SESSION_MASTER_SEED));
    const auto decoy =
        zet::wire::DeriveDecoySecrets(MakeKey(AGENT_LOCAL_SEED), session.Id);
    const auto serverNonce = MakeNonce(SERVER_NONCE_SEED);

    // Fresh state per input: libFuzzer replays an artifact on its own, and
    // anything carried between runs would make the replay a different case.
    zet::wire::NonceMemory knownSeen;
    zet::wire::ServerHandshake known{Clone(session.Auth),
                                     Clone(session.ConnectionSeed), serverNonce,
                                     SERVER_RECEIVE_OFFSET, knownSeen};
    const Trace fromKnown = Drive(known, messages, knownSeen);

    // Rule two of §5. The automaton is never told whether the sid was found, so
    // the run against a decoy may not differ in what it answers or in how it
    // refuses. Nonce and offset are held equal on purpose: the secrets are the
    // only difference, and they are the thing that must not show.
    zet::wire::NonceMemory decoySeen;
    zet::wire::ServerHandshake unknown{Clone(decoy.Auth),
                                       Clone(decoy.ConnectionSeed), serverNonce,
                                       SERVER_RECEIVE_OFFSET, decoySeen};
    const Trace fromUnknown = Drive(unknown, messages, decoySeen);

    // A difference here is an oracle for whether a session exists, readable
    // without authenticating — the answer ET gave away in three named statuses.
    if (!SameShape(fromKnown, fromUnknown)) {
        __builtin_trap();
    }

    // The client faces the same stranger: whatever answers its Hello is
    // unproved until tag_s verifies, which it cannot here.
    zet::wire::NonceMemory clientSeen;
    zet::wire::ClientHandshake client{session.Id, Clone(session.Auth),
                                      Clone(session.ConnectionSeed),
                                      MakeNonce(CLIENT_NONCE_SEED), clientSeen};

    std::array<std::byte, SCRATCH_SIZE> scratch{};
    // Nothing about this call depends on the input — fixed secrets, an empty
    // nonce memory, a buffer wider than Hello — so a refusal is a defect.
    if (!client.Start(zet::MutableByteSpan{scratch})) {
        __builtin_trap();
    }
    std::ignore = Drive(client, messages, clientSeen);

    return 0;
}
