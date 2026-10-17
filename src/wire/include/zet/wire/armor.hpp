#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"

/// The armor bootstrap frames travel in over ssh, per docs/design.md §4:
///
///     -----BEGIN ZET BOOTSTRAP-----
///     <base64 of one frame>
///     -----END ZET BOOTSTRAP-----
///
/// stdout of a remote shell carries whatever .bashrc, motd and friends print,
/// so a frame is found by its markers, never by its position, and everything
/// between blocks is ignored. The armor proves nothing about what is inside
/// it: the frame still parses as any other, and the Accept still has to echo
/// the client's nonce.
namespace zet::wire {

inline constexpr std::string_view ARMOR_BEGIN = "-----BEGIN ZET BOOTSTRAP-----";
inline constexpr std::string_view ARMOR_END = "-----END ZET BOOTSTRAP-----";

/// The widest frame a block may carry once decoded. The bootstrap messages
/// are about a hundred bytes; the rest is room for a later version, and the
/// ceiling is what keeps a block that never ends from holding more.
inline constexpr std::size_t MAX_ARMORED_FRAME = 512;

/// The base64 text of MAX_ARMORED_FRAME bytes, padding included.
inline constexpr std::size_t MAX_ARMOR_TEXT = (MAX_ARMORED_FRAME + 2) / 3 * 4;

/// The longest line the scanner looks at. A marker and an encoded line both
/// fit; a longer line is noise by definition and is skipped to its end
/// without being held.
inline constexpr std::size_t MAX_ARMOR_LINE = 128;

/// Base64 characters on one line the encoder writes: the PEM width, and below
/// MAX_ARMOR_LINE with room for a stray carriage return.
inline constexpr std::size_t ARMOR_LINE_WIDTH = 64;

/// Room WriteArmor needs for a frame of `frameSize` bytes: a newline that
/// starts the block on a line of its own, both markers, the text broken into
/// lines, and a newline after each line.
[[nodiscard]] constexpr std::size_t ArmoredSize(
    std::size_t frameSize) noexcept {
    const std::size_t text = (frameSize + 2) / 3 * 4;
    const std::size_t lines = (text + ARMOR_LINE_WIDTH - 1) / ARMOR_LINE_WIDTH;
    return 1 + ARMOR_BEGIN.size() + 1 + text + lines + ARMOR_END.size() + 1;
}

/// Standard base64 with padding (RFC 4648 §4). Returns the length written.
[[nodiscard]] ProtoResult<std::size_t> Base64Encode(
    ByteSpan bytes, std::span<char> out) noexcept;

/// The strict inverse: a length that is not a multiple of four, a character
/// outside the alphabet, padding anywhere but the end, or bits set below the
/// last whole byte are all MalformedField. One text, one meaning.
[[nodiscard]] ProtoResult<std::size_t> Base64Decode(
    std::string_view text, MutableByteSpan out) noexcept;

/// Writes `frame` as an armored block. The block starts with a newline, so
/// that it begins a line even after a prompt the shell printed without one.
[[nodiscard]] ProtoResult<std::size_t> WriteArmor(ByteSpan frame,
                                                  std::span<char> out) noexcept;

/// What one call to ArmorScanner::Feed did.
struct ArmorScan {
    /// How much of the chunk was looked at. Less than all of it only when a
    /// block was found: the rest is for the next call.
    std::size_t Consumed{0};
    /// The decoded content of a block that just ended, valid until the next
    /// Feed.
    std::optional<ByteSpan> Block;
};

/// Finds armored blocks in a stream that arrives in chunks of any size, cut
/// anywhere. Memory is fixed: one line and one block, whatever the stream
/// holds — a line longer than MAX_ARMOR_LINE is dropped as it streams past.
///
/// Nothing in the stream is an error. A block that does not decode, outgrows
/// MAX_ARMORED_FRAME or is interrupted is skipped and counted, and the search
/// goes on: the real block may still come after a fake one.
class ArmorScanner {
public:
    [[nodiscard]] ArmorScan Feed(ByteSpan chunk) noexcept;

    /// Blocks that began but did not make it to a decoded end.
    [[nodiscard]] std::uint64_t SkippedBlocks() const noexcept {
        return Skipped_;
    }

private:
    void EndLine() noexcept;
    void Abandon() noexcept;

    std::array<char, MAX_ARMOR_LINE> Line_{};
    std::size_t LineLength_{0};
    bool LineTooLong_{false};
    bool InBlock_{false};
    std::array<char, MAX_ARMOR_TEXT> Text_{};
    std::size_t TextLength_{0};
    std::array<std::byte, MAX_ARMORED_FRAME> Decoded_{};
    std::optional<std::size_t> Ready_;
    std::uint64_t Skipped_{0};
};

}  // namespace zet::wire
