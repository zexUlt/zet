// The bootstrap of §4, fed what a remote shell can print.
//
// Both automata read text nobody vouches for: the client reads the agent's
// stdout through whatever .bashrc and motd add to it, the agent reads its
// stdin. The input is one such stream, cut into chunks of a size its first
// byte picks, so that every way a marker or a line can straddle two reads is
// in reach.
//
// Asserted, beyond not crashing under ASan and UBSan:
//   - a Feed never claims more than it was given, and reads nothing once done;
//   - a client only finishes on an Accept that echoes its own nonce, with a
//     version it speaks and a real port;
//   - whatever an agent finishes on, the Accept it writes scans back as one
//     block carrying that same nonce;
//   - every block the scanner yields survives being armored and scanned again.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/bootstrap.hpp"
#include "zet/wire/armor.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace zet::wire;

// Fixed, so that a finding reproduces from the artifact alone. The seed
// corpus is built against these: CLIENT_NONCE is what an Accept in it echoes.
constexpr std::uint8_t CLIENT_SEED = 0x31;
constexpr std::uint8_t AGENT_SEED = 0x52;
constexpr std::uint8_t CLIENT_NONCE_START = 3;
constexpr std::uint8_t CLIENT_NONCE_STEP = 7;
constexpr std::uint8_t AGENT_NONCE_START = 0xA0;
constexpr std::uint16_t AGENT_PORT = 2022;

// Chunks from one byte up to this: past a line's length, longer chunks add
// nothing a shorter one does not already exercise.
constexpr std::size_t MAX_CHUNK = 256;

crypto::Key MakeKey(std::uint8_t seed) noexcept {
    crypto::KeyBytes bytes{};
    std::uint8_t value = seed;
    for (auto& byte : bytes) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + 5);
    }
    return crypto::Key{bytes};
}

BootstrapNonce MakeNonce(std::uint8_t start, std::uint8_t step) noexcept {
    BootstrapNonce nonce{};
    std::uint8_t value = start;
    for (auto& byte : nonce) {
        byte = std::byte{value};
        value = static_cast<std::uint8_t>(value + step);
    }
    return nonce;
}

ByteSpan AsBytes(std::span<const char> text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

template <typename TEnd, typename TOnResult>
void Drive(TEnd& end, ByteSpan stream, std::size_t chunk,
           TOnResult&& onResult) noexcept {
    while (!stream.empty()) {
        const auto piece = stream.first(std::min(chunk, stream.size()));
        const auto step = end.Feed(piece);
        if (!step) {
            return;
        }
        if (step->Consumed > piece.size()) {
            __builtin_trap();
        }
        if (step->Result) {
            onResult(*step->Result);
            const auto after = end.Feed(piece);
            if (!after || after->Consumed != 0 || after->Result) {
                __builtin_trap();
            }
            return;
        }
        if (step->Consumed != piece.size()) {
            __builtin_trap();
        }
        stream = stream.subspan(piece.size());
    }
}

/// The Accept an agent wrote, scanned back: exactly one block, an Accept, our
/// nonce in it.
void CheckAccept(const BootstrapAgent& agent,
                 const BootstrapNonce& expected) noexcept {
    std::array<char, BOOTSTRAP_ACCEPT_TEXT_SIZE> text{};
    const auto written = agent.WriteAccept(std::span{text});
    if (!written) {
        __builtin_trap();
    }
    ArmorScanner scanner;
    const auto scan = scanner.Feed(AsBytes(std::span{text}.first(*written)));
    if (!scan.Block || scan.Consumed != *written) {
        __builtin_trap();
    }
    const auto frame = ReadFrame(*scan.Block, MAX_HANDSHAKE_FRAME);
    if (!frame) {
        __builtin_trap();
    }
    ByteReader reader{frame->Body};
    if (ParseMessageType(reader) != EMessageType::BootstrapAccept) {
        __builtin_trap();
    }
    const auto accept = ParseBootstrapAccept(reader);
    if (!accept || accept->ClientNonce != expected ||
        accept->Port != AGENT_PORT) {
        __builtin_trap();
    }
}

/// Every block the stream holds, armored again and scanned again: the same
/// bytes come back.
void CheckArmorRoundTrip(ByteSpan stream, std::size_t chunk) noexcept {
    ArmorScanner scanner;
    while (!stream.empty()) {
        const auto piece = stream.first(std::min(chunk, stream.size()));
        const auto scan = scanner.Feed(piece);
        if (scan.Consumed > piece.size() ||
            (!scan.Block && scan.Consumed != piece.size())) {
            __builtin_trap();
        }
        if (scan.Block) {
            std::array<char, ArmoredSize(MAX_ARMORED_FRAME)> text{};
            const auto written = WriteArmor(*scan.Block, std::span{text});
            if (!written) {
                __builtin_trap();
            }
            ArmorScanner again;
            const auto rescan =
                again.Feed(AsBytes(std::span{text}.first(*written)));
            if (!rescan.Block ||
                !std::ranges::equal(*rescan.Block, *scan.Block)) {
                __builtin_trap();
            }
        }
        stream = stream.subspan(scan.Consumed);
    }
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    if (!crypto::Init()) {
        __builtin_trap();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const ByteSpan input{reinterpret_cast<const std::byte*>(data), size};
    const std::size_t chunk =
        1 + std::to_integer<std::size_t>(input.front()) % MAX_CHUNK;
    const ByteSpan stream = input.subspan(1);

    const BootstrapNonce clientNonce =
        MakeNonce(CLIENT_NONCE_START, CLIENT_NONCE_STEP);
    BootstrapClient client{MakeKey(CLIENT_SEED), clientNonce};
    Drive(client, stream, chunk, [](const BootstrapResult& result) {
        if (result.Port == 0 || result.Version < PROTOCOL_VERSION_MIN ||
            result.Version > PROTOCOL_VERSION_MAX) {
            __builtin_trap();
        }
    });

    BootstrapAgent agent{MakeKey(AGENT_SEED), MakeNonce(AGENT_NONCE_START, 1),
                         AGENT_PORT};
    Drive(agent, stream, chunk, [&](const BootstrapResult& result) {
        if (result.Port != AGENT_PORT) {
            __builtin_trap();
        }
        // The nonce the agent must echo is the one in the Offer it read; the
        // only copy of it is in the Accept, so read it back from there and
        // check it against a fresh scan of the stream.
        ArmorScanner scanner;
        ByteSpan rest = stream;
        while (!rest.empty()) {
            const auto scan = scanner.Feed(rest);
            rest = rest.subspan(scan.Consumed);
            if (!scan.Block) {
                continue;
            }
            // The same test the agent applies: one whole cleartext frame.
            const auto frame = ReadFrame(*scan.Block, MAX_HANDSHAKE_FRAME);
            if (!frame || frame->Consumed != scan.Block->size() ||
                frame->Header.Epoch != 0) {
                continue;
            }
            ByteReader reader{frame->Body};
            if (ParseMessageType(reader) != EMessageType::BootstrapOffer) {
                continue;
            }
            if (const auto offer = ParseBootstrapOffer(reader)) {
                CheckAccept(agent, offer->ClientNonce);
                return;
            }
        }
        __builtin_trap();
    });

    CheckArmorRoundTrip(stream, chunk);
    return 0;
}
