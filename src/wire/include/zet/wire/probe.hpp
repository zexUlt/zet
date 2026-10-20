#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "zet/core/bytes.hpp"

/// The probe exchange that precedes bootstrap in delivery mode (design.md §4,
/// "Доставка агента (M5.5)").
///
/// The installer script prints lines with markers at the start; everything
/// else is noise from .bashrc and is skipped, exactly as in bootstrap armor.
/// A line must begin with the marker to be recognised — a noisy .bashrc that
/// happens to contain "ZET-HAVE" in a banner will not be mistaken for one.
///
///     installer → stdout   ZET-PROBE 1 <arch> exec|noexec
///     client    → stdin    <hash> <size>
///     installer → stdout   ZET-HAVE  (binary already present)
///                          ZET-SEND  (binary wanted)
///     client    → stdin    <size> raw bytes   (after ZET-SEND only)
///     installer → stdout   ZET-STORED
///                          ZET-FAILED <reason>
namespace zet::wire {

/// Recognised installer answers.
struct ProbeInfo {
    /// Architecture string from `uname -m`.
    /// Valid only while the ProbeScanner that produced it is alive and has
    /// not received another Feed call.
    std::string_view Arch;
    bool NoExec{false};
};

struct ProbeHave {};
struct ProbeSend {};
struct ProbeStored {};
struct ProbeFailed {
    /// Reason text. Valid only while the ProbeScanner that produced it is
    /// alive and has not received another Feed call.
    std::string_view Reason;
};

using ProbeAnswer =
    std::variant<ProbeInfo, ProbeHave, ProbeSend, ProbeStored, ProbeFailed>;

/// What one Feed call produced.
struct ProbeScan {
    /// How many bytes of the chunk were consumed. Less than all only when an
    /// answer was found: the remainder is for the next Feed.
    std::size_t Consumed{0};
    /// The answer, if one was found in this chunk; valid until the next Feed.
    std::optional<ProbeAnswer> Answer;
};

/// The longest line the scanner will hold. Shorter than MAX_ARMOR_LINE but
/// enough for any valid installer marker plus an arch and a flag.
inline constexpr std::size_t MAX_PROBE_LINE = 128;

/// Finds installer-marker lines in a stream that arrives in any-size chunks.
/// Memory is fixed: one line. A line longer than MAX_PROBE_LINE is dropped as
/// it streams past; a line that starts with `ZET-` but is not a known marker
/// is ignored and the scan continues.
///
/// Nothing in the stream is an error: the scanner produces one Answer per
/// recognised line and ignores everything else. The caller decides whether an
/// unexpected sequence (e.g. ZET-FAILED before ZET-PROBE) is fatal.
class ProbeScanner {
public:
    [[nodiscard]] ProbeScan Feed(ByteSpan chunk) noexcept;

    /// How many lines starting with `ZET-` were not recognised.
    [[nodiscard]] std::uint64_t SkippedMarkers() const noexcept {
        return Skipped_;
    }

private:
    void EndLine() noexcept;

    std::array<char, MAX_PROBE_LINE> Line_{};
    std::size_t LineLength_{0};
    bool LineTooLong_{false};

    // Pending answer from the most recent EndLine call.  A plain tag +
    // payload instead of std::optional<ProbeAnswer> because optional's
    // copy/move constructors and operator= route through operator*, which in
    // libc++ hardened mode (_LIBCPP_HARDENING_MODE_EXTENSIVE) unconditionally
    // references __libcpp_verbose_abort — banned in wire objects by
    // check_no_abort.sh.
    enum class EReadyTag : std::uint8_t {
        None,
        Info,
        Have,
        Send,
        Stored,
        Failed,
    };
    EReadyTag ReadyTag_{EReadyTag::None};
    std::string_view ReadyArch_{};
    bool ReadyNoExec_{false};
    std::string_view ReadyReason_{};

    std::uint64_t Skipped_{0};
};

}  // namespace zet::wire
