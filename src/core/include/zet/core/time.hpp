#pragma once

#include <chrono>
#include <compare>
#include <cstdint>

namespace zet {

using Duration = std::chrono::nanoseconds;

/// A point on a monotonic clock.
///
/// Reading the clock is a syscall, so it does not happen here: the protocol
/// core is a pure function of incoming bytes, events and *time passed in*.
/// zet_io owns the only place that asks the operating system what time it is.
/// That is what lets a reconnect test run a session across an hour of virtual
/// time in a millisecond, deterministically.
///
/// The epoch is arbitrary and comparisons only make sense between timestamps
/// from the same source, which is why there is no conversion to wall time.
class Timestamp {
public:
    constexpr Timestamp() noexcept = default;

    static constexpr Timestamp FromNanos(std::int64_t nanos) noexcept {
        return Timestamp{Duration{nanos}};
    }

    explicit constexpr Timestamp(Duration sinceEpoch) noexcept
        : sinceEpoch_(sinceEpoch) {}

    [[nodiscard]] constexpr Duration SinceEpoch() const noexcept {
        return sinceEpoch_;
    }

    constexpr Timestamp& operator+=(Duration d) noexcept {
        sinceEpoch_ += d;
        return *this;
    }
    constexpr Timestamp& operator-=(Duration d) noexcept {
        sinceEpoch_ -= d;
        return *this;
    }

    [[nodiscard]] friend constexpr Timestamp operator+(Timestamp t,
                                                       Duration d) noexcept {
        return Timestamp{t.sinceEpoch_ + d};
    }
    [[nodiscard]] friend constexpr Timestamp operator-(Timestamp t,
                                                       Duration d) noexcept {
        return Timestamp{t.sinceEpoch_ - d};
    }

    /// How much later `a` is than `b`. Negative if it is earlier.
    [[nodiscard]] friend constexpr Duration operator-(Timestamp a,
                                                      Timestamp b) noexcept {
        return a.sinceEpoch_ - b.sinceEpoch_;
    }

    [[nodiscard]] friend constexpr auto operator<=>(
        Timestamp, Timestamp) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(
        Timestamp, Timestamp) noexcept = default;

private:
    Duration sinceEpoch_{};
};

/// A deadline that has not been set yet.
///
/// Timers compare against this rather than against a sentinel numeric value,
/// so "no deadline" cannot be confused with "a deadline far away" — the timer
/// queue asks whether a deadline exists before asking when it is.
class Deadline {
public:
    constexpr Deadline() noexcept = default;

    explicit constexpr Deadline(Timestamp at) noexcept : at_(at), set_(true) {}

    [[nodiscard]] constexpr bool IsSet() const noexcept { return set_; }

    [[nodiscard]] constexpr Timestamp At() const noexcept { return at_; }

    [[nodiscard]] constexpr bool HasExpired(Timestamp now) const noexcept {
        return set_ && now >= at_;
    }

    /// The earlier of two deadlines, treating "not set" as infinitely far.
    [[nodiscard]] friend constexpr Deadline Earliest(Deadline a,
                                                     Deadline b) noexcept {
        if (!a.set_) {
            return b;
        }
        if (!b.set_) {
            return a;
        }
        return a.at_ <= b.at_ ? a : b;
    }

private:
    Timestamp at_{};
    bool set_{false};
};

}  // namespace zet
