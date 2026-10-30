#pragma once

#include <cstddef>
#include <cstdint>

namespace zet::grid {

/// Maximum terminal width (columns).
inline constexpr std::uint16_t MAX_COLS = 4096;

/// Maximum terminal height (rows).
inline constexpr std::uint16_t MAX_ROWS = 1024;

/// Default scrollback ring size in lines (design.md §9).
inline constexpr std::uint32_t DEFAULT_SCROLLBACK_LIMIT = 1000;

/// Maximum number of extended-color side-table entries per Grid.
///
/// An entry holds one truecolor (R, G, B) tuple for a cell's fg or bg.
/// 65536 entries × 3 bytes = 192 KiB per arena, well under budget.
/// When the pool is exhausted the renderer degrades truecolor to 256-color
/// for new cells (design.md §9, degrade ladder step 2).
inline constexpr std::uint16_t MAX_EXT_CELLS = 65535;

/// UTF-8 decode buffer capacity. A codepoint is at most 4 bytes.
inline constexpr std::size_t MAX_UTF8_BYTES = 4;

/// Maximum bytes in a CSI/OSC/etc. parameter string we accumulate.
/// DCS/OSC/APC are bypassed (kBypass) so only CSI params land here.
inline constexpr std::size_t MAX_PARAM_BYTES = 256;

/// Maximum number of CSI numeric sub-parameters parsed from one sequence.
inline constexpr std::size_t MAX_PARAMS = 16;

/// Maximum intermediate bytes in a CSI/Escape sequence.
inline constexpr std::size_t MAX_INTERMEDIATES = 2;

/// Force-reset to Ground after this many bytes without reaching Ground
/// (design.md §9, GROUND_WAIT_TIMEOUT measured in bytes, not time; the
/// time-based reset is the caller's responsibility via a separate counter).
/// 1 MiB matches the design spec.
inline constexpr std::size_t BYPASS_LIMIT_BYTES = 1U * 1024U * 1024U;

/// Tab stop interval (default every 8 columns).
inline constexpr std::uint16_t DEFAULT_TAB_INTERVAL = 8;

}  // namespace zet::grid
