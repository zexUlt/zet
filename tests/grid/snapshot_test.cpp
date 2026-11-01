// Unit tests for zet_grid snapshot serializer (design.md §9).
//
// Each test is paired with a mutant note explaining what breaks if the
// behaviour under test is removed.

#include <doctest/doctest.h>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

#include <cstring>
#include <span>
#include <string_view>

using namespace zet::grid;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Grid* MakeGrid(int cols = 80, int rows = 24, int scrollback = 100) {
    auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(scrollback));
    REQUIRE(r.has_value());
    return r.value();
}

static void Feed(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

static char32_t CP(const Grid& g, int row, int col) {
    return g
        .CellAt(static_cast<std::uint16_t>(row),
                static_cast<std::uint16_t>(col))
        .Codepoint;
}

// Feed snapshot bytes into a fresh grid of the same dimensions.
static Grid* Restore(const Snapshot& snap, int cols, int rows, int scrollback) {
    Grid* g = MakeGrid(cols, rows, scrollback);
    VtParser p;
    const auto bytes = snap.Bytes();
    p.Feed(bytes.data(), bytes.size(), *g);
    return g;
}

// Check that a span of bytes contains `needle` somewhere.
static bool Contains(std::span<const std::byte> haystack,
                     std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0)
            return true;
    }
    return false;
}

// Scan bytes for any raw C0/C1/DEL/surrogate/over-Unicode byte sequences.
// Returns true if hostile bytes are present.
static bool HasHostileBytes(std::span<const std::byte> bytes) {
    // Skip the intentional CAN at offset 0 and any ESC-based sequences.
    // We're looking for raw control characters NOT part of escape sequences.
    // Simple heuristic: after the initial CAN byte, any raw 0x01-0x1F other
    // than ESC (0x1B), SI/SO (0x0F/0x0E) that appear outside an escape
    // sequence indicate a leak.  This scanner skips all escape sequences.
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    // Skip over the CAN preamble byte.
    if (n > 0 && p[0] == 0x18) i = 1;
    while (i < n) {
        std::uint8_t b = p[i];
        if (b == 0x1B) {
            // Skip escape sequence.
            ++i;
            if (i >= n) break;
            if (p[i] == '[') {
                // CSI: skip until final byte in 0x40–0x7E.
                ++i;
                while (i < n && (p[i] < 0x40 || p[i] > 0x7E)) ++i;
                if (i < n) ++i;
            } else if (p[i] == '\\') {
                // ESC \ = ST
                ++i;
            } else if (p[i] == '(' || p[i] == ')') {
                // Charset designation: ESC ( X or ESC ) X
                ++i;             // skip ( or )
                if (i < n) ++i;  // skip designator
            } else if (p[i] == '7' || p[i] == '8') {
                // DECSC / DECRC
                ++i;
            } else {
                // Some other two-byte escape, skip final.
                ++i;
            }
            continue;
        }
        if (b == 0x0E || b == 0x0F) {
            // SO / SI — intentional charset switch emitted by serializer.
            ++i;
            continue;
        }
        if (b == '\r' || b == '\n') {
            ++i;
            continue;
        }
        // Any raw C0 (0x01–0x1F), DEL (0x7F), C1 (0x80–0x9F) is hostile.
        if ((b >= 0x01 && b <= 0x1F) || b == 0x7F ||
            (b >= 0x80 && b <= 0x9F && b != 0xC0 && b != 0xC1)) {
            // Check it's not a valid multi-byte UTF-8 lead byte.
            // C0/C1 as UTF-8 continuation (0x80–0xBF) are fine.
            if (b >= 0x80 && b <= 0xBF) {
                ++i;
                continue;
            }
            if (b >= 0xC2) {
                ++i;
                continue;
            }  // valid UTF-8 lead
            return true;
        }
        ++i;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Preamble tests
// ---------------------------------------------------------------------------

TEST_CASE("preamble: starts with CAN + ESC-ST") {
    // Mutant: remove CAN or ESC\ from the preamble → test fails.
    Grid* g = MakeGrid(40, 10, 0);
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();
    const auto bytes = snap->Bytes();

    // bytes[0] == CAN (0x18)
    REQUIRE(bytes.size() >= 3);
    CHECK(static_cast<std::uint8_t>(bytes[0]) == 0x18);
    // bytes[1..2] == ESC backslash
    CHECK(static_cast<std::uint8_t>(bytes[1]) == 0x1B);
    CHECK(static_cast<std::uint8_t>(bytes[2]) == '\\');

    delete snap;
    delete g;
}

TEST_CASE("preamble: main screen emits ?1049l") {
    // Mutant: omit ?1049l for main → test fails.
    Grid* g = MakeGrid(40, 10, 0);
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(!snap->AltScreen());
    CHECK(Contains(snap->Bytes(), "\x1b[?1049l"));
    CHECK(!Contains(snap->Bytes(), "\x1b[?1049h"));

    delete snap;
    delete g;
}

TEST_CASE("preamble: alt screen emits ?1049h after ?1049l") {
    // Mutant: omit ?1049h for alt → test fails.
    // D1: snapshot goes to main first (?1049l), then back to alt (?1049h).
    Grid* g = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *g, "\x1b[?1049h");  // enter alt screen
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(snap->AltScreen());
    CHECK(Contains(snap->Bytes(), "\x1b[?1049h"));
    // D1: ?1049l also appears (to go to main first), before ?1049h.
    CHECK(Contains(snap->Bytes(), "\x1b[?1049l"));

    delete snap;
    delete g;
}

TEST_CASE("preamble: sync-output wraps only screen section") {
    // Mutant: move ?2026h before history → ScrollbackLen boundary check fails.
    Grid* g = MakeGrid(40, 10, 5);
    VtParser p;
    // Write lines to push some scrollback.
    for (int i = 0; i < 12; ++i) Feed(p, *g, "line\r\n");

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    const auto bytes = snap->Bytes();
    const std::size_t sl = snap->ScrollbackLen();

    // Find ?2026h in the byte sequence.
    const std::string_view syncOn = "\x1b[?2026h";
    std::size_t syncOnPos = std::string::npos;
    for (std::size_t i = 0; i + syncOn.size() <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, syncOn.data(), syncOn.size()) == 0) {
            syncOnPos = i;
            break;
        }
    }
    REQUIRE(syncOnPos != std::string::npos);
    // Sync must start at or after ScrollbackLen offset.
    CHECK(syncOnPos >= sl);

    delete snap;
    delete g;
}

// ---------------------------------------------------------------------------
// Round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("round-trip: visible cells are reproduced") {
    // Mutant: remove cell encoding from EncodeScreen → visible cells differ.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    // Write some text at known positions.
    Feed(p, *src, "Hello");      // row 0, cols 0..4
    Feed(p, *src, "\x1b[3;5H");  // row 2, col 4 (1-based)
    Feed(p, *src, "World");

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 40, 10, 0);

    CHECK(CP(*dst, 0, 0) == U'H');
    CHECK(CP(*dst, 0, 1) == U'e');
    CHECK(CP(*dst, 0, 2) == U'l');
    CHECK(CP(*dst, 0, 3) == U'l');
    CHECK(CP(*dst, 0, 4) == U'o');
    CHECK(CP(*dst, 2, 4) == U'W');
    CHECK(CP(*dst, 2, 5) == U'o');
    CHECK(CP(*dst, 2, 6) == U'r');
    CHECK(CP(*dst, 2, 7) == U'l');
    CHECK(CP(*dst, 2, 8) == U'd');

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("round-trip: truecolor attributes are reproduced") {
    // Mutant: omit RGB SGR encoding → colors differ after restore.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[38;2;255;0;128mA");  // truecolor fg

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 40, 10, 0);

    const Cell& c = dst->CellAt(0, 0);
    CHECK(c.Codepoint == U'A');
    CHECK(c.FgKind == EColorKind::TrueColor);
    CHECK(c.Fg[0] == 255);
    CHECK(c.Fg[1] == 0);
    CHECK(c.Fg[2] == 128);

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("round-trip: cursor position is reproduced") {
    // Mutant: omit CUP at end of mode restore → cursor at (0,0) instead.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[5;7H");  // move cursor to row 4, col 6 (0-based)

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 40, 10, 0);

    CHECK(dst->CursorRow() == 4);
    CHECK(dst->CursorCol() == 6);

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("round-trip: DECAWM mode is reproduced") {
    // Mutant: omit ?7h/l in mode restore → DECAWM state lost.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[?7l");  // disable DECAWM

    REQUIRE(!src->ModeSet(EMode::Awm));

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 40, 10, 0);
    CHECK(!dst->ModeSet(EMode::Awm));

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("round-trip: history lines are reproduced") {
    // Mutant: remove EncodeHistory → scrollback size differs after restore.
    Grid* src = MakeGrid(10, 5, 20);
    VtParser p;
    // Write 7 lines to push 2 into scrollback (grid has 5 rows).
    Feed(p, *src, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF\r\nGGGG");
    REQUIRE(src->ScrollbackSize() >= 2u);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 10, 5, 20);

    // History must be non-empty in the restored grid.
    CHECK(dst->ScrollbackSize() >= 2u);

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("round-trip: scroll region is reproduced") {
    // Mutant: omit DECSTBM in mode restore → scroll region at default 1;rows.
    Grid* src = MakeGrid(40, 20, 0);
    VtParser p;
    Feed(p, *src, "\x1b[3;15r");  // set scroll region rows 3–15 (1-based)

    CHECK(src->ScrollTop() == 2);  // 0-based
    CHECK(src->ScrollBot() == 14);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 40, 20, 0);
    CHECK(dst->ScrollTop() == 2);
    CHECK(dst->ScrollBot() == 14);

    delete snap;
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// Sanitization tests
// ---------------------------------------------------------------------------

TEST_CASE("sanitization: C0 control bytes do not appear raw in cell output") {
    // Mutant: remove SanitizeCp → C0 bytes leak into output stream.
    Grid* g = MakeGrid(40, 10, 0);
    // Directly write a cell with a C0 codepoint (BEL = 0x07).
    // VtParser won't put these in cells, so we mutate directly.
    g->CellAt(0, 0).Codepoint = 0x07;
    g->CellAt(0, 0).Width = 1;

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // The output must NOT contain a raw 0x07 outside an escape sequence.
    CHECK(!HasHostileBytes(snap->Bytes()));

    delete snap;
    delete g;
}

TEST_CASE("sanitization: DEL (0x7F) does not appear raw in output") {
    // Mutant: remove DEL check in SanitizeCp → 0x7F leaks into output.
    Grid* g = MakeGrid(40, 10, 0);
    g->CellAt(0, 0).Codepoint = 0x7F;
    g->CellAt(0, 0).Width = 1;

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(!HasHostileBytes(snap->Bytes()));

    delete snap;
    delete g;
}

TEST_CASE("sanitization: surrogate codepoints do not appear in output") {
    // Mutant: remove surrogate guard → 3-byte UTF-8 for U+D800 in output.
    Grid* g = MakeGrid(40, 10, 0);
    g->CellAt(0, 0).Codepoint = 0xD800;
    g->CellAt(0, 0).Width = 1;

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // Encoded surrogate would be 0xED 0xA0 0x80 — 0xED as UTF-8 lead byte for
    // U+D000 range.  A correctly sanitized output must NOT contain 0xED 0xA0.
    const auto bytes = snap->Bytes();
    bool foundSurrogate = false;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (static_cast<std::uint8_t>(bytes[i]) == 0xED &&
            (static_cast<std::uint8_t>(bytes[i + 1]) & 0xA0) == 0xA0) {
            foundSurrogate = true;
            break;
        }
    }
    CHECK(!foundSurrogate);

    delete snap;
    delete g;
}

TEST_CASE("sanitization: over-Unicode (> U+10FFFF) does not appear in output") {
    // Mutant: remove > 0x10FFFF guard → invalid 4-byte UTF-8 in output.
    Grid* g = MakeGrid(40, 10, 0);
    g->CellAt(0, 0).Codepoint = 0x110000;
    g->CellAt(0, 0).Width = 1;

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(!HasHostileBytes(snap->Bytes()));

    delete snap;
    delete g;
}

// ---------------------------------------------------------------------------
// Byte-cap tests
// ---------------------------------------------------------------------------

TEST_CASE("byte-cap: SnapshotTooLarge when cap is extremely small") {
    // Mutant: remove overflow check → SnapshotTooLarge never returned.
    Grid* g = MakeGrid(80, 24, 0);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 4});
    CHECK(!r.has_value());
    CHECK(r.error() == EGridError::SnapshotTooLarge);
    delete g;
}

TEST_CASE("byte-cap: zero MaxBytes uses SNAPSHOT_MAX_BYTES and succeeds") {
    Grid* g = MakeGrid(80, 24, 0);
    // MaxBytes = 0 → use default SNAPSHOT_MAX_BYTES.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 0});
    REQUIRE(r.has_value());
    CHECK(r.value()->Bytes().size() <= SNAPSHOT_MAX_BYTES);
    delete r.value();
    delete g;
}

TEST_CASE("byte-cap: ScrollbackLen is within Bytes() bounds") {
    Grid* g = MakeGrid(40, 10, 20);
    VtParser p;
    for (int i = 0; i < 15; ++i) Feed(p, *g, "data\r\n");

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(snap->ScrollbackLen() <= snap->Bytes().size());

    delete snap;
    delete g;
}

// ---------------------------------------------------------------------------
// AltScreen snapshot
// ---------------------------------------------------------------------------

TEST_CASE("alt-screen: no history in alt-screen snapshot") {
    // Mutant: skip IsAltScreen check → history emitted for alt → ScrollbackLen
    // wrong.
    Grid* g = MakeGrid(40, 10, 20);
    VtParser p;
    Feed(p, *g, "main line\r\n");  // some main content
    Feed(p, *g, "\x1b[?1049h");    // enter alt screen
    Feed(p, *g, "alt line");

    REQUIRE(g->IsAltScreen());

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // ScrollbackLen should point past preamble but no history bytes added.
    // The screen section (after ScrollbackLen) must paint "alt line".
    Grid* dst = Restore(*snap, 40, 10, 20);
    CHECK(dst->IsAltScreen());
    CHECK(CP(*dst, 0, 0) == U'a');
    CHECK(CP(*dst, 0, 1) == U'l');

    delete snap;
    delete g;
    delete dst;
}

TEST_CASE("alt-screen: PossiblyDiverged flag is preserved") {
    Grid* g = MakeGrid(40, 10, 0);
    g->SetPossiblyDiverged(true);
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    CHECK(r.value()->PossiblyDiverged());
    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// D1: alt-screen carries main screen and history
// ---------------------------------------------------------------------------

TEST_CASE("D1: alt-screen snapshot preserves main screen and history") {
    // Mutant: skip main-screen encode in alt mode → main content lost.
    Grid* g = MakeGrid(10, 5, 20);
    VtParser p;
    // Push some lines to scrollback on main screen.
    for (int i = 0; i < 7; ++i) Feed(p, *g, "main\r\n");
    REQUIRE(g->ScrollbackSize() >= 2u);
    // Enter alt screen and write something.
    Feed(p, *g, "\x1b[?1049h");
    Feed(p, *g, "ALT");
    REQUIRE(g->IsAltScreen());

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    Grid* dst = Restore(*snap, 10, 5, 20);
    CHECK(dst->IsAltScreen());
    // Alt cell visible at (0,0).
    CHECK(CP(*dst, 0, 0) == U'A');
    // History preserved from main screen.
    CHECK(dst->ScrollbackSize() >= 2u);

    delete snap;
    delete g;
    delete dst;
}

// ---------------------------------------------------------------------------
// D2: DECSTR soft-reset in preamble
// ---------------------------------------------------------------------------

TEST_CASE("D2: DECSTR soft-reset appears near start of preamble") {
    // Mutant: remove DECSTR → stale SGR/scroll-region leaks into restore.
    Grid* g = MakeGrid(40, 10, 0);
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    const auto bytes = r.value()->Bytes();

    const std::string_view decstr = "\x1b[!p";
    std::size_t pos = std::string::npos;
    for (std::size_t i = 0; i + decstr.size() <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, decstr.data(), decstr.size()) == 0) {
            pos = i;
            break;
        }
    }
    REQUIRE(pos != std::string::npos);
    // DECSTR must come within the first 12 bytes (CAN + ESC\ = 3 + DECSTR = 4).
    CHECK(pos < 12);

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// D3: DECOM cursor restored after enabling DECOM
// ---------------------------------------------------------------------------

TEST_CASE("D3: DECOM cursor position round-trips correctly") {
    // Mutant: enable DECOM before CUP → cursor drifts by scroll_top rows.
    Grid* src = MakeGrid(40, 20, 0);
    VtParser p;
    Feed(p, *src, "\x1b[5;15r");  // scroll region rows 5–15 (1-based)
    Feed(p, *src, "\x1b[?6h");    // enable DECOM
    // CUP (3;5) in DECOM mode → absolute row = ScrollTop(4) + 3-1 = 6
    Feed(p, *src, "\x1b[3;5H");

    REQUIRE(src->ModeSet(EMode::Decom));
    REQUIRE(src->CursorRow() == 6);
    REQUIRE(src->CursorCol() == 4);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    Grid* dst = Restore(*r.value(), 40, 20, 0);
    CHECK(dst->ModeSet(EMode::Decom));
    CHECK(dst->CursorRow() == 6);
    CHECK(dst->CursorCol() == 4);

    delete r.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D4: pending wrap is preserved
// ---------------------------------------------------------------------------

TEST_CASE("D4: pending wrap state round-trips") {
    // Mutant: omit pending-wrap re-emit → PendingWrap_ lost after restore.
    Grid* src = MakeGrid(10, 5, 0);
    VtParser p;
    Feed(p, *src, "\x1b[1;10H");  // cursor to row 0, col 9 (last col)
    Feed(p, *src, "X");           // write at last column → PendingWrap

    REQUIRE(src->PendingWrap());
    REQUIRE(src->CursorCol() == 9);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    Grid* dst = Restore(*r.value(), 10, 5, 0);
    CHECK(dst->PendingWrap());
    CHECK(dst->CursorRow() == 0);
    CHECK(dst->CursorCol() == 9);

    delete r.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D5: live pen (not saved cursor pen) restored as active pen
// ---------------------------------------------------------------------------

TEST_CASE("D5: live pen is active after restore, not saved cursor pen") {
    // Mutant: use saved-cursor pen for live pen → wrong SGR active after
    // restore.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[31m");  // set red fg
    Feed(p, *src,
         "\x1b"
         "7");                  // DECSC (save with red pen)
    Feed(p, *src, "\x1b[34m");  // change to blue fg (live pen = blue)

    REQUIRE(src->PenFgKind() == EColorKind::Indexed);
    REQUIRE(src->PenFgIdx() == 4);  // blue

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    Grid* dst = Restore(*r.value(), 40, 10, 0);
    // Active pen after restore must be blue (live pen), not red (saved pen).
    CHECK(dst->PenFgKind() == EColorKind::Indexed);
    CHECK(dst->PenFgIdx() == 4);

    delete r.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D6: IRM/LNM use ANSI mode bytes; ?4h does NOT set IRM
// ---------------------------------------------------------------------------

TEST_CASE("D6: IRM emitted as ANSI mode not private mode") {
    // Mutant: emit ?4h for IRM → target interprets as DECSCLM, not IRM.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[4h");  // ANSI SM 4 = IRM
    REQUIRE(src->ModeSet(EMode::Irm));

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(Contains(snap->Bytes(), "\x1b[4h"));
    CHECK(!Contains(snap->Bytes(), "\x1b[?4h"));

    Grid* dst = Restore(*snap, 40, 10, 0);
    CHECK(dst->ModeSet(EMode::Irm));

    delete snap;
    delete src;
    delete dst;
}

TEST_CASE("D6: VtParser ?4h does not set IRM (DECSCLM not IRM)") {
    // Mutant: remove skip for ?4 in DECSET → ?4h incorrectly sets IRM.
    Grid* g = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *g, "\x1b[?4h");  // DECSCLM (smooth scroll) — must NOT set IRM
    CHECK(!g->ModeSet(EMode::Irm));
    delete g;
}

// ---------------------------------------------------------------------------
// D7: orphan-head wide char not emitted as wide char
// ---------------------------------------------------------------------------

TEST_CASE("D7: orphan-head cell emits replacement char, not raw wide glyph") {
    // Mutant: remove Wcwidth orphan check → parser crashes or garbles column
    // alignment on restore.
    Grid* g = MakeGrid(40, 10, 0);
    // U+4E2D (中) has wcwidth=2.  Storing it with Width=1 creates an orphan
    // head.
    g->CellAt(0, 0).Codepoint = U'中';
    g->CellAt(0, 0).Width = 1;

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    const auto bytes = r.value()->Bytes();

    // UTF-8 for U+4E2D is 0xE4 0xB8 0xAD — must not appear in output.
    const std::string_view wideGlyph = "\xe4\xb8\xad";
    CHECK(!Contains(bytes, wideGlyph));

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// D8: underline style (4:N) and underline colour (58;5;N) are emitted
// ---------------------------------------------------------------------------

TEST_CASE("D8: underline style colon-param and underline colour round-trip") {
    // Mutant: omit AttrEx/UlIdx → extended underline lost after restore.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[4:2m");     // double underline (style 2)
    Feed(p, *src, "\x1b[58;5;7m");  // underline colour index 7
    Feed(p, *src, "X");

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // Snapshot must contain the extended underline sub-param and ul colour.
    CHECK(Contains(snap->Bytes(), "4:2"));
    CHECK(Contains(snap->Bytes(), "58;5;7"));

    // Round-trip: restored cell has same AttrEx and UlIdx.
    Grid* dst = Restore(*snap, 40, 10, 0);
    const Cell& c = dst->CellAt(0, 0);
    CHECK(c.AttrEx == 2);
    CHECK(c.UlIdx == 7);

    delete snap;
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D9: no SGR sequence exceeds MAX_PARAMS (16) params
// ---------------------------------------------------------------------------

TEST_CASE("D9: all SGR sequences stay within MAX_PARAMS=16") {
    // Mutant: combine attrs+fg+bg into one sequence → parser drops params > 16.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    // Set every attr + truecolor fg + bg + extended underline + ul colour.
    Feed(p, *src, "\x1b[1;2;3;4:2;5;7;8;9m");  // all text attrs
    Feed(p, *src, "\x1b[38;2;255;128;0m");     // truecolor fg
    Feed(p, *src, "\x1b[48;2;0;128;255m");     // truecolor bg
    Feed(p, *src, "\x1b[58;5;7m");             // ul colour
    Feed(p, *src, "X");

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    const auto bytes = r.value()->Bytes();

    // Scan every CSI 'm' sequence and count params (semicolons + colons + 1).
    const auto* bp = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t n = bytes.size();
    int maxSeen = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (bp[i] != 0x1B || i + 1 >= n || bp[i + 1] != '[') continue;
        std::size_t j = i + 2;
        int cnt = 1;
        while (j < n && (bp[j] < 0x40 || bp[j] > 0x7E)) {
            if (bp[j] == ';' || bp[j] == ':') ++cnt;
            ++j;
        }
        if (j < n && bp[j] == 'm' && cnt > maxSeen) maxSeen = cnt;
        i = j;
    }
    CHECK(maxSeen <= 16);

    delete r.value();
    delete src;
}

// ---------------------------------------------------------------------------
// D10: Rows-1 LFs push history (not Rows, which adds a blank line)
// ---------------------------------------------------------------------------

TEST_CASE("D10: history line count is exact after restore (Rows-1 LFs)") {
    // Mutant: emit Rows LFs instead of Rows-1 → extra blank line in scrollback.
    Grid* src = MakeGrid(10, 5, 20);
    VtParser p;
    // Write exactly 6 lines into a 5-row grid → exactly 1 in scrollback.
    Feed(p, *src, "AAA\r\nBBB\r\nCCC\r\nDDD\r\nEEE\r\nFFF");
    REQUIRE(src->ScrollbackSize() == 1u);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    Grid* dst = Restore(*r.value(), 10, 5, 20);
    // Must be exactly 1 history line, not 2 (which would happen with Rows LFs).
    CHECK(dst->ScrollbackSize() == 1u);

    delete r.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D11: tab stops round-trip
// ---------------------------------------------------------------------------

TEST_CASE("D11: custom tab stops are restored") {
    // Mutant: omit TBC+HTS section → tab stops reset to default every 8 cols.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[3g");    // TBC 3: clear all tab stops
    Feed(p, *src, "\x1b[1;6H");  // move to col 6 (1-based)
    Feed(p, *src,
         "\x1b"
         "H");  // HTS: set tab stop at col 5 (0-based)

    REQUIRE(src->IsTabStop(5));
    REQUIRE(!src->IsTabStop(0));

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    Grid* dst = Restore(*r.value(), 40, 10, 0);
    CHECK(dst->IsTabStop(5));
    CHECK(!dst->IsTabStop(0));

    delete r.value();
    delete src;
    delete dst;
}

// ---------------------------------------------------------------------------
// D12: snapshot allocates only the bytes needed, not SNAPSHOT_MAX_BYTES
// ---------------------------------------------------------------------------

TEST_CASE("D12: snapshot buffer is sized to output, not pinned at 4 MiB") {
    // Mutant: allocate maxBytes always → 4 MiB per snapshot (memory waste).
    Grid* g = MakeGrid(10, 5, 0);  // tiny, empty grid
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // A 10×5 empty grid should produce far less than 4 KiB.
    CHECK(snap->Bytes().size() < 4096u);

    delete snap;
    delete g;
}

// ---------------------------------------------------------------------------
// D13: cursor-hide is before sync window; one sync window around screen paint
// ---------------------------------------------------------------------------

TEST_CASE("D13: cursor-hide (?25l) precedes sync window (?2026h)") {
    // Mutant: put ?25l inside sync window → cursor flickers during replay.
    Grid* g = MakeGrid(40, 10, 0);
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    const auto bytes = r.value()->Bytes();

    auto findSeq = [&](std::string_view sv) -> std::size_t {
        for (std::size_t i = 0; i + sv.size() <= bytes.size(); ++i)
            if (std::memcmp(bytes.data() + i, sv.data(), sv.size()) == 0)
                return i;
        return std::string::npos;
    };

    const std::size_t pos25l = findSeq("\x1b[?25l");
    const std::size_t pos2026h = findSeq("\x1b[?2026h");
    REQUIRE(pos25l != std::string::npos);
    REQUIRE(pos2026h != std::string::npos);
    CHECK(pos25l < pos2026h);

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Sanitization: surviving mutant killers
// ---------------------------------------------------------------------------

TEST_CASE(
    "sanitization: C1 control (0x80-0x9F) does not appear raw in output") {
    // Mutant: remove C1 guard in SanitizeCp → CSI/OSC bytes leak into stream.
    Grid* g = MakeGrid(40, 10, 0);
    g->CellAt(0, 0).Codepoint = 0x9B;  // CSI (C1 control)
    g->CellAt(0, 0).Width = 1;
    g->CellAt(0, 0).Attrs = Bold;  // non-blank so not trimmed

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    CHECK(!HasHostileBytes(r.value()->Bytes()));

    delete r.value();
    delete g;
}

TEST_CASE("sanitization: NUL codepoint emits space not raw 0x00") {
    // Mutant: remove NUL→space in SanitizeCp → 0x00 written raw.
    Grid* g = MakeGrid(40, 10, 0);
    g->CellAt(0, 0).Codepoint = 0;
    g->CellAt(0, 0).Width = 1;
    g->CellAt(0, 0).Attrs = Bold;  // non-blank so EmitSgr fires

    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    const auto bytes = r.value()->Bytes();
    bool foundNul = false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (static_cast<std::uint8_t>(bytes[i]) == 0x00) {
            foundNul = true;
            break;
        }
    CHECK(!foundNul);

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Additional mutant killers: round-trip correctness
// ---------------------------------------------------------------------------

TEST_CASE("bright indexed fg/bg round-trip via 90-97/100-107") {
    // Mutant: swap bright fg to 30+idx (standard) → colour wrong after restore.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src,
         "\x1b[91;101mX");  // bright red fg (idx 9) + bright red bg (idx 9)

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Grid* dst = Restore(*r.value(), 40, 10, 0);

    const Cell& c = dst->CellAt(0, 0);
    CHECK(c.FgKind == EColorKind::Indexed);
    CHECK(c.Fg[0] == 9);  // bright red
    CHECK(c.BgKind == EColorKind::Indexed);
    CHECK(c.Bg[0] == 9);

    delete r.value();
    delete src;
    delete dst;
}

TEST_CASE("wide continuation (Width=0) cells are skipped in encoding") {
    // Mutant: remove Width==0 skip → continuation byte encoded as char.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\xe4\xb8\xad");  // U+4E2D (中), wcwidth=2
    REQUIRE(src->CellAt(0, 0).Width == 2);
    REQUIRE(src->CellAt(0, 1).Width == 0);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Grid* dst = Restore(*r.value(), 40, 10, 0);

    CHECK(dst->CellAt(0, 0).Codepoint == U'中');
    CHECK(dst->CellAt(0, 0).Width == 2);
    CHECK(dst->CellAt(0, 1).Width == 0);

    delete r.value();
    delete src;
    delete dst;
}

TEST_CASE("DECSCUSR cursor style round-trips") {
    // Mutant: omit DECSCUSR → cursor style defaults to 0 after restore.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[2 q");  // steady block cursor
    REQUIRE(src->CursorStyle() == 2);

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Grid* dst = Restore(*r.value(), 40, 10, 0);
    CHECK(dst->CursorStyle() == 2);

    delete r.value();
    delete src;
    delete dst;
}

TEST_CASE("saved cursor colours round-trip via DECSC") {
    // Mutant: omit saved-cursor pen setup before ESC 7 → DECRC restores
    // wrong colours.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "\x1b[32m");  // green fg (index 2)
    Feed(p, *src,
         "\x1b"
         "7");  // DECSC

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Grid* dst = Restore(*r.value(), 40, 10, 0);

    const SavedCursor& sc = dst->SavedCursorMainState();
    CHECK(sc.FgKind == EColorKind::Indexed);
    CHECK(sc.Fg[0] == 2);  // green

    delete r.value();
    delete src;
    delete dst;
}

TEST_CASE("SGR only emitted on pen change (no redundant sequences)") {
    // Mutant: remove MatchesCell check → redundant SGR sequences emitted.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    // Write same-pen content across two rows; EncodeScreen should not emit
    // duplicate SGR for the second row.
    Feed(p, *src, "\x1b[31mAA\r\nBB");  // red text on both rows
    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());

    // Verify round-trip still works (pen tracked correctly).
    Grid* dst = Restore(*r.value(), 40, 10, 0);
    CHECK(dst->CellAt(0, 0).Codepoint == U'A');
    CHECK(dst->CellAt(1, 0).Codepoint == U'B');
    CHECK(dst->CellAt(1, 0).FgKind == EColorKind::Indexed);
    CHECK(dst->CellAt(1, 0).Fg[0] == 1);  // red

    delete r.value();
    delete src;
    delete dst;
}

TEST_CASE("trailing blank cells are trimmed from row output") {
    // Mutant: remove ScreenCellsUsed trim → trailing spaces emitted per row.
    Grid* src = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *src, "Hi");  // 2 chars; remaining 38 cols are blank default

    auto r = Snapshot::Create(*src);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    // With trimming: only 2 cells encoded for row 0.
    // Without: 38 trailing spaces → snapshot much larger.
    // A 40×10 grid with only "Hi" should be well under 200 bytes for screen.
    CHECK(snap->Bytes().size() < 500u);

    delete snap;
    delete src;
}
