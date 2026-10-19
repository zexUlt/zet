#include "escape.hpp"

namespace zet::client {

namespace {

constexpr std::byte TILDE{'~'};
constexpr std::byte DOT{'.'};
constexpr std::byte CR{'\r'};
constexpr std::byte LF{'\n'};

}  // namespace

EscapeScanner::Result EscapeScanner::Feed(ByteSpan in,
                                          MutableByteSpan out) noexcept {
    Result result;
    const auto put = [&](std::byte b) noexcept {
        if (result.Written < out.size()) {
            out[result.Written++] = b;
        }
    };
    for (const std::byte b : in) {
        if (Held_) {
            Held_ = false;
            if (b == DOT) {
                result.Quit = true;
                return result;
            }
            put(TILDE);
            if (b == TILDE) {
                // ~~ is one ~, and what follows is no longer at a line start.
                AtLineStart_ = false;
                continue;
            }
        } else if (AtLineStart_ && b == TILDE) {
            Held_ = true;
            continue;
        }
        put(b);
        AtLineStart_ = b == CR || b == LF;
    }
    return result;
}

}  // namespace zet::client
