#include "zet/wire/probe.hpp"

#include <algorithm>

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
    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < line.size() && line[pos] != ' ') {
        ++pos;
    }
    return line.substr(start, pos - start);
}

/// Parses `ZET-PROBE 1 <arch> exec|noexec`.
[[nodiscard]] std::optional<ProbeInfo> ParseProbe(
    std::string_view line) noexcept {
    if (!StartsWith(line, PROBE_MARKER)) {
        return std::nullopt;
    }
    // Skip "ZET-PROBE"
    std::size_t pos = PROBE_MARKER.size();
    const auto version = Next(line, pos);
    if (version != PROBE_VERSION_1) {
        return std::nullopt;
    }
    const auto arch = Next(line, pos);
    if (arch.empty()) {
        return std::nullopt;
    }
    const auto exec = Next(line, pos);
    if (exec.empty()) {
        return std::nullopt;
    }
    const bool noexec = exec == "noexec";
    if (!noexec && exec != "exec") {
        return std::nullopt;
    }
    return ProbeInfo{.Arch = arch, .NoExec = noexec};
}

}  // namespace

ProbeScan ProbeScanner::Feed(ByteSpan chunk) noexcept {
    Ready_.reset();
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        const char c = static_cast<char>(chunk[i]);
        if (c == '\n') {
            EndLine();
            if (Ready_) {
                return ProbeScan{.Consumed = i + 1,
                                 .Answer = std::move(Ready_)};
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
    if (!line.empty() && line.back() == '\r') {
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

    if (auto probe = ParseProbe(line)) {
        // The Arch string_view points into Line_; valid until next Feed.
        Ready_ = ProbeAnswer{*probe};
        return;
    }

    if (line == HAVE_MARKER) {
        Ready_ = ProbeAnswer{ProbeHave{}};
        return;
    }
    if (line == SEND_MARKER) {
        Ready_ = ProbeAnswer{ProbeSend{}};
        return;
    }
    if (line == STORED_MARKER) {
        Ready_ = ProbeAnswer{ProbeStored{}};
        return;
    }
    if (StartsWith(line, FAILED_MARKER)) {
        Ready_ = ProbeAnswer{ProbeFailed{line.substr(FAILED_MARKER.size())}};
        return;
    }

    // ZET- line that we don't recognise.
    ++Skipped_;
}

}  // namespace zet::wire
