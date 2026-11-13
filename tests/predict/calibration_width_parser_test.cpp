// Width verdicts and the parser (PR #148): inputs that give a verdict the
// terminal contradicts, or rows the mirror calls known while the terminal
// shows something else. Where the model terminal of the rig (a grid with the
// mirror's own parser) cannot draw what a real one does, the answer is
// scripted and the comment names the terminal that gives it.
#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "calibration_rig.hpp"
#include "mirror.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

// "Ж" (class Cyrillic).
const std::string ZHE = "\xd0\x96";
const std::string RU = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
// U+E0B0, private use in the BMP (a Powerline glyph).
const std::string PL = "\xee\x82\xb0";
// U+F0001, private use in plane 15 (Nerd Fonts v3 moved the Material Design
// icons here, nf-md-*).
const std::string PL15 = "\xf3\xb0\x80\x81";

void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

void StartScripted(CalClient& t) {
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    REQUIRE_FALSE(t.Frozen());
}

/// Every cell of `row` the mirror calls known is the terminal's.
void KnownAgree(CalClient& t, std::uint16_t row) {
    const grid::Grid& m = *t.R().G;
    const grid::Grid& ref = *t.C.Ref.G;
    for (std::uint16_t c = m.KnownFrom(row); c < m.Cols(); ++c) {
        CAPTURE(c);
        const char32_t a = m.CellAt(row, c).Codepoint;
        const char32_t b = ref.CellAt(row, c).Codepoint;
        CHECK((a == b || ((a == 0 || a == U' ') && (b == 0 || b == U' '))));
    }
}

}  // namespace

TEST_CASE(
    "width parser: a snapshot after a Wide verdict leaves rows below a wrapped "
    "row "
    "known and wrong") {
    // The terminal draws Cyrillic in two cells and the session knows it. A
    // snapshot paints a row of 12 Cyrillic glyphs in 20 columns with AWM on:
    // the terminal wraps it into two rows and everything below moves down one
    // row. The mirror lays the snapshot out with the default policy (12
    // cells, no wrap) and applies the verdict afterwards: it forgets row 0
    // only, and keeps row 1 ("abc") known where the terminal has "ЖЖ".
    CalClient t(20, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + RU + "> ");
    REQUIRE(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    std::string body = "\x1b[2J\x1b[H";
    for (int i = 0; i < 12; ++i) body += ZHE;
    body += "\r\nabc\r\n$ ";
    t.Snapshot(body);
    REQUIRE(t.C.Ref.G->CellAt(2, 0).Codepoint == U'a');
    CHECK(t.R().G->CursorRow() == t.C.Real.G->CursorRow());
    KnownAgree(t, 1);
    KnownAgree(t, 2);
}

TEST_CASE(
    "width parser: a truncated UTF-8 character before ESC gives a false Wide") {
    // Output cut by bytes in the middle of a character, then an SGR reset.
    // xterm, VTE and alacritty draw U+FFFD for the lone lead byte; the mirror's
    // parser drops it when ESC arrives and keeps the path solvable. Narrow
    // terminal: Ж + U+FFFD put the cursor at 2, the mirror expects 1, d = 1 =
    // the number of Cyrillic glyphs.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + "\xd0\x1b[m");
    REQUIRE(t.Frozen());
    t.Scripted(0, 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Wide);
}

TEST_CASE(
    "width parser: a UTF-8 C1 control on the path gives a false verdict") {
    // VTE acts on C1 controls decoded from UTF-8; the mirror's parser drops
    // them and keeps the path solvable.
    SUBCASE("U+009B CSI on a narrow terminal: false Wide") {
        // Terminal: Ж (1) + CUF 3 = 4. Mirror: Ж + "3C" = 3. d = 1 = n.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + "\xc2\x9b" + "3C");
        REQUIRE(t.Frozen());
        t.Scripted(0, 4);
        CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Wide);
    }
    SUBCASE("U+009D OSC on a wide terminal: false Narrow") {
        // Terminal: Ж (2), "x" swallowed by the OSC that BEL ends = 2.
        // Mirror: Ж + "x" = 2. d = 0.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + "\xc2\x9d" + "x\x07");
        REQUIRE(t.Frozen());
        t.Scripted(0, 2);
        CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Narrow);
    }
}

TEST_CASE(
    "width parser: CSI with an intermediate is taken for CUF: false Narrow") {
    // `CSI 1 SP C` (GSS in ECMA-48) is unknown to xterm, which ignores it. The
    // mirror's dispatch ignores the intermediate and moves the cursor right,
    // so the sequence neither breaks the path nor matches the terminal. xterm
    // with cjkWidth: Ж (2) + "x" = 3. Mirror: Ж (1) + CUF 1 + "x" = 3. d = 0.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + "\x1b[1 Cx");
    REQUIRE(t.Frozen());
    t.Scripted(0, 3);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Narrow);
}

TEST_CASE("width parser: DECLRMM leaves no path solvable and sends no query") {
    // xterm: DECSET 69 enables left/right margins, DECSLRM sets the left one
    // to column 2, CR then goes to column 2 (index 1). The mirror does not
    // model margins: while the mode is set it asks nothing, and a verdict
    // from this path would be false (Narrow xterm: 1 + Ж + "x" = 3, the mirror
    // says 2, d = 1 = the number of Cyrillic glyphs).
    const std::string set = "\x1b[?69h\x1b[2;40s\x1b[1;10H\r" + ZHE + "x";
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent(set);
    CHECK_FALSE(t.Frozen());
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
    CHECK(t.R().G->UnmodelledModeSet(grid::EUnmodelledMode::LeftRightMargins));
    SUBCASE("CSI ? 69 l ends it") {
        t.Agent("\x1b[?69l");
        CHECK_FALSE(t.R().G->AnyUnmodelledMode());
        CHECK(t.Queries() == 2);
    }
    SUBCASE("RIS ends it") {
        t.Agent(
            "\x1b"
            "c");
        CHECK_FALSE(t.R().G->AnyUnmodelledMode());
        CHECK(t.Queries() == 2);
    }
    SUBCASE("DECSTR ends it") {
        t.Agent("\x1b[!p");
        CHECK_FALSE(t.R().G->AnyUnmodelledMode());
        CHECK(t.Queries() == 2);
    }
}

TEST_CASE(
    "width parser: nothing is predicted while a mode the mirror lacks is set") {
    // Reverse wraparound and left/right margins move the cursor where the
    // mirror does not look: a prediction would be drawn in a column that is
    // a guess.
    for (const char* mode : {"\x1b[?45h", "\x1b[?69h", "\x1b[?3h"}) {
        CAPTURE(mode);
        CalClient t(40, 6);
        t.Before("$ ");
        t.Begin();
        t.Key("a");
        t.Agent("a");
        t.Ack();
        t.Key("b");
        t.Agent("b");
        t.Ack();
        t.Agent(mode);
        t.Key("c");
        CHECK(t.R().Count() == 0);
    }
}

TEST_CASE("width parser: the modes the grid does not model are remembered") {
    struct Mode {
        const char* Set;
        const char* Reset;
        grid::EUnmodelledMode Which;
    };
    constexpr std::array MODES{
        Mode{"\x1b[?3h", "\x1b[?3l", grid::EUnmodelledMode::ColumnMode},
        Mode{"\x1b[?45h", "\x1b[?45l", grid::EUnmodelledMode::ReverseWrap},
        Mode{"\x1b[?69h", "\x1b[?69l",
             grid::EUnmodelledMode::LeftRightMargins}};
    for (const Mode& m : MODES) {
        CAPTURE(m.Set);
        for (const char* end : {m.Reset,
                                "\x1b"
                                "c",
                                "\x1b[!p"}) {
            CAPTURE(end);
            auto made = grid::Grid::Create(20, 5, 0);
            REQUIRE(made.has_value());
            std::unique_ptr<grid::Grid> g(made.value());
            grid::VtParser p;
            const auto feed = [&](std::string_view s) {
                p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(),
                       *g);
            };
            const std::uint64_t before = p.UnknownSeqCount();
            feed(m.Set);
            CHECK(g->UnmodelledModeSet(m.Which));
            // Setting it is a sequence the mirror does not know.
            CHECK(p.UnknownSeqCount() == before + 1);
            CHECK(g->PathBroken());
            feed(end);
            CHECK_FALSE(g->AnyUnmodelledMode());
        }
    }
    // The snapshot does not carry them: they are no part of Modes().
    auto made = grid::Grid::Create(20, 5, 0);
    REQUIRE(made.has_value());
    std::unique_ptr<grid::Grid> g(made.value());
    const std::uint32_t modes = g->Modes();
    g->SetMode(69, true);
    CHECK(g->Modes() == modes);
}

TEST_CASE("width parser: DECSLRM is not taken for SCP") {
    auto made = grid::Grid::Create(20, 5, 0);
    REQUIRE(made.has_value());
    std::unique_ptr<grid::Grid> g(made.value());
    grid::VtParser p;
    const auto feed = [&](std::string_view s) {
        p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *g);
    };
    feed("\x1b[3;3H\x1b[2;10s\x1b[5;5H\x1b[u");
    // Nothing was saved: the restore goes to the default, the home.
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    CHECK(p.UnknownSeqCount() == 1);
    // The bare form is SCP.
    feed("\x1b[3;3H\x1b[s\x1b[5;5H\x1b[u");
    CHECK(g->CursorRow() == 2);
    CHECK(g->CursorCol() == 2);
    CHECK(p.UnknownSeqCount() == 1);
}

TEST_CASE(
    "width parser: a Wide verdict for BMP private use is taken for plane 15 "
    "too") {
    // wezterm with cell_widths = {{first = 0xe000, last = 0xf8ff, width = 2}}
    // (or tmux codepoint-widths for the same range): BMP private use is two
    // cells, plane 15 stays one. The verdict from a BMP glyph closes the
    // whole class: the mirror lays U+F0001 out in two cells and holds the
    // column after it as certain.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + PL + "> ");
    REQUIRE(t.Frozen());
    t.Scripted(0, 4);
    REQUIRE(t.Cal->Verdict(EWidthClass::PrivateUseBmp) == EWidthVerdict::Wide);
    t.Agent("\r\n" + PL15 + "x");
    const grid::Grid& g = *t.R().G;
    // The terminal's cursor is at column 2; the mirror's at 3. The column may
    // be wrong, but it must not be certain.
    CHECK(g.WidthDoubtBefore(g.CursorRow(), g.CursorCol()));
}

// ---------------------------------------------------------------------------
// The parser: malformed UTF-8, C1 controls, forms the mirror does not handle
// ---------------------------------------------------------------------------

namespace {

struct Parsed {
    std::unique_ptr<grid::Grid> G;
    grid::VtParser P;

    Parsed() {
        auto made = grid::Grid::Create(40, 10, 0);
        REQUIRE(made.has_value());
        G.reset(made.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
};

}  // namespace

TEST_CASE(
    "width parser: malformed UTF-8 is an unknown sequence, drawn as before") {
    struct Case {
        const char* Name;
        std::string Bytes;
        std::uint16_t Col;  // the cursor after "ab" and the bytes
    };
    const std::array<Case, 20> CASES{{
        {"a lead cut off by ESC", "\xd0\x1b[m", 2},
        {"a lead cut off by CAN", "\xd0\x18", 2},
        {"a lead cut off by SUB", "\xd0\x1a", 3},
        {"a lead cut off by LF", "\xe2\x82\n", 3},
        {"a lead cut off by a lead", "\xd0\xd0\x96", 4},
        {"a stray continuation", "\x80", 2},
        {"a stray continuation, 0xBF", "\xbf", 2},
        {"overlong C0 80", "\xc0\x80", 3},
        {"overlong C1 BF", "\xc1\xbf", 3},
        {"overlong E0 80 80", "\xe0\x80\x80", 3},
        {"overlong F0 80 80 80", "\xf0\x80\x80\x80", 3},
        {"a surrogate", "\xed\xa0\x80", 3},
        {"above U+10FFFF", "\xf4\x90\x80\x80", 3},
        {"F5", "\xf5\x80\x80\x80", 3},
        {"F8", "\xf8", 3},
        {"FE", "\xfe", 3},
        {"FF", "\xff", 3},
        {"a byte above 0x7F in a CSI",
         "\x1b[1\xd0\x96"
         "C",
         3},
        {"C1 U+0080", "\xc2\x80", 2},
        {"C1 U+009F", "\xc2\x9f", 2},
    }};
    for (const Case& c : CASES) {
        INFO(std::string(c.Name));
        Parsed t;
        t.Feed("ab");
        // A path of its own: the cursor's column is the first thing to ask.
        REQUIRE_FALSE(t.G->PathBroken());
        const std::uint64_t before = t.P.UnknownSeqCount();
        t.Feed(c.Bytes);
        CHECK(t.P.UnknownSeqCount() > before);
        CHECK(t.G->PathBroken());
        // What the mirror draws is what it drew before.
        CHECK(t.G->CursorCol() == c.Col);
    }
    SUBCASE("well-formed text is not one") {
        for (const char* ok :
             {"\xd0\x96", "\xc2\xa0", "\xe2\x82\xac", "\xf0\x9f\x9a\x80",
              "\xf4\x8f\xbf\xbf", "\xc2\xa0", "\xed\x9f\xbf"}) {
            Parsed t;
            t.Feed("ab");
            t.Feed(ok);
            CAPTURE(ok);
            CHECK(t.P.UnknownSeqCount() == 0);
        }
    }
}

TEST_CASE("width parser: CSI forms the mirror does not handle are unknown") {
    // Every final that moves the cursor, with each intermediate and each
    // private marker: none may reach the handler of the plain final.
    constexpr std::string_view FINALS = "ABCDEFGHIZadefbSTLMP@XJK";
    const std::array<const char*, 12> FORMS{"1 ", "1!", "1\"", "1$",
                                            "1'", "1*", "1 !", "?1",
                                            ">1", "<1", "=1",  "1:2"};
    for (const char final : FINALS) {
        for (const char* form : FORMS) {
            // "1:2" is a colon among the parameters, not a marker; it is
            // illegal for all of them but SGR.
            const std::string seq =
                std::string("\x1b[") + form + std::string(1, final);
            CAPTURE(seq);
            Parsed t;
            t.Feed("x\x1b[5;11H");
            const std::uint16_t row = t.G->CursorRow();
            const std::uint16_t col = t.G->CursorCol();
            const std::uint64_t cells = t.G->CellChangeCount();
            REQUIRE_FALSE(t.G->PathBroken());
            const std::uint64_t before = t.P.UnknownSeqCount();
            t.Feed(seq);
            CHECK(t.P.UnknownSeqCount() == before + 1);
            CHECK(t.G->PathBroken());
            CHECK(t.G->CursorRow() == row);
            CHECK(t.G->CursorCol() == col);
            CHECK(t.G->CellChangeCount() == cells);
        }
    }
    SUBCASE("a marker among the parameters") {
        for (const char* seq : {"\x1b[1>C", "\x1b[1?C", "\x1b[1;2<H",
                                "\x1b[1=A", "\x1b[?1;2>h", "\x1b[1?m"}) {
            CAPTURE(seq);
            Parsed t;
            t.Feed("x\x1b[5;11H");
            const std::uint16_t col = t.G->CursorCol();
            const std::uint64_t before = t.P.UnknownSeqCount();
            t.Feed(seq);
            CHECK(t.P.UnknownSeqCount() == before + 1);
            CHECK(t.G->CursorCol() == col);
            CHECK(t.G->PathBroken());
        }
    }
    SUBCASE("the plain forms are handled, and move the cursor") {
        for (const char final : std::string_view("ABCDEFGHIZadef")) {
            const std::string seq = std::string("\x1b[2") + final;
            CAPTURE(seq);
            Parsed t;
            t.Feed("x\x1b[5;11H");
            const std::uint16_t row = t.G->CursorRow();
            const std::uint16_t col = t.G->CursorCol();
            const std::uint64_t before = t.P.UnknownSeqCount();
            t.Feed(seq);
            CHECK(t.P.UnknownSeqCount() == before);
            CHECK((t.G->CursorRow() != row || t.G->CursorCol() != col));
        }
    }
    SUBCASE("the forms the mirror does handle") {
        Parsed t;
        t.Feed("x\x1b[ q\x1b[2 q\x1b[?7h\x1b[?25l\x1b[1;5r\x1b[1m\x1b[1:2m");
        CHECK(t.P.UnknownSeqCount() == 0);
        t.Feed("\x1b[!p");
        CHECK(t.P.UnknownSeqCount() == 0);
    }
}

TEST_CASE("width parser: the C1 control of a UTF-8 text is not executed") {
    // VTE takes U+009B for CSI and U+009D for OSC; the mirror does neither,
    // the bytes that follow are text to it.
    Parsed t;
    t.Feed(
        "\xc2\x9b"
        "3C");
    CHECK(t.G->CursorCol() == 2);
    CHECK(t.G->CellAt(0, 0).Codepoint == U'3');
    CHECK(t.P.UnknownSeqCount() == 1);
    Parsed osc;
    osc.Feed(
        "\xc2\x9d"
        "x\x07");
    CHECK(osc.G->CellAt(0, 0).Codepoint == U'x');
    CHECK(osc.P.UnknownSeqCount() == 1);
}

TEST_CASE("width parser: REP after a zero-width code point breaks the path") {
    // The terminal may repeat the glyph with what came after it. A new path
    // starts at the CR, but the code point stays the last one printed.
    SUBCASE("the grid") {
        Parsed t;
        t.Feed(
            "\r"
            "x\xe2\x80\x8d\r");
        REQUIRE_FALSE(t.G->PathBroken());
        t.Feed("\x1b[2b");
        CHECK(t.G->PathBroken());
        // After a plain glyph REP is read.
        Parsed plain;
        plain.Feed(
            "\r"
            "x\r\x1b[2b");
        CHECK_FALSE(plain.G->PathBroken());
    }
    SUBCASE("no width query follows") {
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent(
            "\r"
            "x\xe2\x80\x8d\r\x1b[2b" +
            ZHE);
        CHECK_FALSE(t.Frozen());
        CHECK(t.Queries() == 1);
    }
}

// ---------------------------------------------------------------------------
// A snapshot after a verdict
// ---------------------------------------------------------------------------

TEST_CASE("width parser: the mirror lays a snapshot out under the verdicts") {
    std::string body = "\x1b[2J\x1b[H";
    for (int i = 0; i < 12; ++i) body += ZHE;
    body += "\r\nabc\r\n$ ";
    const auto bytes = [&] {
        return ByteSpan{reinterpret_cast<const std::byte*>(body.data()),
                        body.size()};
    };
    auto mirror = client::Mirror::Create(20, 6);
    REQUIRE(mirror != nullptr);
    SUBCASE("the default policy: one row, abc below it") {
        REQUIRE(mirror->ResetFromSnapshot(bytes(), {}));
        CHECK(mirror->Screen().CellAt(1, 0).Codepoint == U'a');
    }
    SUBCASE("Cyrillic wide: the row wraps and abc is a row lower") {
        grid::WidthPolicy policy;
        policy.Cyrillic = 2;
        REQUIRE(mirror->ResetFromSnapshot(bytes(), policy));
        CHECK(mirror->Screen().Policy() == policy);
        CHECK(mirror->Screen().CellAt(2, 0).Codepoint == U'a');
        CHECK(mirror->Screen().CursorRow() == 3);
    }
    SUBCASE("a resize keeps the policy") {
        grid::WidthPolicy policy;
        policy.PrivateUsePlanes = 2;
        REQUIRE(mirror->ResetFromSnapshot(bytes(), policy));
        REQUIRE(mirror->Resize(30, 8));
        CHECK(mirror->Screen().Policy() == policy);
    }
}

// ---------------------------------------------------------------------------
// Private use apart: BMP and planes 15-16
// ---------------------------------------------------------------------------

TEST_CASE(
    "width parser: the two private use classes have verdicts of their own") {
    // wezterm cell_widths for U+E000-F8FF only: BMP two cells, plane 15 one.
    const auto first = [](CalClient& t, const std::string& glyph) {
        t.Agent("\r" + glyph + "> ");
    };
    SUBCASE("BMP wide, then plane 15 narrow") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 2, 1, false));
        Start(t);
        first(t, PL);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Wide);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUsePlanes) ==
              EWidthVerdict::Unknown);
        first(t, PL15);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUsePlanes) ==
              EWidthVerdict::Narrow);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Wide);
        const grid::WidthPolicy policy = t.Cal->Policy();
        CHECK(policy.PrivateUseBmp == 2);
        CHECK(policy.PrivateUsePlanes == 1);
        // Both known: no query, and the mirror is where the terminal is.
        const std::size_t queries = t.Queries();
        t.Agent("\r" + PL15 + PL + "x");
        CHECK(t.Queries() == queries);
        CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
        CHECK_FALSE(t.R().G->WidthDoubtBefore(t.R().G->CursorRow(),
                                              t.R().G->CursorCol()));
    }
    SUBCASE("plane 15 wide, then BMP narrow") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 1, 2, false));
        Start(t);
        first(t, PL15);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUsePlanes) ==
              EWidthVerdict::Wide);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Unknown);
        first(t, PL);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Narrow);
        CHECK(t.Cal->Policy().PrivateUsePlanes == 2);
        CHECK(t.Cal->Policy().PrivateUseBmp == 1);
    }
    SUBCASE("d = 0 closes both classes on the path as Narrow") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 1, 1, false));
        Start(t);
        t.Agent("\r" + PL + PL15 + "> ");
        CHECK(t.Queries() == 2);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Narrow);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUsePlanes) ==
              EWidthVerdict::Narrow);
    }
    SUBCASE("two open classes on one path give no Wide") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 2, 1, false));
        Start(t);
        t.Agent("\r" + PL + PL15 + "> ");
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
              EWidthVerdict::Unknown);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUsePlanes) ==
              EWidthVerdict::Unknown);
    }
}

TEST_CASE("width parser: the policy sizes the private use classes apart") {
    auto made = grid::Grid::Create(20, 3, 0);
    REQUIRE(made.has_value());
    std::unique_ptr<grid::Grid> g(made.value());
    grid::WidthPolicy policy;
    policy.PrivateUseBmp = 2;
    g->SetWidthPolicy(policy);
    grid::VtParser p;
    const std::string s = PL + PL15;
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *g);
    // BMP two cells, plane 15 one.
    CHECK(g->CursorCol() == 3);
    CHECK(g->CellAt(0, 0).Width == 2);
    CHECK(g->CellAt(0, 2).Width == 1);
    CHECK(g->PathGlyphs(EWidthClass::PrivateUseBmp) == 1);
    CHECK(g->PathGlyphs(EWidthClass::PrivateUsePlanes) == 1);
    // Each class has its own mark bit.
    CHECK(g->WidthDoubtClasses(0) ==
          (grid::WidthClassBit(EWidthClass::PrivateUseBmp) |
           grid::WidthClassBit(EWidthClass::PrivateUsePlanes)));
}
