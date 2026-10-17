#include "zet/io/clock.hpp"

#include <ctime>

namespace zet::io {

Timestamp MonotonicNow() noexcept {
    timespec now{};
    // CLOCK_MONOTONIC cannot fail with a valid pointer and a clock id the
    // kernel has had since 2.6, so the result is not checked.
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    constexpr std::int64_t nanosPerSecond = 1'000'000'000;
    return Timestamp::FromNanos(static_cast<std::int64_t>(now.tv_sec) *
                                    nanosPerSecond +
                                static_cast<std::int64_t>(now.tv_nsec));
}

}  // namespace zet::io
