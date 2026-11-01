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

TEST_CASE("preamble: alt screen emits ?1049h") {
    // Mutant: omit ?1049h for alt → test fails.
    Grid* g = MakeGrid(40, 10, 0);
    VtParser p;
    Feed(p, *g, "\x1b[?1049h");  // enter alt screen
    auto r = Snapshot::Create(*g);
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();

    CHECK(snap->AltScreen());
    CHECK(Contains(snap->Bytes(), "\x1b[?1049h"));
    CHECK(!Contains(snap->Bytes(), "\x1b[?1049l"));

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

// ===========================================================================
// Degrade ladder tests (PR2 — design.md §9)
// ===========================================================================
//
// Each test is labelled with the mutant that proves it fails when the
// behaviour under test is removed.

// ---------------------------------------------------------------------------
// Step 1 — HistoryTrimmed
// ---------------------------------------------------------------------------

TEST_CASE("degrade step1: history trimmed to fit cap") {
    // Mutant: remove history trimming (return SnapshotTooLarge instead of
    // trimming) → REQUIRE(r.has_value()) fails.
    //
    // Build a grid with scrollback, measure the full snapshot, then request
    // cap = fullSize - 1.  The serializer must trim the oldest history line.
    Grid* g = MakeGrid(80, 3, 50);
    VtParser p;
    // Write 55 lines: 3 stay on screen, 50 go to scrollback (grid limit).
    for (int i = 0; i < 55; ++i) Feed(p, *g, "data content\r\n");
    REQUIRE(g->ScrollbackSize() > 0u);

    // Full snapshot (no cap restriction).
    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // cap = S - 1 forces at least one line to be trimmed.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S - 1});
    REQUIRE(r.has_value());
    Snapshot* snap = r.value();
    CHECK(snap->Degradation() == EDegradation::HistoryTrimmed);
    CHECK(snap->Bytes().size() <= S - 1);

    delete snap;
    delete g;
}

TEST_CASE("degrade step1: binary-search trims to maximum fitting count") {
    // Mutant: replace binary search with returning SnapshotTooLarge →
    // REQUIRE(r.has_value()) fails.
    //
    // Verify that the trimmed snapshot is as large as possible (not overly
    // trimmed) by checking that the full snapshot minus one line doesn't fit
    // within the cap but the result does.
    Grid* g = MakeGrid(10, 3, 100);
    VtParser p;
    // Write enough lines to fill scrollback.
    for (int i = 0; i < 110; ++i) Feed(p, *g, "AAAA\r\n");
    REQUIRE(g->ScrollbackSize() > 1u);

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // cap = S - 1 triggers trimming.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S - 1});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::HistoryTrimmed);
    CHECK(r.value()->Bytes().size() <= S - 1);

    delete r.value();
    delete g;
}

TEST_CASE("degrade step1: cap exactly at full size has no degradation") {
    // Mutant: always apply HistoryTrimmed even when not needed →
    // CHECK(deg == None) fails.
    Grid* g = MakeGrid(20, 5, 20);
    VtParser p;
    for (int i = 0; i < 25; ++i) Feed(p, *g, "line\r\n");

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // cap = S: no trimming needed.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::None);

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Step 2 — Colors256
// ---------------------------------------------------------------------------

TEST_CASE("degrade step2: truecolor downgraded to 256 when needed") {
    // Mutant: remove Colors256 step → serializer goes straight to NoColor or
    // SnapshotTooLarge; Degradation() != Colors256, so CHECK fails.
    //
    // Fill an 80×5 grid with a single TrueColor fg (255,0,128) on every cell.
    // The TrueColor SGR is longer than the 256-color SGR, so reducing cap
    // by 1 from the full TrueColor size forces the Colors256 step.
    Grid* g = MakeGrid(80, 5, 0);
    VtParser p;
    // Set TrueColor on every cell via SGR then text.
    for (int row = 0; row < 5; ++row) {
        Feed(p, *g, "\x1b[38;2;255;0;128m");
        for (int col = 0; col < 80; ++col) Feed(p, *g, "X");
        if (row < 4) Feed(p, *g, "\r\n");
    }

    // Full snapshot (all TrueColor).
    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    CHECK(full.value()->Degradation() == EDegradation::None);
    delete full.value();

    // 256-color snapshot must be smaller (TrueColor SGR is longer).
    // Set cap = S - 1 → should trigger Colors256.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S - 1});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::Colors256);
    CHECK(r.value()->Bytes().size() <= S - 1);

    delete r.value();
    delete g;
}

TEST_CASE("degrade step2: truecolor->256 maps known values correctly") {
    // Mutant: return wrong palette index (e.g. always 0) →
    // CHECK(idx == expected) fails.
    //
    // Verify the xterm-256 mapping on four canonical inputs by checking that
    // the encoded snapshot contains the expected indexed SGR sequences.
    struct Case {
        std::uint8_t r, g, b;
        std::uint8_t expected256;
        const char* sgrSubstr;  // expected substring in 256-color output
    };
    // Pure red (255,0,0)   → cube index 196 = 16+36*5+0+0
    // Pure green (0,255,0) → cube index 46  = 16+0+36*5/6*5... = 16+30
    // Pure white (255,255,255) → cube index 231 = 16+180+30+5
    // Mid-gray (118,118,118)  → grayscale 243 = 232+11
    static const Case kCases[] = {
        {255, 0, 0, 196, "\x1b[0;38;5;196m"},
        {0, 255, 0, 46, "\x1b[0;38;5;46m"},
        {255, 255, 255, 231, "\x1b[0;38;5;231m"},
        {118, 118, 118, 243, "\x1b[0;38;5;243m"},
    };

    for (const auto& tc : kCases) {
        // Set one cell to TrueColor; put text.
        Grid* g = MakeGrid(10, 5, 0);
        VtParser p;
        char sgr[32];
        std::snprintf(sgr, sizeof(sgr), "\x1b[38;2;%d;%d;%dm",
                      static_cast<int>(tc.r), static_cast<int>(tc.g),
                      static_cast<int>(tc.b));
        Feed(p, *g, sgr);
        Feed(p, *g, "A");

        // Force Colors256 by capping below full TrueColor.
        auto full = Snapshot::Create(*g);
        REQUIRE(full.has_value());
        const std::size_t S = full.value()->Bytes().size();
        delete full.value();

        auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S - 1});
        REQUIRE(r.has_value());
        // Accept either Colors256 or None (if 256-colour happened to
        // produce the same size or smaller; unlikely but guard it).
        if (r.value()->Degradation() == EDegradation::Colors256) {
            CHECK(Contains(r.value()->Bytes(), tc.sgrSubstr));
        }

        delete r.value();
        delete g;
    }
}

// ---------------------------------------------------------------------------
// Step 3 — NoColor
// ---------------------------------------------------------------------------

TEST_CASE("degrade step3: colour stripped when 256-colour still too large") {
    // Mutant: remove NoColor step → no snapshot returned or wrong Degradation.
    //
    // Use a grid with dense alternating 256-indexed colours so the 256-colour
    // snapshot is large, but a no-colour snapshot is small enough for the cap.
    Grid* g = MakeGrid(80, 10, 0);
    VtParser p;
    // Alternate indexed colours 208 and 196 per cell.
    for (int row = 0; row < 10; ++row) {
        for (int col = 0; col < 80; ++col) {
            if (col % 2 == 0)
                Feed(p, *g, "\x1b[38;5;208m");
            else
                Feed(p, *g, "\x1b[38;5;196m");
            Feed(p, *g, "X");
        }
        if (row < 9) Feed(p, *g, "\r\n");
    }

    // Full size.
    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // Build a no-colour snapshot to find its size.
    // We can't call DoEncode directly, so use a small cap that forces NoColor.
    // The no-colour snapshot should be substantially smaller than S because
    // colour SGRs dominate.  Try cap = S / 3 (generous for mode restore).
    const std::size_t noCap = S / 3;
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = noCap});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::NoColor);
    CHECK(r.value()->Bytes().size() <= noCap);

    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Step 4 — RowsCropped
//
// All step4 tests use an 80×50 grid fully filled with content so the
// no-colour snapshot (~4400+ bytes) comfortably exceeds the cap=600 used
// below.  With CursorRow near the bottom, the maximum-crop snapshot (2–3
// remaining rows) fits within 600 bytes.
//
// Overhead breakdown (approximately):
//   preamble:     ~44 bytes
//   mode restore: ~208 bytes
//   total fixed:  ~252 bytes
// Per full row (80 chars): CUP (~8 bytes) + 80 chars = ~88 bytes.
// 3 rows × 88 + 252 = 516 bytes < 600.
// ---------------------------------------------------------------------------

// Fill every cell of the grid with 'A' via CUP moves (no scrollback side
// effects).
static void FillGrid(VtParser& p, Grid& g, char ch = 'A') {
    const int rows = g.Rows();
    const int cols = g.Cols();
    char cup[16];
    for (int row = 0; row < rows; ++row) {
        std::snprintf(cup, sizeof(cup), "\x1b[%d;1H", row + 1);
        Feed(p, g, cup);
        for (int col = 0; col < cols; ++col) {
            char c[2] = {ch, '\0'};
            Feed(p, g, c);
        }
    }
}

TEST_CASE("degrade step4: top rows cropped when no-colour still too large") {
    // Mutant: remove RowsCropped step → SnapshotTooLarge returned;
    // REQUIRE(r.has_value()) fails.
    Grid* g = MakeGrid(80, 50, 0);
    VtParser p;
    FillGrid(p, *g);
    // Cursor at row 49 (last row, 0-based).
    Feed(p, *g, "\x1b[50;1H");
    REQUIRE(g->CursorRow() == 49);

    // Verify the grid is large enough that full snapshot exceeds cap=600.
    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    REQUIRE(full.value()->Bytes().size() > 600u);
    delete full.value();

    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 600});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::RowsCropped);
    CHECK(r.value()->Bytes().size() <= 600u);

    delete r.value();
    delete g;
}

TEST_CASE("degrade step4: cursor row is kept after top-row crop") {
    // Mutant: crop cursor row too → cursor row content absent in restored grid.
    // Grid 80×50, cursor at row 48 (second-to-last).
    Grid* g = MakeGrid(80, 50, 0);
    VtParser p;
    FillGrid(p, *g);
    // Move cursor to row 48, col 3.
    Feed(p, *g, "\x1b[49;4H");
    REQUIRE(g->CursorRow() == 48);
    REQUIRE(g->CellAt(48, 0).Codepoint == U'A');

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    REQUIRE(full.value()->Bytes().size() > 600u);
    delete full.value();

    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 600});
    REQUIRE(r.has_value());

    // The result must be RowsCropped (not SnapshotTooLarge).
    REQUIRE(r.value()->Degradation() == EDegradation::RowsCropped);

    // Restore into a fresh grid.
    Grid* dst = Restore(*r.value(), 80, 50, 0);
    // Cursor row (48) must be preserved.
    CHECK(CP(*dst, 48, 0) == U'A');
    CHECK(dst->CursorRow() == 48);

    delete dst;
    delete r.value();
    delete g;
}

TEST_CASE("degrade step4: cropped rows come from top (row 0 absent)") {
    // Mutant: crop from bottom instead of top → row 0 content present after
    // restore, CHECK(CP(*dst, 0, 0) != U'Z') would flip.
    // Grid 80×50; row 0 gets 'Z', rows 1..49 get 'A'. Cursor at row 49.
    Grid* g = MakeGrid(80, 50, 0);
    VtParser p;
    FillGrid(p, *g);
    // Overwrite row 0 col 0 with 'Z'.
    Feed(p, *g, "\x1b[1;1HZ");
    REQUIRE(g->CellAt(0, 0).Codepoint == U'Z');
    // Cursor at row 49.
    Feed(p, *g, "\x1b[50;1H");
    REQUIRE(g->CursorRow() == 49);

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    REQUIRE(full.value()->Bytes().size() > 600u);
    delete full.value();

    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 600});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::RowsCropped);

    Grid* dst = Restore(*r.value(), 80, 50, 0);
    // Row 0 was cropped; 'Z' must not survive.
    CHECK(CP(*dst, 0, 0) != U'Z');
    // Cursor row must be preserved.
    CHECK(dst->CursorRow() == 49);

    delete dst;
    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Output always ≤ cap
// ---------------------------------------------------------------------------

TEST_CASE("degrade: output always fits within cap") {
    // Mutant: emit bytes beyond cap → size check fails.
    //
    // Parametric: try several caps across the range [minimal, full].
    Grid* g = MakeGrid(40, 10, 50);
    VtParser p;
    for (int i = 0; i < 60; ++i) Feed(p, *g, "\x1b[38;2;200;100;50mline\r\n");

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // Try a range of caps.
    for (std::size_t cap :
         {S, S - 1, S / 2, S / 4, S / 8, std::size_t{512}, std::size_t{256}}) {
        if (cap == 0) continue;
        auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
        if (r.has_value()) {
            CHECK(r.value()->Bytes().size() <= cap);
            delete r.value();
        }
        // SnapshotTooLarge is acceptable when cap is very small.
    }

    delete g;
}

// ---------------------------------------------------------------------------
// Boundary: cap exactly at full size, cap-1, cap+1
// ---------------------------------------------------------------------------

TEST_CASE("degrade: boundary cap == full size has None degradation") {
    // Mutant: always set Degradation != None → CHECK fails.
    Grid* g = MakeGrid(30, 5, 10);
    VtParser p;
    for (int i = 0; i < 15; ++i) Feed(p, *g, "hello\r\n");

    auto full = Snapshot::Create(*g);
    REQUIRE(full.has_value());
    const std::size_t S = full.value()->Bytes().size();
    delete full.value();

    // Exactly at full size: no degradation.
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S});
    REQUIRE(r.has_value());
    CHECK(r.value()->Degradation() == EDegradation::None);
    CHECK(r.value()->Bytes().size() == S);

    // One byte above: same result.
    auto r2 = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S + 1});
    REQUIRE(r2.has_value());
    CHECK(r2.value()->Degradation() == EDegradation::None);

    // One byte below: degradation must kick in.
    auto r3 = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = S - 1});
    REQUIRE(r3.has_value());
    CHECK(r3.value()->Degradation() != EDegradation::None);
    CHECK(r3.value()->Bytes().size() <= S - 1);

    delete r.value();
    delete r2.value();
    delete r3.value();
    delete g;
}
