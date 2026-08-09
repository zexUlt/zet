// The AEAD open path, against a key the input cannot know.
//
// Nothing here will ever decrypt, and that is the point: every byte sequence a
// peer can put on the wire has to come back as an error with the receiver
// untouched, rather than as a crash in the process that holds every session.
//
// The bytes do not go through ReadFrame — frame_fuzz owns the parser — so that
// they reach AeadOpen instead of being turned away by a length check. The
// first byte picks the epoch, which is what chooses between the current key,
// the prepared next one, and neither.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/sealed_frame.hpp"

namespace {

// Fixed, so that a finding reproduces from the artifact alone.
constexpr std::size_t KEY_SEED = 0x5A;
constexpr std::size_t SALT_SEED = 0xA5;

[[nodiscard]] zet::crypto::Key MakeKey() noexcept {
    zet::crypto::KeyBytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(KEY_SEED + i);
    }
    return zet::crypto::Key{bytes};
}

[[nodiscard]] zet::wire::DirectionSalt MakeSalt() noexcept {
    zet::wire::DirectionSalt salt{};
    for (std::size_t i = 0; i < salt.size(); ++i) {
        salt[i] = static_cast<std::byte>(SALT_SEED + i);
    }
    return salt;
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
    zet::ByteReader reader{
        zet::ByteSpan{reinterpret_cast<const std::byte*>(data), size}};

    const auto epoch = reader.ReadU8();
    if (!epoch) {
        return 0;
    }
    const zet::ByteSpan body = reader.Rest();
    if (body.size() > zet::wire::MAX_FRAME) {
        return 0;
    }

    // A fresh opener per input: libFuzzer replays an artifact on its own, and
    // state carried between runs would make what it replays a different case.
    zet::wire::FrameOpener opener{MakeKey(), MakeSalt()};

    const zet::wire::FrameView frame{
        .Header = {.Length = static_cast<std::uint32_t>(body.size()),
                   .Epoch = *epoch},
        .Body = body,
        .Consumed = zet::wire::HEADER_SIZE + body.size(),
    };

    std::vector<std::byte> out(body.size());
    const auto opened = opener.Open(zet::MutableByteSpan{out}, frame);

    // Forging a Poly1305 tag is not something a fuzzer does by luck; if this
    // ever fires, the AEAD wrapper is not authenticating anything.
    if (opened) {
        __builtin_trap();
    }

    // Invariant 2 at this layer: a rejected frame leaves the receiver where it
    // was. One injected byte that moved the counter would desynchronise the
    // stream for good.
    if (opener.Counter() != 0 || opener.Epoch() != 0) {
        __builtin_trap();
    }

    return 0;
}
