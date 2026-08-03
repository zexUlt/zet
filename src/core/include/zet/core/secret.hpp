#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace zet {

/// What Secret is allowed to hold, and not as a formality: the wipe clears the
/// object's own bytes, so for anything owning a heap buffer —
/// Secret<std::string> being the obvious mistake — it would clear the pointer
/// and leave the key sitting in freed memory. Key material belongs in a
/// fixed-size array.
template <typename TValue>
concept CSecretPayload = std::is_trivially_copyable_v<TValue>;

/// Holds key material so that logging it is impossible rather than impolite.
///
/// There is no stream operator, no implicit conversion and no way to get at
/// the bytes except through Expose(), which is deliberately ugly and easy to
/// grep for. A reviewer looking for places where a key could leak has a finite
/// list to read.
///
/// The value is wiped when the object dies. From M1 this becomes
/// sodium_memzero; until libsodium arrives the wipe goes through a volatile
/// pointer, which the compiler is not allowed to elide as a dead store.
template <CSecretPayload TValue>
class Secret {
public:
    Secret() noexcept = default;

    explicit Secret(TValue value) noexcept : Value_(std::move(value)) {}

    ~Secret() { Wipe(); }

    // Copying key material silently is exactly the accident this type exists
    // to prevent. Moving is fine: the source is wiped on the way out.
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    Secret(Secret&& other) noexcept : Value_(std::move(other.Value_)) {
        other.Wipe();
    }

    Secret& operator=(Secret&& other) noexcept {
        if (this != &other) {
            Wipe();
            Value_ = std::move(other.Value_);
            other.Wipe();
        }
        return *this;
    }

    /// The one way in. Named so that `grep Expose` finds every use.
    [[nodiscard]] const TValue& Expose() const noexcept { return Value_; }
    [[nodiscard]] TValue& Expose() noexcept { return Value_; }

    void Wipe() noexcept {
        auto* raw = reinterpret_cast<volatile unsigned char*>(&Value_);
        for (std::size_t i = 0; i < sizeof(TValue); ++i) {
            raw[i] = 0;
        }
    }

private:
    TValue Value_{};
};

}  // namespace zet
