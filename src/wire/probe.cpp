#include "zet/wire/probe.hpp"

namespace zet::wire {

namespace {

constexpr std::string_view PROBE_MARKER = "ZET-PROBE";
constexpr std::string_view HAVE_MARKER = "ZET-HAVE";
constexpr std::string_view SEND_MARKER = "ZET-SEND";
constexpr std::string_view STORED_MARKER = "ZET-STORED";
constexpr std::string_view FAILED_MARKER = "ZET-FAILED ";

/// Supported probe version in this implementation.
constexpr std::string_view PROBE_VERSION_1 = "1";

/// Whether `line` starts with `prefix`.
[[nodiscard]] bool StartsWith(std::string_view line,
                              std::string_view prefix) noexcept {
    return line.size() >= prefix.size() &&
           line.substr(0, prefix.size()) == prefix;
}

/// The next space-delimited token starting at `pos` in `line`; advances `pos`
/// past the token and any trailing space. Returns empty when none remains.
[[nodiscard]] std::string_view Next(std::string_view line,
                                    std::size_t& pos) noexcept {
    // Use .data()[pos] to avoid std::string_view::operator[]'s libc++ hardening
    // bounds check, which would pull __libcpp_verbose_abort into wire objects.
    while (pos < line.size() && line.data()[pos] == ' ') {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < line.size() && line.data()[pos] != ' ') {
        ++pos;
    }
    // Use the pointer+length constructor instead of substr: substr's
    // pos<=size() assertion (_LIBCPP_ASSERT_VALID_ELEMENT_ACCESS, enabled in
    // _LIBCPP_HARDENING_MODE_EXTENSIVE) references __libcpp_verbose_abort.
    // The assertion is not eliminable at -O0 because `start <= line.size()`
    // requires range inference from the loop bound that the optimiser skips
    // in non-Release builds.
    return {line.data() + start, pos - start};
}

/// Parses `ZET-PROBE 1 <arch> exec|noexec` into `out`.  Returns true on
/// success.  Uses an output parameter instead of std::optional to avoid
/// optional::operator* / operator->, which call __libcpp_verbose_abort in
/// libc++ hardened mode (banned in wire objects by check_no_abort.sh).
[[nodiscard]] bool ParseProbe(std::string_view line, ProbeInfo& out) noexcept {
    if (!StartsWith(line, PROBE_MARKER)) {
        return false;
    }
    // Skip "ZET-PROBE"
    std::size_t pos = PROBE_MARKER.size();
    const auto version = Next(line, pos);
    if (version != PROBE_VERSION_1) {
        return false;
    }
    const auto arch = Next(line, pos);
    if (arch.empty()) {
        return false;
    }
    const auto exec = Next(line, pos);
    if (exec.empty()) {
        return false;
    }
    const bool noexec = exec == "noexec";
    if (!noexec && exec != "exec") {
        return false;
    }
    out = ProbeInfo{.Arch = arch, .NoExec = noexec};
    return true;
}

}  // namespace

ProbeScan ProbeScanner::Feed(ByteSpan chunk) noexcept {
    ReadyTag_ = ReadyTag::None;
    // Use .data()[i] instead of operator[] to avoid the libc++ hardening check
    // that would introduce a __libcpp_verbose_abort reference (banned in wire).
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        const char c = static_cast<char>(chunk.data()[i]);
        if (c == '\n') {
            EndLine();
            // Build ProbeScan directly from bare payload types — not from
            // the ReadyTag_ field via optional copy/move — so that
            // optional(U&&) (the single-arg converting constructor) is used
            // rather than optional(optional&&), which would route through
            // operator* and reference __libcpp_verbose_abort in hardened mode.
            const std::size_t consumed = i + 1;
            switch (ReadyTag_) {
                case ReadyTag::Info:
                    ReadyTag_ = ReadyTag::None;
                    return ProbeScan{
                        .Consumed = consumed,
                        .Answer = ProbeInfo{ReadyArch_, ReadyNoExec_}};
                case ReadyTag::Have:
                    ReadyTag_ = ReadyTag::None;
                    return ProbeScan{.Consumed = consumed,
                                     .Answer = ProbeHave{}};
                case ReadyTag::Send:
                    ReadyTag_ = ReadyTag::None;
                    return ProbeScan{.Consumed = consumed,
                                     .Answer = ProbeSend{}};
                case ReadyTag::Stored:
                    ReadyTag_ = ReadyTag::None;
                    return ProbeScan{.Consumed = consumed,
                                     .Answer = ProbeStored{}};
                case ReadyTag::Failed:
                    ReadyTag_ = ReadyTag::None;
                    return ProbeScan{.Consumed = consumed,
                                     .Answer = ProbeFailed{ReadyReason_}};
                case ReadyTag::None:
                    break;
            }
            continue;
        }
        if (LineLength_ < Line_.size()) {
            Line_.data()[LineLength_] = c;
            ++LineLength_;
        } else {
            LineTooLong_ = true;
        }
    }
    return ProbeScan{.Consumed = chunk.size(), .Answer = std::nullopt};
}

void ProbeScanner::EndLine() noexcept {
    std::string_view line{Line_.data(), LineLength_};
    // A pty may turn \n into \r\n.
    // Use .data()[size()-1] instead of .back() for the same reason.
    if (!line.empty() && line.data()[line.size() - 1] == '\r') {
        line.remove_suffix(1);
    }
    const bool tooLong = LineTooLong_;
    LineLength_ = 0;
    LineTooLong_ = false;

    // A line that didn't start with ZET- is noise; skip silently.
    if (!StartsWith(line, "ZET-")) {
        return;
    }

    // Line begins ZET- but was cut: not a valid marker.
    if (tooLong) {
        ++Skipped_;
        return;
    }

    ProbeInfo probeInfo{};
    if (ParseProbe(line, probeInfo)) {
        // Arch string_view points into Line_; valid until next Feed.
        ReadyTag_ = ReadyTag::Info;
        ReadyArch_ = probeInfo.Arch;
        ReadyNoExec_ = probeInfo.NoExec;
        return;
    }

    if (line == HAVE_MARKER) {
        ReadyTag_ = ReadyTag::Have;
        return;
    }
    if (line == SEND_MARKER) {
        ReadyTag_ = ReadyTag::Send;
        return;
    }
    if (line == STORED_MARKER) {
        ReadyTag_ = ReadyTag::Stored;
        return;
    }
    if (StartsWith(line, FAILED_MARKER)) {
        // Reason string_view points into Line_; valid until next Feed.
        ReadyTag_ = ReadyTag::Failed;
        // Pointer+length, not substr: substr's pos<=size() assertion
        // (_LIBCPP_ASSERT_VALID_ELEMENT_ACCESS in EXTENSIVE mode) is not
        // provably dead at -O0 because the StartsWith guard is a separate
        // non-inlined function, so the compiler cannot see that
        // FAILED_MARKER.size() <= line.size() here.
        ReadyReason_ = {line.data() + FAILED_MARKER.size(),
                        line.size() - FAILED_MARKER.size()};
        return;
    }

    // ZET- line that we don't recognise.
    ++Skipped_;
}

}  // namespace zet::wire
