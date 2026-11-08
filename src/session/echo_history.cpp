#include "zet/session/echo_history.hpp"

namespace zet::session {

void EchoHistory::Record(std::uint64_t end, Timestamp now) noexcept {
    if (end <= Confirmed_ || (Count_ > 0 && end <= At(Count_ - 1).End)) {
        return;
    }
    if (Count_ == ECHO_HISTORY_MAX) {
        At(Count_ - 1) = Pair{.End = end, .At = now};
        return;
    }
    At(Count_) = Pair{.End = end, .At = now};
    ++Count_;
}

Deadline EchoHistory::NextDeadline(std::uint64_t floor) const noexcept {
    for (std::size_t i = 0; i < Count_; ++i) {
        if (At(i).End > floor) {
            return Deadline{At(i).At + ECHO_TIMEOUT};
        }
    }
    return Deadline{};
}

std::optional<std::uint64_t> EchoHistory::Aged(std::uint64_t floor,
                                               Timestamp now) const noexcept {
    std::optional<std::uint64_t> aged;
    for (std::size_t i = 0; i < Count_; ++i) {
        if (At(i).At + ECHO_TIMEOUT > now) {
            break;
        }
        if (At(i).End > floor) {
            aged = At(i).End;
        }
    }
    return aged;
}

void EchoHistory::Confirm(std::uint64_t input) noexcept {
    if (input <= Confirmed_) {
        return;
    }
    Confirmed_ = input;
    while (Count_ > 0 && At(0).End <= Confirmed_) {
        Head_ = (Head_ + 1) % ECHO_HISTORY_MAX;
        --Count_;
    }
}

}  // namespace zet::session
