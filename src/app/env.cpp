#include "zet/app/env.hpp"

#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <system_error>

namespace zet::app {

namespace {

void Complain(const char* name, std::string_view value,
              std::string_view middle = ", not a whole number: ") noexcept {
    constexpr std::string_view prefix = "zet: ignoring ";
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

std::optional<std::size_t> SizeFromEnv(const char* name) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread.
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{raw};
    std::size_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0) {
        Complain(name, text, ", not a positive whole number: ");
        return std::nullopt;
    }
    return value;
}

std::optional<std::string_view> ChoiceFromEnv(
    const char* name, std::span<const std::string_view> allowed) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread.
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{raw};
    for (const auto choice : allowed) {
        if (text == choice) {
            return choice;
        }
    }
    Complain(name, text, ", not one of the allowed values: ");
    return std::nullopt;
}

std::optional<std::string_view> StringFromEnv(const char* name) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread.
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    return std::string_view{raw};
}

}  // namespace zet::app
