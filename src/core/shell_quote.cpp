#include "zet/core/shell_quote.hpp"

#include <algorithm>

namespace zet {

ProtoResult<std::size_t> ShellQuote(std::string_view text,
                                    std::span<char> out) noexcept {
    if (text.find('\0') != std::string_view::npos) {
        return std::unexpected(EProtoError::MalformedField);
    }
    const std::size_t size = ShellQuotedSize(text);
    if (out.size() < size) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }
    constexpr std::string_view escapedQuote = "'\\''";
    std::size_t at = 0;
    out[at++] = '\'';
    for (const char c : text) {
        if (c == '\'') {
            std::ranges::copy(escapedQuote,
                              out.begin() + static_cast<std::ptrdiff_t>(at));
            at += escapedQuote.size();
        } else {
            out[at++] = c;
        }
    }
    out[at++] = '\'';
    return at;
}

}  // namespace zet
