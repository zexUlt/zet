#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/grid/limits.hpp"

namespace zet::grid {

class Grid;

/// VT parser states (Paul Williams' DEC ANSI state machine, design.md §9).
///
/// 14 states matching the published diagram.  kBypass is not an official
/// state name but captures the DCS/OSC/APC/PM/SOS pass-through behaviour:
/// bytes are forwarded to the caller without accumulation until ST is seen.
enum class EParserState : std::uint8_t {
    Ground,
    Escape,
    EscapeIntermediate,
    CsiEntry,
    CsiParam,
    CsiIntermediate,
    CsiIgnore,
    DcsEntry,
    DcsParam,
    DcsIntermediate,
    DcsPassthrough,
    DcsIgnore,
    OscString,
    SosPmApcString,
    /// Bypass: DCS/OSC/APC/PM/SOS content streaming without accumulation.
    /// Exits to Ground on ST (ESC \), BEL (in OSC only), or CAN/SUB/ESC.
    kBypass,
};

/// A single parsed action (what the parser dispatches to the Grid).
/// The full call-dispatch happens inside VtParser::Feed; this struct
/// is exposed for introspection from tests / fuzz targets.
enum class EAction : std::uint8_t {
    None,
    Print,    ///< printable character
    Execute,  ///< C0/C1 control character
    Hook,     ///< DCS hook (enter DCS pass-through)
    Put,      ///< DCS pass-through byte
    Unhook,   ///< DCS unhook (leave DCS pass-through)
    OscStart,
    OscPut,
    OscEnd,
    CsiDispatch,  ///< final byte of a CSI sequence
    EscDispatch,  ///< final byte of an ESC sequence
    Collect,      ///< intermediate byte
    Param,        ///< numeric/sub-param byte
    Clear,        ///< clear params
    Ignore,
};

/// The VT parser (sans-I/O, feeds bytes into a Grid).
///
/// Implements Paul Williams' DEC ANSI state machine with the following
/// additions:
///   - UTF-8 decoding: multibyte sequences are assembled and dispatched
///     as a single Print action with the decoded codepoint.
///   - kBypass: DCS/OSC/APC/PM/SOS content is forwarded without accumulation.
///     The BypassCallback, if set, receives each bypass byte.
///   - UnknownSeqCount: monotonically incremented for unrecognised sequences.
///
/// Usage:
///     VtParser parser;
///     parser.Feed(bytes, length, grid);
class VtParser {
public:
    VtParser() noexcept;

    /// Optional callback invoked for each bypass byte (DCS/OSC/APC/PM/SOS
    /// payload).  Signature: void(std::byte).
    using BypassFn = void (*)(std::byte, void*);
    void SetBypassCallback(BypassFn fn, void* ctx) noexcept {
        BypassFn_ = fn;
        BypassCtx_ = ctx;
    }

    /// Feed raw bytes from the pty into the parser.  Calls Grid mutators
    /// as sequences are recognised.  May be called with any number of bytes
    /// at a time; state persists across calls.
    void Feed(const std::byte* data, std::size_t length, Grid& grid) noexcept;

    /// Feed a single byte (convenience).
    void FeedByte(std::byte b, Grid& grid) noexcept;

    // -----------------------------------------------------------------------
    // State inspection (tests / fuzz invariants)
    // -----------------------------------------------------------------------

    [[nodiscard]] EParserState State() const noexcept { return State_; }

    /// Number of unrecognised sequences since construction / last Reset.
    [[nodiscard]] std::uint64_t UnknownSeqCount() const noexcept {
        return UnknownSeqCount_;
    }

    /// Byte counter in the current bypass sequence.  Resets at each new
    /// DCS/OSC/APC/PM/SOS.  Reaching BYPASS_LIMIT_BYTES forces Ground.
    [[nodiscard]] std::size_t BypassBytes() const noexcept {
        return BypassBytes_;
    }

    /// Hard-reset the parser to Ground state (e.g. on reconnect or
    /// GROUND_WAIT_TIMEOUT).
    void Reset() noexcept;

private:
    // Process one byte already decoded from UTF-8 (codepoint < 0x80) or
    // the full codepoint for multibyte chars.
    void ProcessCodepoint(char32_t cp, Grid& grid) noexcept;

    // Dispatch the current CSI/ESC/DCS sequence to the Grid.
    void DispatchCsi(char32_t finalByte, Grid& grid) noexcept;
    void DispatchEsc(char32_t finalByte, Grid& grid) noexcept;

    // Helpers.
    void TransitionTo(EParserState next, Grid& grid) noexcept;
    void ClearParams() noexcept;
    void AddParam(std::uint8_t ch) noexcept;
    void CollectIntermediate(std::uint8_t ch) noexcept;
    void EmitBypass(std::byte b) noexcept;
    void HandleControl(std::uint8_t c, Grid& grid) noexcept;

    /// Parse collected numeric params into an array of uint16 values.
    /// Returns count. Sub-params (separated by ':') are coalesced into one
    /// slot (only the leading value is kept for sub-params beyond index 0
    /// within the same slot, because the callers that need sub-params —
    /// SGR 38/48/58 — read them explicitly below).
    std::size_t ParseParams(std::uint16_t* out,
                            std::size_t maxOut) const noexcept;

    // -----------------------------------------------------------------------
    // UTF-8 decoder state
    // -----------------------------------------------------------------------
    std::uint32_t Utf8Accumulator_{0};
    std::uint8_t Utf8Remaining_{0};

    // -----------------------------------------------------------------------
    // Parser state
    // -----------------------------------------------------------------------
    EParserState State_{EParserState::Ground};

    // Param/intermediate buffers (bounded per limits.hpp)
    char ParamBuf_[MAX_PARAM_BYTES]{};
    std::size_t ParamLen_{0};

    std::uint8_t Intermediates_[MAX_INTERMEDIATES]{};
    std::size_t IntermediateCount_{0};

    bool PrivateMode_{false};  ///< DEC private sequence (? or >)

    // Bypass state
    bool InOsc_{false};  ///< true = OSC (accepts BEL terminator)
    std::size_t BypassBytes_{0};

    // Callbacks for bypass bytes
    BypassFn BypassFn_{nullptr};
    void* BypassCtx_{nullptr};

    // Statistics
    std::uint64_t UnknownSeqCount_{0};
};

}  // namespace zet::grid
