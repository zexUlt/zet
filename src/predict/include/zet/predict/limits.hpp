#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "zet/core/time.hpp"

namespace zet::predict {

/// Predictions held at once (design.md §9, "Пределы"). Past it the epoch is
/// raised and nothing new is predicted until echo-acks free room.
inline constexpr std::size_t PREDICTIONS_MAX = 256;

/// EchoAcks waiting for the mirror to catch up with their output offset. When
/// the queue is full the oldest is dropped: the next one covers at least as
/// much input, so confirmation arrives later, never earlier.
inline constexpr std::size_t ECHO_ACK_PENDING_MAX = 16;

// Display thresholds, mosh's (design.md §9, "Когда показывать"). They are
// compared with SRTT / 2, which the engine does as "SRTT > 2 * threshold".

/// SRTT / 2 above this: show predictions.
inline constexpr Duration PREDICT_SHOW_HIGH = std::chrono::milliseconds(30);
/// SRTT / 2 at or below this, and nothing drawn: stop showing.
inline constexpr Duration PREDICT_SHOW_LOW = std::chrono::milliseconds(20);
/// SRTT / 2 above this: underline predictions.
inline constexpr Duration PREDICT_FLAG_HIGH = std::chrono::milliseconds(80);
/// SRTT / 2 at or below this: stop underlining.
inline constexpr Duration PREDICT_FLAG_LOW = std::chrono::milliseconds(50);

/// A prediction unconfirmed for this long shows predictions even at low SRTT.
inline constexpr Duration GLITCH_THRESHOLD = std::chrono::milliseconds(250);
/// Unconfirmed for this long: show and underline.
inline constexpr Duration GLITCH_FLAG_THRESHOLD = std::chrono::seconds(5);
/// Quick confirmations needed to clear a glitch; more than this many glitches
/// underline.
inline constexpr std::uint32_t GLITCH_REPAIR_COUNT = 10;
/// Minimum spacing between quick confirmations that count towards a repair.
inline constexpr Duration GLITCH_REPAIR_MININTERVAL =
    std::chrono::milliseconds(150);

}  // namespace zet::predict
