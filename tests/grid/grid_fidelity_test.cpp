// xterm fidelity of DECSC/DECRC, SGR 21 and the buffer-switch modes.
//
// Spec sources, all from the xterm tree (ThomasDickey/xterm-snapshots):
//   ctlseqs.ms  "Ps = 1048 -> Save cursor as in DECSC" / "Restore cursor as in
//               DECRC"; "Ps = 21 -> Doubly-underlined, ECMA-48 3rd";
//               "Ps = 24 -> Not underlined".
//   cursor.c    CursorSave2 saves row/col, SGR flags, curgl/curgr, wrap_flag,
//               the G0-G3 sets; DECSC_FLAGS = ATTRIBUTES|ORIGIN|PROTECTED, so
//               DECOM is saved too.  CursorRestoreFlags restores the sets
//               (resetCharsets when nothing was saved), the flags, places the
//               cursor under the restored origin mode, then sets
//               do_wrap = sc->wrap_flag "after CursorSet/ResetWrap".  The
//               DECAWM flag itself is not part of the state.
//   charproc.c  case 21: UIntSet(flags, ATR_DBL_UNDER); case 24 clears
//               UNDERLINE and ATR_DBL_UNDER.
//
// The grid models G0/G1 and SO/SI only (no G2/G3, no GR, no single shifts)
// and has no selective-erase attribute, so those parts of the xterm state do
// not exist here.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

Grid* Mk(int cols, int rows) {
    auto g = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows), 10);
    REQUIRE(g.has_value());
    return g.value();
}

void Feed(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

char32_t CP(const Grid& g, int row, int col) {
    return g
        .CellAt(static_cast<std::uint16_t>(row),
                static_cast<std::uint16_t>(col))
        .Codepoint;
}

// ESC 7 / ESC 8 spelled so the adjacent digit cannot read as an octal escape.
constexpr std::string_view DECSC =
    "\x1b"
    "7";
constexpr std::string_view DECRC =
    "\x1b"
    "8";

}  // namespace

// ---------------------------------------------------------------------------
// DECSC / DECRC state
// ---------------------------------------------------------------------------

TEST_CASE("DECRC restores the origin mode saved by DECSC") {
    // Mutant: DECSC/DECRC ignore DECOM (the mode stays as it was at DECRC).
    Grid* g = Mk(10, 8);
    VtParser p;
    Feed(p, *g, "\x1b[3;5r\x1b[?6h\x1b[2;4H");  // DECOM on, region rows 3..5
    Feed(p, *g, DECSC);
    CHECK(g->CursorRow() == 3);  // region row 2 = absolute row 3 (0-based)
    Feed(p, *g, "\x1b[?6l");     // origin mode off, cursor homed
    REQUIRE(!g->ModeSet(EMode::Decom));
    Feed(p, *g, DECRC);
    CHECK(g->ModeSet(EMode::Decom));
    CHECK(g->CursorRow() == 3);
    CHECK(g->CursorCol() == 3);
    delete g;
}

TEST_CASE("DECRC clears the origin mode when it was off at DECSC") {
    // Mutant: DECRC leaves the current DECOM in place.
    Grid* g = Mk(10, 8);
    VtParser p;
    Feed(p, *g, "\x1b[3;3H");
    Feed(p, *g, DECSC);  // DECOM off
    Feed(p, *g, "\x1b[3;5r\x1b[?6h");
    REQUIRE(g->ModeSet(EMode::Decom));
    Feed(p, *g, DECRC);
    CHECK(!g->ModeSet(EMode::Decom));
    CHECK(g->CursorRow() ==
          2);  // placed under the restored (off) mode: no clamp
    delete g;
}

TEST_CASE("DECRC under a saved origin mode clamps at the bottom margin only") {
    // xterm cursor.c CursorSet: with ORIGIN, use_row += top_marg and only
    // "use_row <= max_row" (max_row = bot_marg) bounds it; the top is bounded
    // by 0 alone.  So a saved row above the region stays, one below clamps.
    // Mutants: the clamp looks at the DECOM in force at DECRC instead of the
    // restored one; a top clamp is added.
    Grid* g = Mk(10, 8);
    VtParser p;
    Feed(p, *g, "\x1b[?6h");  // DECOM, full-screen region, cursor at row 0
    Feed(p, *g, DECSC);
    Feed(p, *g, "\x1b[?6l\x1b[4;6r");  // DECOM off, region rows 4..6
    Feed(p, *g, DECRC);
    CHECK(g->ModeSet(EMode::Decom));
    CHECK(g->CursorRow() == 0);  // above the region: no top clamp

    Feed(p, *g, "\x1b[?6l\x1b[r\x1b[?6h\x1b[8;1H");  // DECOM, row 8
    Feed(p, *g, DECSC);
    REQUIRE(g->CursorRow() == 7);
    Feed(p, *g, "\x1b[?6l\x1b[2;5r");  // region rows 2..5
    Feed(p, *g, DECRC);
    CHECK(g->ModeSet(EMode::Decom));
    CHECK(g->CursorRow() == 4);  // clamped to the bottom margin
    delete g;
}

TEST_CASE("DECRC restores the pending-wrap flag") {
    // Mutant: DECRC always clears the wrap flag.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2;10Hx");  // last column: wrap pending
    REQUIRE(g->PendingWrap());
    Feed(p, *g, DECSC);
    Feed(p, *g, "\x1b[4;4H");
    REQUIRE(!g->PendingWrap());
    Feed(p, *g, DECRC);
    CHECK(g->CursorRow() == 1);
    CHECK(g->CursorCol() == 9);
    CHECK(g->PendingWrap());
    Feed(p, *g, "y");  // wraps to the next line
    CHECK(CP(*g, 2, 0) == U'y');
    delete g;
}

TEST_CASE("DECRC restores the wrap flag only in the last column") {
    // Mutant: the wrap flag is restored unconditionally.  A widening Resize
    // after the save leaves the saved column short of the new last column.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2;10Hx");
    REQUIRE(g->PendingWrap());
    Feed(p, *g, DECSC);
    REQUIRE(g->Resize(20, 5).has_value());
    Feed(p, *g, DECRC);
    CHECK(g->CursorCol() == 9);
    CHECK(!g->PendingWrap());
    Feed(p, *g, "y");  // prints in place, no wrap
    CHECK(CP(*g, 1, 9) == U'y');
    CHECK(g->CursorRow() == 1);
    CHECK(g->CursorCol() == 10);
    delete g;
}

TEST_CASE("a narrowing Resize keeps the saved wrap flag") {
    // xterm ScreenResize does not touch the saved cursor's wrap_flag; the
    // saved column is clamped to the new last column, where the flag is valid.
    // Mutant: Resize clears the saved flag.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2;10Hx");
    Feed(p, *g, DECSC);
    REQUIRE(g->Resize(6, 5).has_value());
    Feed(p, *g, DECRC);
    CHECK(g->CursorCol() == 5);
    CHECK(g->PendingWrap());
    delete g;
}

TEST_CASE("DECRC clears the wrap flag when none was pending at DECSC") {
    // Mutant: DECRC keeps the live wrap flag.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2;10H");
    Feed(p, *g, DECSC);  // cursor in the last column, no wrap pending
    Feed(p, *g, "x");
    REQUIRE(g->PendingWrap());
    Feed(p, *g, DECRC);
    CHECK(!g->PendingWrap());
    delete g;
}

TEST_CASE("DECRC restores G0/G1 designations and the shift state") {
    // Mutant: charset state is not part of the saved cursor.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b(0\x1b)0\x0e");  // G0=G1=graphics, SO (G1 active)
    Feed(p, *g, DECSC);
    Feed(p, *g, "\x1b(B\x1b)B\x0f");  // back to ASCII, SI
    REQUIRE(g->CharsetG0() == 0);
    REQUIRE(g->CharsetG1() == 0);
    REQUIRE(g->CharsetActive() == 0);
    Feed(p, *g, DECRC);
    CHECK(g->CharsetG0() == 1);
    CHECK(g->CharsetG1() == 1);
    CHECK(g->CharsetActive() == 1);
    Feed(p, *g, "q");  // DEC Special Graphics: horizontal line
    CHECK(CP(*g, 0, 0) == U'─');
    delete g;
}

TEST_CASE("DECRC without a prior DECSC resets the character sets") {
    // xterm CursorRestoreFlags: nothing saved -> resetCharsets, home, default
    // pen.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b(0\x1b)0\x0e\x1b[1m\x1b[3;3H");
    Feed(p, *g, DECRC);
    CHECK(g->CharsetG0() == 0);
    CHECK(g->CharsetG1() == 0);
    CHECK(g->CharsetActive() == 0);
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    CHECK(g->PenAttrs() == 0);
    delete g;
}

TEST_CASE("CSI s / CSI u share the DECSC slot and its state") {
    // ctlseqs: CSI s is SCOSC (available while DECLRMM is off, which the grid
    // never enables); xterm routes it to CursorSave like ESC 7.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b(0\x1b[2;10Hx\x1b[s");
    Feed(p, *g, "\x1b(B\x1b[4;4H");
    Feed(p, *g, DECRC);  // ESC 8 reads what CSI s wrote
    CHECK(g->CharsetG0() == 1);
    CHECK(g->PendingWrap());
    Feed(p, *g, "\x1b(B\x1b[5;5H\x1b[u");
    CHECK(g->CharsetG0() == 1);
    CHECK(g->CursorRow() == 1);
    CHECK(g->PendingWrap());
    delete g;
}

// ---------------------------------------------------------------------------
// Snapshot reproduces the saved-cursor state
// ---------------------------------------------------------------------------

namespace {

Grid* Restored(const Grid& src, int cols, int rows) {
    auto snap = Snapshot::Create(src);
    REQUIRE(snap.has_value());
    Grid* dst = Mk(cols, rows);
    VtParser p;
    const auto b = snap.value()->Bytes();
    p.Feed(b.data(), b.size(), *dst);
    delete snap.value();
    return dst;
}

}  // namespace

TEST_CASE("snapshot carries DECOM and charsets of the saved cursor") {
    // Mutant: the snapshot sets the DECSC slot with default DECOM/charsets.
    Grid* src = Mk(12, 8);
    VtParser p;
    Feed(p, *src, "\x1b[3;6r\x1b[?6h\x1b(0\x1b)0\x0e\x1b[2;5H");
    Feed(p, *src, DECSC);
    Feed(p, *src, "\x1b[?6l\x1b(B\x1b)B\x0f\x1b[r\x1b[1;1H");
    Grid* dst = Restored(*src, 12, 8);

    const SavedCursor& sc = dst->SavedCursorMainState();
    CHECK(sc.Decom);
    CHECK(sc.Row == src->SavedCursorMainState().Row);
    CHECK(sc.Col == src->SavedCursorMainState().Col);
    CHECK(sc.CharsetG0 == 1);
    CHECK(sc.CharsetG1 == 1);
    CHECK(sc.CharsetActive == 1);
    // Live state is not disturbed by the slot set-up.
    CHECK(!dst->ModeSet(EMode::Decom));
    CHECK(dst->CharsetG0() == 0);
    CHECK(dst->CharsetActive() == 0);
    CHECK(dst->CursorRow() == 0);
    CHECK(dst->CursorCol() == 0);
    delete dst;
    delete src;
}

TEST_CASE("snapshot of the alt screen carries the main slot state too") {
    // ?47h keeps the main slot as DECSC left it. Mutant: the pre-position
    // ignores DECOM/charsets of the main slot, or leaves the slot's region,
    // DECOM or graphics charset active while the alt screen is painted.
    Grid* src = Mk(12, 8);
    VtParser p;
    Feed(p, *src, "\x1b[3;6r\x1b[?6h\x1b(0\x1b[2;5H");
    Feed(p, *src, DECSC);
    Feed(p, *src, "\x1b[?6l\x1b(B\x1b[?47h");  // region 3..6 stays live
    for (const char* row : {"1", "2", "3", "4", "5", "6", "7", "8"}) {
        Feed(p, *src, "\x1b[");
        Feed(p, *src, row);
        Feed(p, *src, ";1Hqwertyuiopa");  // full row, no scroll: DECOM is off
    }
    REQUIRE(src->IsAltScreen());
    Grid* dst = Restored(*src, 12, 8);

    REQUIRE(dst->IsAltScreen());
    const SavedCursor& sc = dst->SavedCursorMainState();
    CHECK(sc.Decom);
    CHECK(sc.CharsetG0 == 1);
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 12; ++c) CHECK(CP(*dst, r, c) == CP(*src, r, c));
    delete dst;
    delete src;
}

// ---------------------------------------------------------------------------
// SGR 21
// ---------------------------------------------------------------------------

TEST_CASE("SGR 21 is doubly-underlined, not bold off") {
    // Mutant: 21 ignored (or treated as bold off).
    Grid* g = Mk(10, 3);
    VtParser p;
    Feed(p, *g, "\x1b[1m\x1b[21m");
    CHECK((g->PenAttrs() & Underline) != 0);
    CHECK(g->PenAttrEx() == 2);
    CHECK((g->PenAttrs() & Bold) != 0);  // bold is left alone
    Feed(p, *g, "a");
    CHECK(g->CellAt(0, 0).AttrEx == 2);
    CHECK((g->CellAt(0, 0).Attrs & Underline) != 0);
    delete g;
}

TEST_CASE("SGR 24 clears a double underline; SGR 4 replaces it with single") {
    Grid* g = Mk(10, 3);
    VtParser p;
    Feed(p, *g, "\x1b[21m\x1b[24m");
    CHECK((g->PenAttrs() & Underline) == 0);
    CHECK(g->PenAttrEx() == 0);
    Feed(p, *g, "\x1b[21m\x1b[4m");
    CHECK((g->PenAttrs() & Underline) != 0);
    CHECK(g->PenAttrEx() == 1);
    delete g;
}

// ---------------------------------------------------------------------------
// Buffer-switch modes (#109 leftovers)
// ---------------------------------------------------------------------------

TEST_CASE("clearing the alt screen clears its last row") {
    // Mutant: the clear loop stops one row short.
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[?47h\x1b[5;1HLast\x1b[?47l");  // stale text, bottom row
    Feed(p, *g, "\x1b[?47h");
    REQUIRE(CP(*g, 4, 0) == U'L');  // ?47 does not clear
    Feed(p, *g, "\x1b[?47l\x1b[?1049h");
    for (int c = 0; c < 10; ++c) CHECK(CP(*g, 4, c) == 0);
    for (int r = 0; r < 5; ++r) CHECK(CP(*g, r, 0) == 0);
    Feed(p, *g, "\x1b[?1049l\x1b[?47h\x1b[5;1HLast\x1b[?47l");
    Feed(p, *g, "\x1b[?47h\x1b[?1047l");  // ?1047l clears the active alt buffer
    Feed(p, *g, "\x1b[?47h");
    for (int c = 0; c < 10; ++c) CHECK(CP(*g, 4, c) == 0);
    delete g;
}

TEST_CASE("?47 and ?1047 leave the main saved-cursor slot untouched") {
    // Mutant: either mode saves into (or restores from) the main slot.
    // ctlseqs: ?1047 "Use Alternate Screen Buffer", ?1048 is the separate
    // "Save cursor as in DECSC"; xterm srm_ALTBUF / srm_OPT_ALTBUF only call
    // ToAlternate / FromAlternate (charproc.c).
    Grid* g = Mk(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[31m\x1b[4;4H");
    Feed(p, *g, DECSC);  // main slot: (3,3), red
    Feed(p, *g, "\x1b[0m\x1b[1;1H");
    for (std::string_view seq : {"\x1b[?47h", "\x1b[?1047h"}) {
        Feed(p, *g, seq);
        Feed(p, *g, "\x1b[2;2H\x1b[1m");
        const SavedCursor& main = g->SavedCursorMainState();
        CHECK(main.Row == 3);
        CHECK(main.Col == 3);
        CHECK(main.FgKind == EColorKind::Indexed);
        CHECK(g->SavedCursorAltState().Row == 0);  // alt slot untouched too
        Feed(p, *g, seq == "\x1b[?47h" ? "\x1b[?47l" : "\x1b[?1047l");
        CHECK(g->SavedCursorMainState().Row == 3);
        CHECK(g->SavedCursorMainState().Col == 3);
        CHECK(g->CursorRow() == 1);  // and nothing restored on the way out
    }
    Feed(p, *g, DECRC);  // the slot still holds the original save
    CHECK(g->CursorRow() == 3);
    CHECK(g->CursorCol() == 3);
    CHECK(g->PenFgKind() == EColorKind::Indexed);
    delete g;
}
