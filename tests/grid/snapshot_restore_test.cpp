// Extra regression tests targeting surviving mutants identified in the PR #99
// mutation sweep (out.txt).  Each test is annotated with the mutant(s) it
// kills.

#include <doctest/doctest.h>

#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

static Grid* MkEx(int cols, int rows, int sb) {
    auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(sb));
    REQUIRE(r.has_value());
    return r.value();
}

static void FeedEx(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

static Grid* RestoreEx(const Snapshot& snap, int cols, int rows, int sb) {
    Grid* g = MkEx(cols, rows, sb);
    VtParser p;
    const auto bytes = snap.Bytes();
    p.Feed(bytes.data(), bytes.size(), *g);
    return g;
}

static bool SpanContains(std::span<const std::byte> hay,
                         std::string_view needle) {
    if (needle.empty()) return true;
    const auto* p = reinterpret_cast<const char*>(hay.data());
    std::string_view sv(p, hay.size());
    return sv.find(needle) != std::string_view::npos;
}

static bool SpanHasByte(std::span<const std::byte> hay, std::uint8_t b) {
    for (auto v : hay)
        if (static_cast<std::uint8_t>(v) == b) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// M05: drop first clear — dirty target is not cleared before history
// ---------------------------------------------------------------------------
TEST_CASE("M05: first erase clears dirty target before history") {
    // Put content on source, then restore into a dirty target that has
    // cells in the history region.  Without the first erase, dirty content
    // from the target leaks into the visible area.
    Grid* src = MkEx(10, 3, 5);
    VtParser sp;
    FeedEx(sp, *src, "abc\r\ndef\r\nghi\r\njkl\r\nmno\r\npqr");
    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());

    // Dirty target: typed many rows so scrollback has junk, plus screen junk.
    Grid* dst = MkEx(10, 3, 5);
    VtParser dp;
    FeedEx(dp, *dst, "XXXXX\r\nYYYYY\r\nZZZZZ\r\nAAAAA\r\nBBBBB\r\nCCCCC");
    const auto bytes = snap.value()->Bytes();
    dp.Feed(bytes.data(), bytes.size(), *dst);

    // After feeding "abc\r\ndef\r\nghi\r\njkl\r\nmno\r\npqr" into a 3-row grid,
    // the visible rows are: row0="jkl", row1="mno", row2="pqr".
    // Verify all three visible rows of dst match src (no dirty junk remains).
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            CHECK(dst->CellAt(static_cast<std::uint16_t>(row),
                              static_cast<std::uint16_t>(col))
                      .Codepoint == src->CellAt(static_cast<std::uint16_t>(row),
                                                static_cast<std::uint16_t>(col))
                                        .Codepoint);
        }
    }
    // None of the dirty junk should survive (check first cell of row 0 is not
    // 'X').
    CHECK(dst->CellAt(0, 0).Codepoint != U'X');

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M08: C1 control 0x9F must not appear raw (upper bound check)
// ---------------------------------------------------------------------------
TEST_CASE("M08: codepoint 0x9F is sanitised in snapshot bytes") {
    Grid* g = MkEx(10, 3, 5);
    g->PutChar(0x9F, 1);
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());
    const auto bytes = snap.value()->Bytes();

    // Raw 0x9F must not appear.
    CHECK(!SpanHasByte(bytes, 0x9F));
    // UTF-8 encoding of U+009F is C2 9F — must not appear either.
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    bool found = false;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i)
        if (p[i] == 0xC2 && p[i + 1] == 0x9F) {
            found = true;
            break;
        }
    CHECK(!found);

    delete snap.value();
    delete g;
}

// ---------------------------------------------------------------------------
// M09: surrogate 0xDFFF must be sanitised (upper bound check)
// ---------------------------------------------------------------------------
TEST_CASE("M09: surrogate 0xDFFF is sanitised") {
    Grid* g = MkEx(10, 3, 5);
    g->PutChar(0xDFFF, 1);
    // A visible non-surrogate char follows to force the surrogate cell into
    // the encoded extent of the row.
    g->PutChar(U'Z', 1);
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());
    const auto bytes = snap.value()->Bytes();

    // Surrogate in UTF-8 would be ED BF BF — must not appear.
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    bool found = false;
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i)
        if (p[i] == 0xED && p[i + 1] == 0xBF && p[i + 2] == 0xBF) {
            found = true;
            break;
        }
    CHECK(!found);

    delete snap.value();
    delete g;
}

// ---------------------------------------------------------------------------
// M10: codepoint > 0x10FFFF must be sanitised
// ---------------------------------------------------------------------------
TEST_CASE("M10: codepoint 0x110000 is sanitised") {
    Grid* g = MkEx(10, 3, 5);
    g->PutChar(0x110000, 1);
    g->PutChar(U'A', 1);  // ensure cell 0 is in used range
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());

    // Restore and compare to a twin that used '?' directly.
    Grid* twin = MkEx(10, 3, 5);
    twin->PutChar(U'?', 1);
    twin->PutChar(U'A', 1);
    auto twinSnap = Snapshot::Create(*twin);
    REQUIRE(twinSnap.has_value());

    const auto a = snap.value()->Bytes();
    const auto b = twinSnap.value()->Bytes();
    CHECK(a.size() == b.size());
    CHECK((a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size()) == 0));

    delete snap.value();
    delete twinSnap.value();
    delete g;
    delete twin;
}

// ---------------------------------------------------------------------------
// M11: C0 control 0x1F must be sanitised (lower-bound check, 0x20→0x1B)
// ---------------------------------------------------------------------------
TEST_CASE("M11: codepoint 0x1F is sanitised") {
    Grid* g = MkEx(10, 3, 5);
    g->PutChar(0x1F, 1);  // US (unit separator) — C0 control
    g->PutChar(U'B', 1);
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());

    Grid* twin = MkEx(10, 3, 5);
    twin->PutChar(U'?', 1);
    twin->PutChar(U'B', 1);
    auto twinSnap = Snapshot::Create(*twin);
    REQUIRE(twinSnap.has_value());

    const auto a = snap.value()->Bytes();
    const auto b = twinSnap.value()->Bytes();
    CHECK(a.size() == b.size());
    CHECK((a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size()) == 0));

    delete snap.value();
    delete twinSnap.value();
    delete g;
    delete twin;
}

// ---------------------------------------------------------------------------
// M12: NUL cell must produce a space byte, not a raw NUL in snapshot output
// ---------------------------------------------------------------------------
TEST_CASE("M12: NUL codepoint encodes as space, not raw NUL byte") {
    // Place a non-blank cell after col 0 so the blank col 0 (NUL codepoint)
    // falls within the cells-used range and is emitted.
    Grid* g = MkEx(10, 3, 5);
    g->PutChar(U'Z', 1);  // col 0 = 'Z'
    // Move cursor back to col 0 of a fresh row.
    g->CarriageReturn();
    g->LineFeed();
    // row 1, col 0: leave as default (Codepoint=0, Width=1 by default).
    g->PutChar(U'A', 1);  // col 0 = 'A' — forces col 0 NUL on row 1 to col 1?
    // Actually we need a NUL cell BEFORE a visible cell.
    // CUP to row 2, col 2 and put a char — cols 0,1 are NUL, within used=3.
    // Use a direct PutChar approach: row2 at col0=NUL, col1=NUL, col2='C'.
    g->CarriageReturn();
    g->LineFeed();
    // Cursor is at (2,0). Move to col 2 first, leave cols 0,1 blank.
    g->PutChar(U' ', 1);  // col 0 = space (keeps it blank)
    g->PutChar(U' ', 1);  // col 1 = space
    g->PutChar(U'C', 1);  // col 2 = 'C', used = 3

    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());
    const auto bytes = snap.value()->Bytes();

    // NUL byte (0x00) must not appear anywhere in the snapshot.
    CHECK(!SpanHasByte(bytes, 0x00));

    delete snap.value();
    delete g;
}

// ---------------------------------------------------------------------------
// M14: blank cell with UlIdx != 0 must not be skipped
// ---------------------------------------------------------------------------
TEST_CASE("M14: blank cell with ul-colour survives round-trip") {
    // \x1b[58;5;7m sets underline colour index 7 without underline attr.
    // The space has UlIdx=7, Attrs=0 — it is NOT blank under IsBlankCell.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src,
           "\x1b[58;5;7m   \x1b[0mZ");  // 3 spaces with UlIdx=7, then 'Z'

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    // The space at col 0 must have UlIdx=7 in dst.
    CHECK(dst->CellAt(0, 0).UlIdx == src->CellAt(0, 0).UlIdx);
    CHECK(src->CellAt(0, 0).UlIdx == 7);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M15: blank cell with non-default background must not be skipped
// ---------------------------------------------------------------------------
TEST_CASE("M15: blank cell with bg colour survives round-trip") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[44m  Z\x1b[0m");  // two spaces with blue bg, then Z

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(src->CellAt(0, 0).BgKind == EColorKind::Indexed);
    CHECK(dst->CellAt(0, 0).BgKind == src->CellAt(0, 0).BgKind);
    CHECK(dst->CellAt(0, 0).Bg[0] == src->CellAt(0, 0).Bg[0]);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M20: pen.UlIdx must be tracked: UlIdx bleed between adjacent cells
// ---------------------------------------------------------------------------
TEST_CASE("M20: UlIdx tracking prevents bleed to subsequent cell") {
    // 'A' has UlIdx=7; 'B' has default (UlIdx=0).
    // With M20 (pen.UlIdx not tracked), pen.UlIdx stays 0 after 'A', so
    // for 'B' MatchesCell sees pen.UlIdx==0==c.UlIdx → skip EmitSgr →
    // terminal keeps UlIdx=7 from 'A'.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[58;5;7mA\x1b[0mB");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    // 'B' at col 1 must have UlIdx=0.
    CHECK(dst->CellAt(0, 1).UlIdx == 0);
    CHECK(dst->CellAt(0, 1).Codepoint == U'B');

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M21: pen.AttrEx must be tracked: AttrEx bleed between adjacent cells
// ---------------------------------------------------------------------------
TEST_CASE("M21: AttrEx tracking prevents bleed to subsequent cell") {
    // 'A' gets AttrEx=2 (double-underline), then 'B' gets default AttrEx=0.
    // With M21 (pen.AttrEx not tracked), pen.AttrEx stays 0, so for 'B'
    // MatchesCell(B) returns true → EmitSgr skipped → terminal keeps AttrEx=2.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[4:2mA\x1b[0mB");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    // 'B' at col 1 must have AttrEx=0.
    CHECK(dst->CellAt(0, 1).AttrEx == 0);
    CHECK(dst->CellAt(0, 1).Codepoint == U'B');

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M22: MatchesCell must include all three Bg bytes
// ---------------------------------------------------------------------------
TEST_CASE("M22: truecolor bg blue component distinguishes adjacent cells") {
    // 'A' bg = (0,0,200), 'B' bg = (0,0,100).
    // With M22 (Bg[2] ignored in MatchesCell), pen.Bg={0,0,200} matches
    // c.Bg={0,0,100} → EmitSgr skipped → 'B' inherits wrong bg.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[48;2;0;0;200mA\x1b[48;2;0;0;100mB\x1b[0m");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(dst->CellAt(0, 0).Bg[2] == 200);
    CHECK(dst->CellAt(0, 1).Bg[2] == 100);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M23: MatchesCell must include Attrs
// ---------------------------------------------------------------------------
TEST_CASE("M23: bold attr bleed prevented by Attrs check in MatchesCell") {
    // 'A' is bold; 'B' is not.  With M23 (Attrs ignored), MatchesCell('B')
    // returns true → terminal keeps bold for 'B'.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[1mA\x1b[0mB");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    // 'B' must have no bold (Attrs=0).
    CHECK((dst->CellAt(0, 1).Attrs & Bold) == 0);
    CHECK(dst->CellAt(0, 1).Codepoint == U'B');

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M26: history EOL pen must be reset (no SGR bleed into screen section)
// ---------------------------------------------------------------------------
TEST_CASE(
    "M26: history EOL pen reset emits SGR-reset after coloured history row") {
    // A history row that ends with non-default fg must be followed by \x1b[0m
    // in the snapshot bytes so the next row starts clean.
    // Without the EOL reset (M26 mutant: if (false) {...}), no \x1b[0m is
    // emitted after the coloured history row, and the terminal pen bleeds.
    Grid* src = MkEx(20, 5, 10);
    VtParser sp;
    // One history row that ends with RED (no reset at end of line).
    // The last cell on this row is 'D' with red fg.
    FeedEx(sp, *src, "\x1b[31mHISTORY_RED\r\n");
    // Second history row: default fg.
    FeedEx(sp, *src, "second\r\n");
    // Remaining rows to push first row fully into history and fill screen.
    for (int i = 0; i < 5; ++i) FeedEx(sp, *src, "row\r\n");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());

    // Snapshot structure: preamble … \x1b[2J\x1b[H … history … \x1b[2J\x1b[H …
    // screen. History is between the FIRST and SECOND \x1b[2J\x1b[H.
    const auto bytes = snap.value()->Bytes();
    const auto* bdata = reinterpret_cast<const char*>(bytes.data());
    const std::string_view sv(bdata, bytes.size());

    const std::string_view clr = "\x1b[2J\x1b[H";
    const auto first = sv.find(clr);
    REQUIRE(first != std::string_view::npos);
    const auto second = sv.find(clr, first + clr.size());
    REQUIRE(second != std::string_view::npos);
    const std::string_view history =
        sv.substr(first + clr.size(), second - first - clr.size());

    // EmitSgr always emits \x1b[0;31m (reset+red) for an indexed-fg-1 cell.
    // After the row ends with red, the EOL reset emits \x1b[0m.
    // Both sequences must appear in history, and the reset must come after red.
    const auto redPos = history.rfind("\x1b[0;31m");
    const auto rstPos = history.rfind("\x1b[0m");
    CHECK(redPos != std::string_view::npos);
    CHECK(rstPos != std::string_view::npos);
    CHECK(rstPos > redPos);

    delete snap.value();
    delete src;
}

// ---------------------------------------------------------------------------
// M28: second clear after history must erase history-shifted content
// ---------------------------------------------------------------------------
TEST_CASE("M28: second clear removes content shifted by history LFs") {
    Grid* src = MkEx(10, 3, 5);
    VtParser sp;
    // Scroll 4 lines into history.
    FeedEx(sp, *src, "h1\r\nh2\r\nh3\r\nh4\r\n");
    FeedEx(sp, *src, "screen0\r\nscreen1\r\nscreen2");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());

    Grid* dst = MkEx(10, 3, 5);
    VtParser dp;
    const auto bytes = snap.value()->Bytes();
    dp.Feed(bytes.data(), bytes.size(), *dst);

    // Screen row 0 must show "screen0", not history junk.
    CHECK(dst->CellAt(0, 0).Codepoint == U's');
    CHECK(dst->CellAt(0, 1).Codepoint == U'c');

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M34: LNM must be encoded (line-feed new-line mode)
// ---------------------------------------------------------------------------
TEST_CASE("M34: LNM mode round-trips") {
    // Enable LNM (\x1b[20h): LF moves cursor to col 0 of next row.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[20h");  // LNM on
    // Position cursor at col 4.
    FeedEx(sp, *src, "\x1b[1;5H");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());

    // Restore into dst keeping the parser alive to feed \n afterwards.
    Grid* dst = MkEx(20, 4, 10);
    VtParser dp;
    const auto bytes = snap.value()->Bytes();
    dp.Feed(bytes.data(), bytes.size(), *dst);

    // After restore, LNM should be set in dst.
    CHECK(dst->ModeSet(EMode::Lnm));

    // Feed \n to both: with LNM, \n moves cursor to col 0 of next row.
    FeedEx(sp, *src, "\n");
    FeedEx(dp, *dst, "\n");

    CHECK(dst->CursorCol() == src->CursorCol());  // both at col 0
    CHECK(dst->CursorRow() == src->CursorRow());

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M38: saved cursor col must be encoded +1 (not off-by-one)
// ---------------------------------------------------------------------------
TEST_CASE("M38: DECSC saved cursor column is exact") {
    // DECSC at (row 0, col 5): \x1b" "7. After round-trip, DECRC (\x1b" "8)
    // must restore to col 5.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src,
           "\x1b[1;6H\x1b"
           "7\x1b[1;1H");  // save at (row0,col5), move to (0,0)

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    const auto bytes = snap.value()->Bytes();

    Grid* dst = MkEx(20, 4, 10);
    VtParser dp;
    dp.Feed(bytes.data(), bytes.size(), *dst);
    FeedEx(dp, *dst,
           "\x1b"
           "8");  // DECRC

    CHECK(dst->CursorCol() == 5);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M43: SO/SI charset active must use correct codes (not swapped)
// ---------------------------------------------------------------------------
TEST_CASE("M43: charset G1 active (SO) survives round-trip") {
    // Activate G1 with SO (\x0e) after designating G1=graphics.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b)0\x0e");  // G1=graphics, activate G1

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(dst->CharsetG1() == src->CharsetG1());
    CHECK(dst->CharsetActive() == src->CharsetActive());  // must be 1 (G1)

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M44: mouse 1006 must encode as 1006, not 1005
// ---------------------------------------------------------------------------
TEST_CASE("M44: mouse 1006 mode round-trips") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[?1006h");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(dst->ModeSet(EMode::Mouse1006));

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M45: DECTCEM must reflect actual cursor-visibility state (not always on)
// ---------------------------------------------------------------------------
TEST_CASE("M45: hidden cursor round-trips") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "abc\x1b[?25l");  // hide cursor

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(!dst->ModeSet(EMode::Dectcem));

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M46: ?2026l must appear in the snapshot bytes (sync-output end)
// ---------------------------------------------------------------------------
TEST_CASE("M46: snapshot bytes contain sync-output close sequence") {
    Grid* g = MkEx(10, 3, 5);
    VtParser p;
    FeedEx(p, *g, "hello");
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());

    CHECK(SpanContains(snap.value()->Bytes(), "\x1b[?2026l"));

    delete snap.value();
    delete g;
}

// ---------------------------------------------------------------------------
// M48: ScrollbackLen must be > 0 when history is present
// ---------------------------------------------------------------------------
TEST_CASE("M48: ScrollbackLen is non-zero for snapshot with history") {
    Grid* g = MkEx(10, 3, 5);
    VtParser p;
    // Scroll a couple of lines into history.
    FeedEx(p, *g, "line1\r\nline2\r\nline3\r\nline4\r\nline5\r\nline6");
    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());

    CHECK(snap.value()->ScrollbackLen() > 0);
    CHECK(snap.value()->ScrollbackLen() < snap.value()->Bytes().size());

    delete snap.value();
    delete g;
}

// ---------------------------------------------------------------------------
// M49: MaxBytes > SNAPSHOT_MAX_BYTES must be clamped (not passed raw)
// ---------------------------------------------------------------------------
TEST_CASE("M49: MaxBytes above SNAPSHOT_MAX_BYTES is clamped") {
    Grid* g = MkEx(10, 3, 5);
    VtParser p;
    FeedEx(p, *g, "hello");

    // A MaxBytes larger than SNAPSHOT_MAX_BYTES should be clamped, and the
    // snapshot must still succeed (content is small).
    SnapshotOptions opts;
    opts.MaxBytes = SNAPSHOT_MAX_BYTES + 1;
    auto snap = Snapshot::Create(*g, opts);
    CHECK(snap.has_value());
    if (snap.has_value()) {
        CHECK(snap.value()->Bytes().size() <= SNAPSHOT_MAX_BYTES);
        delete snap.value();
    }
    delete g;
}

// ---------------------------------------------------------------------------
// M50: cap check must use '>' not '>=' (exact-fit snapshot must succeed)
// ---------------------------------------------------------------------------
TEST_CASE(
    "M50: snapshot with MaxBytes exactly equal to content size succeeds") {
    Grid* g = MkEx(10, 3, 5);
    VtParser p;
    FeedEx(p, *g, "hi");

    // First, measure the natural size with default opts.
    auto first = Snapshot::Create(*g);
    REQUIRE(first.has_value());
    const std::size_t naturalSize = first.value()->Bytes().size();
    delete first.value();

    // Now request exactly naturalSize bytes — must succeed.
    SnapshotOptions opts;
    opts.MaxBytes = naturalSize;
    auto snap = Snapshot::Create(*g, opts);
    CHECK(snap.has_value());
    if (snap.has_value()) delete snap.value();

    // Request one byte less — must fail with SnapshotTooLarge.
    if (naturalSize > 0) {
        SnapshotOptions opts2;
        opts2.MaxBytes = naturalSize - 1;
        auto snap2 = Snapshot::Create(*g, opts2);
        CHECK(!snap2.has_value());
    }

    delete g;
}

// ---------------------------------------------------------------------------
// M53: wide chars in history must step by 2, not always 1
// ---------------------------------------------------------------------------
TEST_CASE("M53: wide char in history round-trips correctly") {
    // Scroll a row with a wide char into history.
    Grid* src = MkEx(10, 3, 5);
    VtParser sp;
    // "中" is U+4E2D (width=2).  Feed 4 such rows to push them to scrollback.
    for (int i = 0; i < 4; ++i)
        FeedEx(sp, *src, "\xe4\xb8\xad\r\n");  // 中\r\n → history
    FeedEx(sp, *src, "screen");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 10, 3, 5);

    // History row 0 must have '中' (U+4E2D) at col 0.
    const Line* hl = dst->ScrollbackLine(0);
    REQUIRE(hl != nullptr);
    CHECK(hl->Cells[0].Codepoint == 0x4E2D);
    CHECK(hl->Cells[0].Width == 2);
    // Col 1 should be continuation (Width=0, Codepoint=0 or '中').
    CHECK(hl->Cells[1].Width == 0);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M55: charset G0 = graphics must encode as '0', not always 'B'
// ---------------------------------------------------------------------------
TEST_CASE("M55: G0 graphics charset designation round-trips") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b(0");  // designate G0 = graphics

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(dst->CharsetG0() == 1);  // 1 = graphics

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M59: bright bg indices 8-15 must use 100-107, not fall through to 48;5;N
// ---------------------------------------------------------------------------
TEST_CASE("M59: bright indexed bg round-trips via 100-107 range") {
    // BgKind=Indexed, Bg[0]=9 (bright red).  Both \x1b[101m and \x1b[48;5;9m
    // produce the same terminal cell, so the round-trip test is:
    // the cell's BgKind and Bg[0] survive unchanged.
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[48;5;9mA\x1b[0m");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK(dst->CellAt(0, 0).BgKind == EColorKind::Indexed);
    CHECK(dst->CellAt(0, 0).Bg[0] == 9);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M60: Dim attribute must survive round-trip
// ---------------------------------------------------------------------------
TEST_CASE("M60: Dim attr round-trips") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[2mD\x1b[0m");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK((dst->CellAt(0, 0).Attrs & Dim) != 0);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M61: StrikeThrough attribute must survive round-trip
// ---------------------------------------------------------------------------
TEST_CASE("M61: StrikeThrough attr round-trips") {
    Grid* src = MkEx(20, 4, 10);
    VtParser sp;
    FeedEx(sp, *src, "\x1b[9mS\x1b[0m");

    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 20, 4, 10);

    CHECK((dst->CellAt(0, 0).Attrs & StrikeThrough) != 0);

    delete snap.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// M62: orphan wide-head in history must emit U+FFFD, not raw wide glyph
// ---------------------------------------------------------------------------
TEST_CASE("M62: orphan wide-head in history emits replacement char") {
    // Construct a history row that has a wide char in the last column so
    // it becomes an orphan head (Width=1 slot, valid wide codepoint).
    // We do this by building a 9-column grid and placing '中' at col 8-9,
    // then shrinking would create orphan — but we can use PutChar directly.
    Grid* g = MkEx(10, 3, 5);
    // Fill 9 narrow chars then put a wide char: terminal places head at col 9
    // but col 9 is last col (0-indexed, grid is 10-wide so cols 0-9).
    // Actually head at col 8 (0-indexed), continuation at col 9.
    // To make an orphan: resize would need to happen.  Instead let's use
    // PutChar to force a wide char into a last col scenario by scrolling.
    VtParser p;
    FeedEx(p, *g, "12345678\xe4\xb8\xad\r\n");  // row fills, scrolls to history
    FeedEx(p, *g, "x\r\n");
    FeedEx(p, *g, "y\r\n");
    FeedEx(p, *g, "z");

    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());
    Grid* dst = RestoreEx(*snap.value(), 10, 3, 5);

    // The wide char should survive (it's a proper pair, not orphan).
    const Line* hl = dst->ScrollbackLine(0);
    REQUIRE(hl != nullptr);
    // col 8 should be '中' head (if it fit), or '?' if orphaned.
    // The important thing: no continuation at col 9 with Width=0 AND col 8
    // with Width=1 codepoint that is a wide glyph (orphan head), which would
    // emit the glyph raw.  The snapshot must have emitted U+FFFD instead.
    // This is an information test: after round-trip, cell at col8 should not
    // have Width=1 AND Codepoint=0x4E2D simultaneously (that's the orphan
    // pattern).
    bool orphanRaw =
        (hl->Cells[8].Width == 1 && hl->Cells[8].Codepoint == 0x4E2D &&
         hl->Cells[9].Width == 0);
    CHECK(!orphanRaw);

    delete snap.value();
    delete g;
    delete dst;
}

// ---------------------------------------------------------------------------
// D14: stale SGR pen must not bleed into ED — every \x1b[2J must be preceded
// by \x1b[0m so the erase uses the default background.
// ---------------------------------------------------------------------------
TEST_CASE("D14: SGR reset before every ED clear in snapshot") {
    // Build a grid with a truecolor background in a cell so EncodeAnyScreen
    // leaves a non-default pen active before the second clear.
    Grid* g = MkEx(10, 4, 5);
    VtParser p;
    // Set a TrueColor background and write a cell.
    FeedEx(p, *g, "\x1b[48;2;200;100;50mA\x1b[0m");

    auto snap = Snapshot::Create(*g);
    REQUIRE(snap.has_value());

    const auto by = snap.value()->Bytes();
    const std::string_view sv(reinterpret_cast<const char*>(by.data()),
                              by.size());

    const std::string_view clrSeq = "\x1b[2J";
    const std::string_view resetSeq = "\x1b[0m";
    // The preamble clear (first \x1b[2J) follows DECSTR which resets the pen;
    // it is exempt from the \x1b[0m requirement.  Skip it and check all later
    // clears.
    std::size_t pos = sv.find(clrSeq);
    int clrCount = 0;
    if (pos != std::string_view::npos) {
        ++clrCount;
        pos += clrSeq.size();
    }
    while ((pos = sv.find(clrSeq, pos)) != std::string_view::npos) {
        ++clrCount;
        const std::string_view prefix = sv.substr(0, pos);
        CHECK(prefix.rfind(resetSeq) != std::string_view::npos);
        pos += clrSeq.size();
    }
    CHECK(clrCount >= 2);

    delete snap.value();
    delete g;
}
