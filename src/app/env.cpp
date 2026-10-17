#include "zet/app/env.hpp"

#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace zet::app {

namespace {

void Complain(const char* name, std::string_view value) noexcept {
    constexpr std::string_view prefix = "zet: ignoring ";
    constexpr std::string_view middle = ", not a whole number: ";
    // Before the loop starts; one short write each, results not checked
    // because there is nowhere else to report a failing stderr.
    static_cast<void>(::write(STDERR_FILENO, prefix.data(), prefix.size()));
    static_cast<void>(::write(STDERR_FILENO, name, std::strlen(name)));
    static_cast<void>(::write(STDERR_FILENO, middle.data(), middle.size()));
    static_cast<void>(::write(STDERR_FILENO, value.data(), value.size()));
    static_cast<void>(::write(STDERR_FILENO, "\n", 1));
}

}  // namespace

std::optional<Duration> DurationFromEnv(const char* name, EUnit unit) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread.
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{raw};
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        Complain(name, text);
        return std::nullopt;
    }
    const auto count = static_cast<std::int64_t>(value);
    if (unit == EUnit::Seconds) {
        return std::chrono::duration_cast<Duration>(
            std::chrono::seconds{count});
    }
    return std::chrono::duration_cast<Duration>(
        std::chrono::milliseconds{count});
}

}  // namespace zet::app
