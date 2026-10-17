#include "zet/wire/armor.hpp"

#include <algorithm>

namespace zet::wire {

namespace {

constexpr std::string_view ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char PADDING = '=';

/// Bits per base64 character, and the mask of one character's value.
constexpr unsigned SEXTET_BITS = 6;
constexpr std::uint32_t SEXTET_MASK = 0x3F;
constexpr std::uint32_t BYTE_MASK = 0xFF;
constexpr unsigned BYTE_BITS = 8;

/// Four characters carry three bytes.
constexpr std::size_t QUAD = 4;
constexpr std::size_t TRIPLE = 3;

/// The value of one alphabet character, or nullopt for anything else.
[[nodiscard]] std::optional<std::uint32_t> SextetOf(char c) noexcept {
    const auto at = ALPHABET.find(c);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(at);
}

[[nodiscard]] bool IsArmorText(char c) noexcept {
    return c == PADDING || ALPHABET.find(c) != std::string_view::npos;
}

/// Appends `text` at `at` in `out`; the caller has checked the room.
std::size_t Put(std::span<char> out, std::size_t at,
                std::string_view text) noexcept {
    std::ranges::copy(text, out.begin() + static_cast<std::ptrdiff_t>(at));
    return at + text.size();
}

}  // namespace

ProtoResult<std::size_t> Base64Encode(ByteSpan bytes,
                                      std::span<char> out) noexcept {
    const std::size_t needed = (bytes.size() + 2) / TRIPLE * QUAD;
    if (out.size() < needed) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }
    std::size_t written = 0;
    for (std::size_t i = 0; i < bytes.size(); i += TRIPLE) {
        const std::size_t have = std::min(TRIPLE, bytes.size() - i);
        std::uint32_t group = 0;
        for (std::size_t k = 0; k < TRIPLE; ++k) {
            group <<= BYTE_BITS;
            if (k < have) {
                group |= std::to_integer<std::uint32_t>(bytes[i + k]);
            }
        }
        for (std::size_t k = 0; k < QUAD; ++k) {
            const unsigned shift =
                static_cast<unsigned>(QUAD - 1 - k) * SEXTET_BITS;
            // One character per six bits that carry input; the rest pads.
            out[written + k] =
                k <= have ? ALPHABET[(group >> shift) & SEXTET_MASK] : PADDING;
        }
        written += QUAD;
    }
    return written;
}

ProtoResult<std::size_t> Base64Decode(std::string_view text,
                                      MutableByteSpan out) noexcept {
    if (text.size() % QUAD != 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    std::size_t padding = 0;
    if (!text.empty() && text.back() == PADDING) {
        padding = text.size() >= 2 && text[text.size() - 2] == PADDING ? 2 : 1;
    }
    const std::size_t decoded = text.size() / QUAD * TRIPLE - padding;
    if (out.size() < decoded) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < text.size(); i += QUAD) {
        const bool last = i + QUAD == text.size();
        const std::size_t carried = last ? QUAD - padding : QUAD;
        std::uint32_t group = 0;
        for (std::size_t k = 0; k < QUAD; ++k) {
            group <<= SEXTET_BITS;
            if (k >= carried) {
                continue;
            }
            // Padding in the middle, or a third '=' at the end, lands here.
            const auto sextet = SextetOf(text[i + k]);
            if (!sextet) {
                return std::unexpected(EProtoError::MalformedField);
            }
            group |= *sextet;
        }
        const std::size_t bytes = carried - 1;
        // Bits below the last whole byte must be zero: otherwise two texts
        // would decode to the same bytes, and the armor would not be one
        // spelling of one frame.
        const unsigned unused =
            static_cast<unsigned>(TRIPLE - bytes) * BYTE_BITS;
        if (bytes < TRIPLE && (group & ((1U << unused) - 1U)) != 0) {
            return std::unexpected(EProtoError::MalformedField);
        }
        for (std::size_t k = 0; k < bytes; ++k) {
            const unsigned shift =
                static_cast<unsigned>(TRIPLE - 1 - k) * BYTE_BITS;
            out[written + k] =
                static_cast<std::byte>((group >> shift) & BYTE_MASK);
        }
        written += bytes;
    }
    return written;
}

ProtoResult<std::size_t> WriteArmor(ByteSpan frame,
                                    std::span<char> out) noexcept {
    if (frame.size() > MAX_ARMORED_FRAME) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    if (out.size() < ArmoredSize(frame.size())) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }
    std::array<char, MAX_ARMOR_TEXT> text{};
    auto encoded = Base64Encode(frame, std::span{text});
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const std::string_view body{text.data(), *encoded};

    std::size_t at = Put(out, 0, "\n");
    at = Put(out, at, ARMOR_BEGIN);
    at = Put(out, at, "\n");
    for (std::size_t from = 0; from < body.size(); from += ARMOR_LINE_WIDTH) {
        // Not substr: its out_of_range throw becomes an abort without
        // exceptions, and the no-abort gate counts it as one.
        const std::size_t width =
            std::min(ARMOR_LINE_WIDTH, body.size() - from);
        at = Put(out, at, std::string_view{body.data() + from, width});
        at = Put(out, at, "\n");
    }
    at = Put(out, at, ARMOR_END);
    return Put(out, at, "\n");
}

ArmorScan ArmorScanner::Feed(ByteSpan chunk) noexcept {
    Ready_.reset();
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        const char c = static_cast<char>(chunk[i]);
        if (c == '\n') {
            EndLine();
            if (Ready_) {
                return ArmorScan{.Consumed = i + 1,
                                 .Block = ByteSpan{Decoded_}.first(*Ready_)};
            }
            continue;
        }
        if (LineLength_ < Line_.size()) {
            Line_[LineLength_] = c;
            ++LineLength_;
        } else {
            LineTooLong_ = true;
        }
    }
    return ArmorScan{.Consumed = chunk.size(), .Block = std::nullopt};
}

void ArmorScanner::EndLine() noexcept {
    std::string_view line{Line_.data(), LineLength_};
    // A pty or a Windows-side relay may turn the newline into CRLF.
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    const bool tooLong = LineTooLong_;
    LineLength_ = 0;
    LineTooLong_ = false;

    if (!InBlock_) {
        if (!tooLong && line == ARMOR_BEGIN) {
            InBlock_ = true;
            TextLength_ = 0;
        }
        return;
    }
    if (tooLong) {
        Abandon();
        return;
    }
    if (line == ARMOR_END) {
        InBlock_ = false;
        const auto decoded =
            Base64Decode(std::string_view{Text_.data(), TextLength_},
                         MutableByteSpan{Decoded_});
        if (decoded) {
            Ready_ = *decoded;
        } else {
            ++Skipped_;
        }
        return;
    }
    if (line == ARMOR_BEGIN) {
        // A block that never ended, then a fresh one: the second may be real.
        ++Skipped_;
        TextLength_ = 0;
        return;
    }
    if (line.empty() || !std::ranges::all_of(line, IsArmorText) ||
        line.size() > Text_.size() - TextLength_) {
        Abandon();
        return;
    }
    std::ranges::copy(line,
                      Text_.begin() + static_cast<std::ptrdiff_t>(TextLength_));
    TextLength_ += line.size();
}

void ArmorScanner::Abandon() noexcept {
    InBlock_ = false;
    TextLength_ = 0;
    ++Skipped_;
}

}  // namespace zet::wire
