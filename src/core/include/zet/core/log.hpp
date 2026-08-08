#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "zet/core/time.hpp"

namespace zet::log {

/// There is no Fatal.
///
/// A logging library with a level that ends the process is an invitation to
/// use it as error handling, and EternalTerminal accepted: 164 LOG(FATAL)
/// sites, several reachable from the network, each one taking down every other
/// user's session on that machine. Making the level absent is cheaper than
/// forbidding its use.
enum class ELevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

[[nodiscard]] constexpr std::string_view Describe(ELevel level) noexcept {
    switch (level) {
        case ELevel::Trace:
            return "trace";
        case ELevel::Debug:
            return "debug";
        case ELevel::Info:
            return "info";
        case ELevel::Warn:
            return "warn";
        case ELevel::Error:
            return "error";
    }
    return "?";
}

/// A structured field. Values are borrowed, never owned: emitting a record
/// must not allocate, because the core does not allocate.
class Field {
public:
    enum class EKind : std::uint8_t { Text, Unsigned, Signed };

    [[nodiscard]] static constexpr Field Text(std::string_view key,
                                              std::string_view value) noexcept {
        Field f;
        f.Key_ = key;
        f.Kind_ = EKind::Text;
        f.Text_ = value;
        return f;
    }

    [[nodiscard]] static constexpr Field Unsigned(
        std::string_view key, std::uint64_t value) noexcept {
        Field f;
        f.Key_ = key;
        f.Kind_ = EKind::Unsigned;
        f.Unsigned_ = value;
        return f;
    }

    [[nodiscard]] static constexpr Field Signed(std::string_view key,
                                                std::int64_t value) noexcept {
        Field f;
        f.Key_ = key;
        f.Kind_ = EKind::Signed;
        f.Signed_ = value;
        return f;
    }

    [[nodiscard]] constexpr std::string_view Key() const noexcept {
        return Key_;
    }
    [[nodiscard]] constexpr EKind Kind() const noexcept { return Kind_; }
    [[nodiscard]] constexpr std::string_view AsText() const noexcept {
        return Text_;
    }
    [[nodiscard]] constexpr std::uint64_t AsUnsigned() const noexcept {
        return Unsigned_;
    }
    [[nodiscard]] constexpr std::int64_t AsSigned() const noexcept {
        return Signed_;
    }

private:
    constexpr Field() noexcept = default;

    std::string_view Key_;
    EKind Kind_{EKind::Text};
    std::string_view Text_;
    std::uint64_t Unsigned_{0};
    std::int64_t Signed_{0};
};

struct Record {
    ELevel Level{ELevel::Info};
    std::string_view Message;
    std::span<const Field> Fields;
    Timestamp At;
};

/// Where records go. Rendering to text is the sink's job, not the core's:
/// journald wants fields, a terminal wants a line, and neither belongs in a
/// library that must not perform I/O.
using Sink = void (*)(const Record&, void* context) noexcept;

/// Installs the sink. Until one is installed, records are discarded — a
/// library that writes to stderr because nobody told it otherwise is a
/// library that corrupts someone's stdout one day.
void SetSink(Sink sink, void* context) noexcept;

void SetMinLevel(ELevel level) noexcept;

[[nodiscard]] ELevel MinLevel() noexcept;

/// Cheap enough to guard a call site with, so that building fields for a
/// record nobody wants costs nothing.
[[nodiscard]] bool IsEnabled(ELevel level) noexcept;

void Emit(ELevel level, std::string_view message,
          std::span<const Field> fields = {}, Timestamp at = {}) noexcept;

}  // namespace zet::log
