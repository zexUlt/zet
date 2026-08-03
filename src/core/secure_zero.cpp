#include "zet/core/secure_zero.hpp"

namespace zet {

void SecureZero(void* data, std::size_t size) noexcept {
    // Writing through a volatile pointer: the standard requires every access
    // to happen, so the loop cannot be discarded as a dead store.
    auto* raw = static_cast<volatile unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        raw[i] = 0;
    }
}

}  // namespace zet
