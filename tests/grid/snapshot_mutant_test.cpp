// Tests that close the survivors of a mutation sweep over the snapshot
// encoder (src/grid/snapshot.cpp).  Each test names the mutants it kills; the
// oracle is a round trip (encode, feed a fresh Grid, compare everything the
// encoder promises to reproduce) or, where the round-tripped state is the same
// for a wrong and a right encoding, the exact bytes the design (decision 034)
// asks for.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

struct Term {
    std::unique_ptr<Grid> G;
    VtParser P;

    Term(int cols, int rows, int scrollback) {
        auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                              static_cast<std::uint16_t>(rows),
                              static_cast<std::uint32_t>(scrollback));
        REQUIRE(r.has_value());
        G.reset(r.value());
    }

    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
};

std::string View(const Snapshot& s) {
    const auto b = s.Bytes();
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

bool Contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t Count(const std::string& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

std::string Describe(const Cell& c) {
    return "cp=" +
           std::to_string(static_cast<std::uint32_t>(
               c.Codepoint == 0 ? U' ' : c.Codepoint)) +
           " w=" + std::to_string(c.Width) + " at=" + std::to_string(c.Attrs) +
           " fk=" + std::to_string(static_cast<int>(c.FgKind)) + "/" +
           std::to_string(c.Fg[0]) + "," + std::to_string(c.Fg[1]) + "," +
           std::to_string(c.Fg[2]) +
           " bk=" + std::to_string(static_cast<int>(c.BgKind)) + "/" +
           std::to_string(c.Bg[0]) + "," + std::to_string(c.Bg[1]) + "," +
           std::to_string(c.Bg[2]) + " ax=" + std::to_string(c.AttrEx) +
           " ul=" + std::to_string(c.UlIdx);
}

bool SameCell(const Cell& a, const Cell& b) {
    return Describe(a) == Describe(b);
}

bool SameSaved(const SavedCursor& a, const SavedCursor& b) {
    return a.Row == b.Row && a.Col == b.Col &&
           SameCell(Cell{.Attrs = a.Attrs,
                         .FgKind = a.FgKind,
                         .BgKind = a.BgKind,
                         .Fg = {a.Fg[0], a.Fg[1], a.Fg[2]},
                         .Bg = {a.Bg[0], a.Bg[1], a.Bg[2]},
                         .AttrEx = a.AttrEx,
                         .UlIdx = a.UlIdx},
                    Cell{.Attrs = b.Attrs,
                         .FgKind = b.FgKind,
                         .BgKind = b.BgKind,
                         .Fg = {b.Fg[0], b.Fg[1], b.Fg[2]},
                         .Bg = {b.Bg[0], b.Bg[1], b.Bg[2]},
                         .AttrEx = b.AttrEx,
                         .UlIdx = b.UlIdx}) &&
           a.Decom == b.Decom && a.CharsetG0 == b.CharsetG0 &&
           a.CharsetG1 == b.CharsetG1 && a.CharsetActive == b.CharsetActive;
}

/// Empty when the two grids hold the same state, else a list of differences.
std::string Diff(const Grid& a, const Grid& b) {
    std::string out;
    auto add = [&out](const std::string& what) { out += what + "\n"; };
    if (a.Cols() != b.Cols() || a.Rows() != b.Rows()) return "size\n";
    if (a.ScrollbackSize() != b.ScrollbackSize()) {
        add("scrollback size " + std::to_string(a.ScrollbackSize()) + " vs " +
            std::to_string(b.ScrollbackSize()));
    } else {
        for (std::uint32_t i = 0; i < a.ScrollbackSize(); ++i)
            for (std::uint16_t c = 0; c < a.Cols(); ++c)
                if (!SameCell(a.ScrollbackLine(i)->Cells[c],
                              b.ScrollbackLine(i)->Cells[c]))
                    add("scrollback[" + std::to_string(i) + "][" +
                        std::to_string(c) +
                        "]: " + Describe(a.ScrollbackLine(i)->Cells[c]) +
                        " | " + Describe(b.ScrollbackLine(i)->Cells[c]));
    }
    for (std::uint16_t r = 0; r < a.Rows(); ++r)
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            if (!SameCell(a.MainCellAt(r, c), b.MainCellAt(r, c)))
                add("main[" + std::to_string(r) + "][" + std::to_string(c) +
                    "]: " + Describe(a.MainCellAt(r, c)) + " | " +
                    Describe(b.MainCellAt(r, c)));
            if (a.IsAltScreen() && !SameCell(a.CellAt(r, c), b.CellAt(r, c)))
                add("alt[" + std::to_string(r) + "][" + std::to_string(c) +
                    "]: " + Describe(a.CellAt(r, c)) + " | " +
                    Describe(b.CellAt(r, c)));
        }
    if (a.IsAltScreen() != b.IsAltScreen()) add("alt screen flag");
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol())
        add("cursor (" + std::to_string(a.CursorRow()) + "," +
            std::to_string(a.CursorCol()) + ") vs (" +
            std::to_string(b.CursorRow()) + "," +
            std::to_string(b.CursorCol()) + ")");
    if (a.PendingWrap() != b.PendingWrap()) add("pending wrap");
    if (a.ScrollTop() != b.ScrollTop() || a.ScrollBot() != b.ScrollBot())
        add("scroll region");
    const std::uint32_t mask = ~ModeFlag(EMode::SyncOutput);
    if ((a.Modes() & mask) != (b.Modes() & mask)) add("modes");
    if (!SameSaved(a.SavedCursorMainState(), b.SavedCursorMainState()))
        add("main saved cursor");
    if (!SameSaved(a.SavedCursorAltState(), b.SavedCursorAltState()))
        add("alt saved cursor");
    if (a.CharsetG0() != b.CharsetG0() || a.CharsetG1() != b.CharsetG1() ||
        a.CharsetActive() != b.CharsetActive())
        add("charsets");
    if (a.CursorStyle() != b.CursorStyle()) add("cursor style");
    for (std::uint16_t c = 0; c < a.Cols(); ++c)
        if (a.IsTabStop(c) != b.IsTabStop(c))
            add("tab stop " + std::to_string(c));
    if (a.PenAttrs() != b.PenAttrs() || a.PenFgKind() != b.PenFgKind() ||
        a.PenBgKind() != b.PenBgKind() || a.PenAttrEx() != b.PenAttrEx() ||
        a.PenUlIdx() != b.PenUlIdx() || a.PenFg()[0] != b.PenFg()[0] ||
        a.PenFg()[1] != b.PenFg()[1] || a.PenFg()[2] != b.PenFg()[2] ||
        a.PenBg()[0] != b.PenBg()[0] || a.PenBg()[1] != b.PenBg()[1] ||
        a.PenBg()[2] != b.PenBg()[2])
        add("pen");
    return out;
}

struct Trip {
    std::string Bytes;
    std::string Delta;
};

/// Feed `input` to a source grid, snapshot it, restore into a fresh grid and
/// compare.  When `after` is not empty it is then fed to both grids and the
/// comparison repeated, which exposes pen and charset state the snapshot got
/// wrong without a visible cell to show it.
Trip RoundTrip(int cols, int rows, int scrollback, std::string_view input,
               std::string_view after = {}) {
    Term src(cols, rows, scrollback);
    src.Feed(input);
    auto snap = Snapshot::Create(*src.G);
    REQUIRE(snap.has_value());
    const std::unique_ptr<Snapshot> owner(snap.value());
    Term dst(cols, rows, scrollback);
    const auto b = owner->Bytes();
    dst.P.Feed(b.data(), b.size(), *dst.G);
    Trip t{View(*owner), Diff(*src.G, *dst.G)};
    if (!after.empty()) {
        src.Feed(after);
        dst.Feed(after);
        t.Delta += Diff(*src.G, *dst.G);
    }
    return t;
}

std::size_t FullSize(const Grid& g) {
    auto r = Snapshot::Create(g);
    REQUIRE(r.has_value());
    const std::unique_ptr<Snapshot> owner(r.value());
    REQUIRE(owner->Degradation() == EDegradation::None);
    return owner->Bytes().size();
}

/// Largest cap at which Create selects `want`; fails the test if none does.
std::size_t LargestCapWith(const Grid& g, EDegradation want) {
    for (std::size_t cap = FullSize(g); cap > 0; --cap) {
        auto r = Snapshot::Create(g, SnapshotOptions{.MaxBytes = cap});
        if (!r.has_value()) break;
        const std::unique_ptr<Snapshot> owner(r.value());
        if (owner->Degradation() == want) return cap;
    }
    FAIL("no cap selects the requested degradation step");
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Blank trimming.
// ---------------------------------------------------------------------------

TEST_CASE("trailing blanks with a non-default pen are not trimmed") {
    // A blank is trimmed only if it is indistinguishable from an erased cell.
    // Kills: IsBlankCell ignoring FgKind, Attrs, AttrEx, UlIdx.
    const std::string_view inputs[] = {
        "ab\x1b[31m \x1b[0m",
        "ab\x1b[1m \x1b[0m",
        "ab\x1b[4m \x1b[0m",
        "ab\x1b[7m \x1b[0m",
        "ab\x1b[58;5;3m \x1b[0m",
        "ab\x1b[4:3m\x1b[24m \x1b[0m",
        "ab\x1b[4m\x1b[58;5;3m\x1b[24m \x1b[0m",
    };
    for (const std::string_view in : inputs) {
        INFO("input: " << in);
        const Trip t = RoundTrip(12, 3, 0, in, "x");
        CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    }
}

TEST_CASE("trailing plain spaces are trimmed from the bytes") {
    // The round-tripped state is the same with or without them, so only the
    // bytes tell.  Kills: IsBlankCell not treating a printed space as blank.
    const Trip t = RoundTrip(12, 3, 4, "ab      \x1b[1;3H", "");
    CHECK(t.Delta.empty());
    CHECK_FALSE(Contains(t.Bytes, "ab "));
    Term hist(12, 2, 4);
    hist.Feed("cd      \r\n\r\n\r\nz");
    auto s = Snapshot::Create(*hist.G);
    REQUIRE(s.has_value());
    const std::unique_ptr<Snapshot> owner(s.value());
    CHECK_FALSE(Contains(View(*owner), "cd "));
}

// ---------------------------------------------------------------------------
// SGR diffing and the colour encodings.
// ---------------------------------------------------------------------------

TEST_CASE("adjacent cells differing in one colour component keep their own") {
    // Kills: MatchesCell ignoring Fg[1], Bg[2], BgKind (black vs default).
    const std::string_view inputs[] = {
        "\x1b[38;2;10;20;30ma\x1b[38;2;11;20;30mb\x1b[38;2;11;21;30mc"
        "\x1b[38;2;11;21;31md",
        "\x1b[48;2;10;20;30ma\x1b[48;2;11;20;30mb\x1b[48;2;11;21;30mc"
        "\x1b[48;2;11;21;31md",
        "\x1b[40ma\x1b[0mb",
        "\x1b[30ma\x1b[0mb",
        "\x1b[48;5;0ma\x1b[0mb",
    };
    for (const std::string_view in : inputs) {
        INFO("input: " << in);
        const Trip t = RoundTrip(12, 3, 0, in);
        CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    }
}

TEST_CASE("palette indices 8 and 15 use the bright SGR ranges") {
    // Kills: foreground/background index thresholds off by one (8 would be
    // written as 38/48, which starts an extended colour).
    const Trip t =
        RoundTrip(12, 3, 0,
                  "\x1b[90ma\x1b[97mb\x1b[100mc\x1b[107md\x1b[0m\x1b[38;5;8me"
                  "\x1b[48;5;8mf\x1b[38;5;15mg\x1b[48;5;15mh");
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    // Index 15 is the same colour as 97, but the encoder writes the short
    // form (kills fg threshold < 15).
    CHECK(Contains(t.Bytes, "\x1b[0;97m"));
    CHECK(Contains(t.Bytes, "\x1b[0;90m"));
}

TEST_CASE("single underline is plain SGR 4, styles use the colon form") {
    // 4:1 and 4 parse to the same cell; the short form is the encoding.
    // Kills: AttrEx > 1 relaxed to >= 1.
    const Trip t = RoundTrip(12, 3, 0, "\x1b[4mx\x1b[0m\x1b[4:2my");
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    CHECK(Contains(t.Bytes, "\x1b[0;4m"));
    CHECK_FALSE(Contains(t.Bytes, ";4:1"));
    CHECK(Contains(t.Bytes, "\x1b[0;4:2m"));
}

TEST_CASE("256-colour step: cells that map to one index share one SGR") {
    // Two truecolor cells that differ only in the green channel fold to the
    // same palette index; the second must not emit another SGR.  Kills:
    // NormalizeCell leaving Fg[1] of the downgraded colour set.
    Term t(20, 3, 0);
    t.Feed("\x1b[38;2;255;0;0ma\x1b[38;2;255;1;0mb\x1b[38;2;255;2;0mc\x1b[0m");
    const std::size_t cap = LargestCapWith(*t.G, EDegradation::Colors256);
    auto r = Snapshot::Create(*t.G, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    const std::unique_ptr<Snapshot> owner(r.value());
    CHECK(owner->Degradation() == EDegradation::Colors256);
    CHECK(Count(View(*owner), "38;5;196") == 1);
}

// ---------------------------------------------------------------------------
// Pen bookkeeping between the sections.
// ---------------------------------------------------------------------------

TEST_CASE(
    "history lines that end in a background do not leak it into the "
    "scroll") {
    // LF with a coloured background scrolls in coloured blank rows, which the
    // next history line only partly overwrites.  Kills: the end-of-line reset
    // ignoring BgKind.  (UlIdx is not in this list on purpose: erase fill
    // carries only the background, so the reset is not observable for it.)
    std::string in;
    for (int i = 0; i < 8; ++i) in += "ab\x1b[44m \x1b[0m\r\n";
    in += "z";
    const Trip t = RoundTrip(10, 3, 50, in);
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    const Trip u = RoundTrip(10, 3, 50, "q\x1b[48;2;1;2;3m \x1b[0m\r\nz");
    CHECK_MESSAGE(u.Delta.empty(), u.Delta);
}

TEST_CASE("alt-screen paint starts from the default pen") {
    // The main-slot pen is bold when ?1049h is issued; the alt paint's first
    // cell (column 0) is bold too and must still carry its SGR.  Kills: the
    // pen not being reset after the alt-screen clear.
    const Trip t = RoundTrip(12, 3, 0, "\x1b[1mm\x1b[1;1H\x1b[?1049hx", "y");
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
}

TEST_CASE("live pen equal to the saved pen survives the mode restore") {
    // Kills: the pen not being reset before the live pen is set.
    const std::string_view inputs[] = {
        "\x1b[1m\x1b"
        "7",
        "\x1b[38;2;1;2;3m\x1b"
        "7",
        "\x1b[1;4m\x1b[41m\x1b"
        "7",
    };
    for (const std::string_view in : inputs) {
        INFO("input: " << in);
        const Trip t = RoundTrip(12, 3, 0, in,
                                 "x\x1b"
                                 "8y");
        CHECK_MESSAGE(t.Delta.empty(), t.Delta);
    }
}

// ---------------------------------------------------------------------------
// Saved-cursor slot state, tab stops, pending wrap.
// ---------------------------------------------------------------------------

TEST_CASE("a saved slot carrying one charset component round-trips") {
    // Each of G0, G1 and the active-set flag alone must make the slot need
    // state set-up.  Kills: SlotNeedsState ignoring G0, G1, CharsetActive.
    const std::string_view inputs[] = {
        "\x1b(0\x1b"
        "7\x1b(B",
        "\x1b)0\x1b"
        "7\x1b)B",
        "\x0e\x1b"
        "7\x0f",
    };
    for (const std::string_view in : inputs) {
        INFO("input: " << in);
        const Trip t = RoundTrip(12, 4, 0, in,
                                 "\x1b"
                                 "8qq");
        CHECK_MESSAGE(t.Delta.empty(), t.Delta);
        const Trip alt = RoundTrip(12, 4, 0, std::string(in) + "\x1b[?1049h",
                                   "\x1b"
                                   "8qq");
        CHECK_MESSAGE(alt.Delta.empty(), alt.Delta);
    }
}

TEST_CASE("a tab stop in the last column round-trips") {
    // Kills: the tab-stop loop stopping one column early.
    const Trip t = RoundTrip(20, 3, 0,
                             "\x1b[3g\x1b[1;20H\x1b"
                             "H\x1b[1;5H\x1b"
                             "H\x1b[1;1H",
                             "\t\tx");
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
}

TEST_CASE("pending wrap survives autowrap being switched off afterwards") {
    // Not a kill for the temporary AWM-on around the re-print: Grid sets the
    // wrap flag on the last column whether or not AWM is on, so that step only
    // matters to other terminals and the mutant is equivalent here.
    const Trip t = RoundTrip(10, 3, 0, "0123456789\x1b[?7l", "X");
    CHECK_MESSAGE(t.Delta.empty(), t.Delta);
}

TEST_CASE("pending wrap with a graphics charset switched on afterwards") {
    // The last cell holds ASCII 'q' (printed before the switch), so the
    // re-print under an active graphics set would turn it into a box glyph.
    // Kills: the G1 switch to ASCII around the re-print being dropped.
    const Trip g1 = RoundTrip(10, 3, 0, "012345678q\x1b)0\x0e", "q");
    CHECK_MESSAGE(g1.Delta.empty(), g1.Delta);
    const Trip g0 = RoundTrip(10, 3, 0, "012345678q\x1b(0", "q");
    CHECK_MESSAGE(g0.Delta.empty(), g0.Delta);
}

// ---------------------------------------------------------------------------
// Characters.
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 length boundaries round-trip") {
    // U+00E9, U+07FF, U+0800, U+FFFD, U+10000, U+100000, U+10FFFD.  Kills:
    // the 2/3/4-byte thresholds and lead-byte masks in WriteUtf8.
    const std::string in =
        "\xc3\xa9\xdf\xbf\xe0\xa0\x80\xef\xbf\xbd\xf0\x90\x80\x80\xf4\x80\x80"
        "\x80\xf4\x8f\xbf\xbd";
    Term src(12, 3, 4);
    src.Feed(in);
    REQUIRE(src.G->MainCellAt(0, 0).Codepoint == 0xE9);
    REQUIRE(src.G->MainCellAt(0, 4).Codepoint == 0x10000);
    REQUIRE(src.G->MainCellAt(0, 5).Codepoint == 0x100000);
    REQUIRE(src.G->MainCellAt(0, 6).Codepoint == 0x10FFFD);
    const Trip screen = RoundTrip(12, 3, 4, in);
    CHECK_MESSAGE(screen.Delta.empty(), screen.Delta);
    const Trip history = RoundTrip(12, 2, 4, in + "\r\n\r\nz");
    CHECK_MESSAGE(history.Delta.empty(), history.Delta);
}

TEST_CASE("a wide glyph stored in a narrow cell is written as U+FFFD") {
    // Kills: the orphan-head check missing in the history encoder and in the
    // pending-wrap re-print.
    Term hist(10, 3, 5);
    hist.G->PutChar(0x4E2D, 1);
    for (int i = 0; i < 5; ++i) hist.Feed("\r\n");
    REQUIRE(hist.G->ScrollbackSize() > 0);
    auto s = Snapshot::Create(*hist.G);
    REQUIRE(s.has_value());
    const std::unique_ptr<Snapshot> owner(s.value());
    CHECK_FALSE(Contains(View(*owner), "\xe4\xb8\xad"));
    Term dst(10, 3, 5);
    dst.P.Feed(owner->Bytes().data(), owner->Bytes().size(), *dst.G);
    REQUIRE(dst.G->ScrollbackSize() > 0);
    CHECK(dst.G->ScrollbackLine(0)->Cells[0].Codepoint == 0xFFFD);
    CHECK(dst.G->ScrollbackLine(0)->Cells[0].Width == 1);

    Term wrap(10, 3, 0);
    wrap.Feed("\x1b[1;10H");
    wrap.G->PutChar(0x4E2D, 1);
    REQUIRE(wrap.G->PendingWrap());
    auto w = Snapshot::Create(*wrap.G);
    REQUIRE(w.has_value());
    const std::unique_ptr<Snapshot> wOwner(w.value());
    Term wdst(10, 3, 0);
    wdst.P.Feed(wOwner->Bytes().data(), wOwner->Bytes().size(), *wdst.G);
    CHECK(wdst.G->CursorRow() == 0);
    CHECK(wdst.G->CursorCol() == 9);
    CHECK(wdst.G->PendingWrap());
    CHECK(wdst.G->MainCellAt(0, 9).Codepoint == 0xFFFD);
}

TEST_CASE("U+0080 never reaches the bytes raw") {
    // Kills: the C1 range check starting at U+0081.
    Term t(10, 3, 0);
    t.G->PutChar(0x80, 1);
    auto s = Snapshot::Create(*t.G);
    REQUIRE(s.has_value());
    const std::unique_ptr<Snapshot> owner(s.value());
    CHECK_FALSE(Contains(View(*owner), "\xc2\x80"));
}

// ---------------------------------------------------------------------------
// Ladder.
// ---------------------------------------------------------------------------

TEST_CASE("history trim keeps as many of the newest lines as fit") {
    // Old lines are heavy, new ones are light.  Measuring the oldest `mid`
    // lines instead of the newest would keep fewer than fit.  Kills: the trim
    // search measuring HistStart = 0.
    auto fill = [](Term& t, bool heavy) {
        if (heavy) {
            for (int i = 0; i < 4; ++i) {
                std::string line;
                for (int c = 0; c < 10; ++c)
                    line += "\x1b[38;2;" + std::to_string(10 + c) + ";" +
                            std::to_string(20 + i) + ";30mH";
                t.Feed(line + "\x1b[0m\r\n");
            }
        }
        for (int i = 0; i < 6; ++i) t.Feed("s\r\n");
        t.Feed("z");
    };
    Term both(10, 3, 50);
    fill(both, true);
    Term light(10, 3, 50);
    fill(light, false);
    const std::size_t cap = FullSize(*light.G);

    auto r = Snapshot::Create(*both.G, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    const std::unique_ptr<Snapshot> owner(r.value());
    CHECK(owner->Degradation() == EDegradation::HistoryTrimmed);
    Term dst(10, 3, 50);
    dst.P.Feed(owner->Bytes().data(), owner->Bytes().size(), *dst.G);
    REQUIRE(dst.G->ScrollbackSize() == 4);
    for (std::uint32_t i = 0; i < 4; ++i)
        CHECK(dst.G->ScrollbackLine(i)->Cells[0].Codepoint == U's');
}
