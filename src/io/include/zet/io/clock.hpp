#pragma once

#include "zet/core/time.hpp"

namespace zet::io {

/// The monotonic clock: the one place in zet that asks the system what time
/// it is (zet/core/time.hpp). Everything below zet_io is handed the result.
[[nodiscard]] Timestamp MonotonicNow() noexcept;

}  // namespace zet::io
