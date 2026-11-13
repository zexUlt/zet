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

// Cursor position calibration (design.md §9, "Калибровка зеркала запросом
// позиции курсора", "Пределы").

/// How long the client waits for the answer to its own `CSI 6n CSI c` before
/// the terminal is declared silent.
inline constexpr Duration CPR_TIMEOUT = std::chrono::seconds(1);
/// How long a possible beginning of the client's own answer is held back from
/// the agent while the query is pending. A terminal writes the position and
/// the DA1 reply in one piece or in two writes close together, so a held
/// beginning is rare. A complete position is held longer, to the live
/// entry's CPR_TIMEOUT, for a DA1 reply that comes late.
inline constexpr Duration CPR_HOLD = std::chrono::milliseconds(50);
/// After CPR_TIMEOUT the client's own entry is kept this long, so that a late
/// answer is still stripped; then it is dropped and stdin passes through
/// untouched. The same span bounds how long a count of the program's DA1
/// queries is believed (one query is dropped per span for which the count has
/// not been zero), so that a reply lost on the way cannot block the alignment
/// for good.
inline constexpr Duration CPR_FORGET = std::chrono::seconds(10);

/// Output held back while the mirror waits at the point of the client's own
/// query (design.md §9, "Заморозка"). Past it there is no calibration: the
/// wait ends, the held output goes into the mirror, and the query's answer is
/// stripped and thrown away. The buffer is allocated when the wait begins and
/// freed when it ends.
inline constexpr std::size_t CPR_LAG_BYTES_MAX = 1024 * 1024;

/// Answers that settle nothing about a class of glyph widths (the column
/// difference is neither 0 nor the number of glyphs, or other glyphs of open
/// width share the path) before the class is Unsolved for the session
/// (design.md §9, "Ширина").
inline constexpr std::uint8_t CPR_SOLVE_FAILS_MAX = 3;

/// Longest answer the demultiplexer recognises: `CSI r ; c R` with five-digit
/// numbers is 14 bytes. A device attributes answer is `CSI ? Pm c`: xterm's
/// default vt420 reply is 38 bytes and Windows Terminal's is 39, so the limit
/// is 128, more than three times the longest seen, for terminals that report
/// more features. Beyond it the bytes are not an answer. Position and the
/// answer after it can be held together; the buffer is a fixed array, never
/// allocated.
inline constexpr std::size_t CPR_POSITION_REPLY_MAX = 14;
inline constexpr std::size_t CPR_DA1_REPLY_MAX = 128;
inline constexpr std::size_t CPR_HELD_MAX =
    CPR_POSITION_REPLY_MAX + CPR_DA1_REPLY_MAX;

}  // namespace zet::predict
