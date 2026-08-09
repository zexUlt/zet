#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/limits.hpp"

namespace zet::wire {

/// Per-direction half of the nonce, derived once and then fixed. The other half
/// is the frame counter, so the two together never repeat within a key.
inline constexpr std::size_t SALT_SIZE =
    crypto::NONCE_SIZE - sizeof(std::uint64_t);
using DirectionSalt = std::array<std::byte, SALT_SIZE>;

/// Frames one key is allowed to seal, from docs/design.md §8. Well below any
/// birthday bound; the point is that a counter that cannot be exhausted removes
/// the question of what happens when it wraps.
inline constexpr std::uint64_t MAX_FRAMES_PER_EPOCH = 1ULL << 24;

/// Bytes one key is allowed to seal, from §8. A key that has moved a gigabyte
/// has been alive long enough to be worth replacing regardless of frame count.
inline constexpr std::uint64_t MAX_BYTES_PER_EPOCH = 1ULL << 30;

/// The KDF context that ratchets a key forward. Its only job is to separate
/// this derivation from every other use of the same master.
inline constexpr std::string_view REKEY_CONTEXT = "zet-rkey";
static_assert(REKEY_CONTEXT.size() == crypto::CONTEXT_SIZE);

/// Seals frames for one direction of one connection.
///
/// The counter is never sent. It is implicit in the position of the frame in
/// the stream, so a peer that falls out of step fails the tag rather than
/// decrypting something plausible.
class FrameSealer {
public:
    /// `frameLimit` is the policy for this connection, not a property of the
    /// class: the default is the figure from §8, and a test can hand in a small
    /// one to reach the exhausted-key path without sealing sixteen million
    /// frames.
    FrameSealer(crypto::Key key, DirectionSalt salt,
                std::uint64_t frameLimit = MAX_FRAMES_PER_EPOCH,
                std::uint64_t byteLimit = MAX_BYTES_PER_EPOCH) noexcept;

    /// Writes header and sealed body into `out` and returns the total length.
    /// `out` needs HEADER_SIZE + plaintext + TAG_SIZE bytes.
    [[nodiscard]] ProtoResult<std::size_t> Seal(MutableByteSpan out,
                                                ByteSpan plaintext) noexcept;

    /// Whether this key has done its share, by either limit from §8. The
    /// time-based trigger belongs to the session, which is where the clock is.
    [[nodiscard]] bool NeedsRekey() const noexcept;

    /// Ratchets forward: epoch++, the next key is derived from the current one,
    /// and the old key is wiped. Forward secrecy within a connection: whoever
    /// takes the current key later cannot read what came before it.
    [[nodiscard]] ProtoResult<void> Rekey() noexcept;

    [[nodiscard]] std::uint64_t Counter() const noexcept { return Counter_; }
    [[nodiscard]] std::uint8_t Epoch() const noexcept { return Epoch_; }
    [[nodiscard]] std::uint64_t BytesSealed() const noexcept {
        return BytesSealed_;
    }

private:
    crypto::Key Key_;
    DirectionSalt Salt_;
    std::uint64_t FrameLimit_;
    std::uint64_t ByteLimit_;
    std::uint64_t Counter_{0};
    std::uint64_t BytesSealed_{0};
    std::uint8_t Epoch_{0};
};

/// Opens frames for one direction of one connection.
class FrameOpener {
public:
    FrameOpener(crypto::Key key, DirectionSalt salt,
                std::uint64_t frameLimit = MAX_FRAMES_PER_EPOCH) noexcept;

    /// Verifies and decrypts the body of `frame` into `out`.
    ///
    /// A frame that fails to open leaves the counter untouched: a rejected
    /// frame must not move the receiver's state, or one injected byte would
    /// desynchronise the stream for good.
    [[nodiscard]] ProtoResult<ByteSpan> Open(MutableByteSpan out,
                                             const FrameView& frame) noexcept;

    [[nodiscard]] std::uint64_t Counter() const noexcept { return Counter_; }
    [[nodiscard]] std::uint8_t Epoch() const noexcept { return Epoch_; }

private:
    /// Derives the next epoch's key and stands it up alongside the current one.
    [[nodiscard]] ProtoResult<void> PrepareNext() noexcept;

    crypto::Key Key_;
    crypto::Key NextKey_;
    DirectionSalt Salt_;
    std::uint64_t FrameLimit_;
    std::uint64_t Counter_{0};
    std::uint64_t NextCounter_{0};
    std::uint8_t Epoch_{0};
    bool NextReady_{false};
};

}  // namespace zet::wire
