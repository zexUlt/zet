#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "zet/core/time.hpp"
#include "zet/session/limits.hpp"

namespace zet::session {

/// What the agent remembers of the client's input for the EchoAck (design.md
/// §9, «Предиктивное эхо»): pairs of the end of the input the pty took and the
/// moment it took it. Sans-I/O: the moments are handed in.
///
/// The pairs are kept in order, both coordinates growing. At most
/// ECHO_HISTORY_MAX of them; when a new one finds the history full, the last
/// pair is replaced by it. The acknowledgement then names the same input
/// later, never earlier: a pair's end counts only once the pair's moment is
/// ECHO_TIMEOUT old, and replacing moves the moment forward.
///
/// Confirmed() is the end of the input that EchoAcks already name, session
/// wide: input is never reset, so a reconnect continues from it.
class EchoHistory {
public:
    /// The pty took input up to `end` at `now`. An `end` that does not go past
    /// the newest pair (or Confirmed) adds nothing.
    void Record(std::uint64_t end, Timestamp now) noexcept;

    /// When the oldest pair past `floor` is ECHO_TIMEOUT old. Unset when no
    /// pair goes past `floor`.
    [[nodiscard]] Deadline NextDeadline(std::uint64_t floor) const noexcept;

    /// The largest end whose moment is at least ECHO_TIMEOUT before `now`;
    /// nullopt if there is none past `floor`.
    [[nodiscard]] std::optional<std::uint64_t> Aged(
        std::uint64_t floor, Timestamp now) const noexcept;

    /// An EchoAck now names input up to `input`: those pairs are spent.
    void Confirm(std::uint64_t input) noexcept;

    [[nodiscard]] std::uint64_t Confirmed() const noexcept {
        return Confirmed_;
    }
    [[nodiscard]] std::size_t Size() const noexcept { return Count_; }

private:
    struct Pair {
        std::uint64_t End{0};
        Timestamp At;
    };

    [[nodiscard]] const Pair& At(std::size_t index) const noexcept {
        return Pairs_[(Head_ + index) % ECHO_HISTORY_MAX];
    }
    [[nodiscard]] Pair& At(std::size_t index) noexcept {
        return Pairs_[(Head_ + index) % ECHO_HISTORY_MAX];
    }

    std::array<Pair, ECHO_HISTORY_MAX> Pairs_{};
    std::size_t Head_{0};
    std::size_t Count_{0};
    std::uint64_t Confirmed_{0};
};

}  // namespace zet::session
