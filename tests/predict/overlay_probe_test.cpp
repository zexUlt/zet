// The width probe's write (design.md §9, round C, step C4c): the gates of a
// clean point, the bytes, the glyphs. The terminal is a second grid ("Real",
// tests/predict/overlay_rig.hpp), fed the bytes the client would queue; the
// mirror is never fed the probe, and after the write the terminal must equal
// the one that never had it ("Ref"): cells, backgrounds, cursor, pen and the
// glyph REP repeats.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "overlay_rig.hpp"
#include "predict_rig.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/predict/calibration.hpp"
#include "zet/predict/probe.hpp"

using namespace zet::predict;
using namespace zet::predict::test;
using zet::grid::EWidthClass;

namespace {

std::string Str(zet::ByteSpan b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::string Utf8(char32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

std::size_t Count(const std::string& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i = hay.find(needle, i + needle.size())) {
        ++n;
    }
    return n;
}

constexpr char32_t PUA_BMP_A = U'\uE0B0';
constexpr char32_t PUA_BMP_B = U'\uE702';
constexpr char32_t PUA_BMP_C = U'\uEA60';
constexpr char32_t PUA_BMP_D = U'\uF015';
constexpr char32_t PUA_BMP_EXTRA = U'\uE000';
constexpr char32_t PUA_PLANE_A = U'\U000F024B';
constexpr char32_t PUA_PLANE_B = U'\U000F1AF0';
constexpr char32_t CYR_ZHE = U'Ж';
constexpr char32_t CYR_YO_LOWER = U'ё';

/// Whether both terminals have the same background colours, which SameScreen
/// compares by kind only.
bool SameBackgrounds(const zet::grid::Grid& a, const zet::grid::Grid& b) {
    for (std::uint16_t r = 0; r < a.Rows(); ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const auto& x = a.CellAt(r, c);
            const auto& y = b.CellAt(r, c);
            if (x.BgKind != y.BgKind) return false;
            if (x.BgKind != zet::grid::EColorKind::Default &&
                (x.Bg[0] != y.Bg[0] || x.Bg[1] != y.Bg[1] ||
                 x.Bg[2] != y.Bg[2])) {
                return false;
            }
        }
    }
    return true;
}

struct ProbeRun {
    WidthProbeWrite Write;
    std::string Bytes;
    /// The cursor of the terminal where the query is written: the answer.
    std::uint16_t AnswerRow{0};
    std::uint16_t AnswerCol{0};
};

/// The probe at the mirror's cursor, written to the terminal in the client's
/// order: the overlay is off, the bytes up to the query, the answer read where
/// the terminal's cursor stands, the rest.
ProbeRun Probe(Client& c, WidthProbe& p, EWidthClass cls,
               char32_t encountered = 0) {
    c.Undo();
    const auto w = p.Build(c.R.E, c.R.View(), cls, encountered);
    REQUIRE(w.has_value());
    ProbeRun run;
    run.Write = *w;
    run.Bytes = Str(w->Bytes);
    REQUIRE(w->QueryOffset + zet::predict::Calibration::QUERY_SIZE <=
            w->Bytes.size());
    c.Real.Feed(w->Bytes.first(w->QueryOffset));
    run.AnswerRow = c.Real.G->CursorRow();
    run.AnswerCol = c.Real.G->CursorCol();
    c.Real.Feed(w->Bytes.subspan(w->QueryOffset));
    return run;
}

/// What the probe must leave: the terminal is the one without it, and REP
/// repeats the program's glyph, on both.
void CheckRestored(Client& c) {
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
    CHECK(SameBackgrounds(*c.Real.G, *c.Ref.G));
    c.Real.Feed("\x1b[3b");
    c.Ref.Feed("\x1b[3b");
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
}

struct Env {
    Client C{40, 5, EDisplayMode::Always};
    std::unique_ptr<WidthProbe> P = WidthProbe::Create();

    Env() {
        REQUIRE(P != nullptr);
        // The terminal and the mirror both start on row 0, and a resync is
        // what makes the rows the probe's to trust.
        REQUIRE(C.R.G->Resync(0, 0, true));
        C.Start();
    }
};

/// A mirror alone, at a clean point: resynced on row 0 (a fresh mirror's rows
/// are the terminal's only by luck), the prompt and two echoed keys.
struct Mirror {
    Rig R;
    explicit Mirror(int cols = 40, int rows = 5,
                    EDisplayMode mode = EDisplayMode::Always,
                    bool resynced = true)
        : R(cols, rows, mode) {
        if (resynced) REQUIRE(R.G->Resync(0, 0, true));
        R.Prompt();
        R.ConfirmEpoch();
    }
    EProbeGate Gate(EWidthClass cls = EWidthClass::Cyrillic, char32_t enc = 0) {
        return WidthProbe::Check(R.E, R.View(), cls, enc);
    }
};

}  // namespace

TEST_CASE("the glyphs are the representatives and then the encountered point") {
    std::array<char32_t, PROBE_GLYPHS_MAX> g{};

    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, 0, g) == 2);
    CHECK(g[0] == CYR_ZHE);
    CHECK(g[1] == CYR_YO_LOWER);

    CHECK(WidthProbe::Glyphs(EWidthClass::PrivateUseBmp, 0, g) == 4);
    CHECK(g[0] == PUA_BMP_A);
    CHECK(g[1] == PUA_BMP_B);
    CHECK(g[2] == PUA_BMP_C);
    CHECK(g[3] == PUA_BMP_D);

    CHECK(WidthProbe::Glyphs(EWidthClass::PrivateUsePlanes, 0, g) == 2);
    CHECK(g[0] == PUA_PLANE_A);
    CHECK(g[1] == PUA_PLANE_B);

    // After the representatives, never before them.
    REQUIRE(WidthProbe::Glyphs(EWidthClass::Cyrillic, U'А', g) == 3);
    CHECK(g[0] == CYR_ZHE);
    CHECK(g[1] == CYR_YO_LOWER);
    CHECK(g[2] == U'А');
    REQUIRE(WidthProbe::Glyphs(EWidthClass::PrivateUseBmp, PUA_BMP_EXTRA, g) ==
            5);
    CHECK(g[4] == PUA_BMP_EXTRA);
    // Plane 16 has no representative of its own: its point is measured as the
    // encountered one.
    REQUIRE(WidthProbe::Glyphs(EWidthClass::PrivateUsePlanes, U'\U0010FFFD',
                               g) == 3);
    CHECK(g[2] == U'\U0010FFFD');
}

TEST_CASE("the encountered point is not repeated when it is a representative") {
    std::array<char32_t, PROBE_GLYPHS_MAX> g{};
    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, CYR_YO_LOWER, g) == 2);
    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, CYR_ZHE, g) == 2);
    CHECK(WidthProbe::Glyphs(EWidthClass::PrivateUseBmp, PUA_BMP_C, g) == 4);
    CHECK(WidthProbe::Glyphs(EWidthClass::PrivateUsePlanes, PUA_PLANE_B, g) ==
          2);
}

TEST_CASE(
    "a point of another class is not added, and other classes have no "
    "probe") {
    std::array<char32_t, PROBE_GLYPHS_MAX> g{};
    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, U'a', g) == 2);
    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, PUA_BMP_A, g) == 2);
    CHECK(WidthProbe::Glyphs(EWidthClass::PrivateUseBmp, U'А', g) == 4);
    // U+0400 is not in the class: only Ё, А–я and ё are ambiguous.
    CHECK(WidthProbe::Glyphs(EWidthClass::Cyrillic, U'Ѐ', g) == 2);
    for (const EWidthClass cls :
         {EWidthClass::Ambiguous, EWidthClass::Emoji, EWidthClass::Cluster}) {
        CHECK(WidthProbe::Glyphs(cls, U'А', g) == 0);
        Mirror m;
        CHECK(m.Gate(cls) == EProbeGate::ClassNotProbed);
    }
}

TEST_CASE(
    "the probe is the frame, hidden glyphs, the query, and the restore "
    "after it") {
    Env e;
    e.C.Undo();
    const auto w = e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic, 0);
    REQUIRE(w.has_value());
    const std::string b = Str(w->Bytes);
    const std::string glyphs = Utf8(CYR_ZHE) + Utf8(CYR_YO_LOWER);

    CHECK(w->Row == 0);
    CHECK(w->Col == 4);
    CHECK(w->Glyphs == 2);
    // A frame and a hidden cursor first; the start is set absolutely.
    CHECK(b.starts_with("\x1b[?2026h\x1b[?25l\x1b[5G\x1b[0;8m"));
    // The glyphs, hidden, and the query right behind them, as one piece.
    REQUIRE(w->QueryOffset >= glyphs.size());
    CHECK(b.substr(w->QueryOffset - glyphs.size(), glyphs.size()) == glyphs);
    CHECK(b.substr(w->QueryOffset, 7) == "\x1b[6n\x1b[c");
    CHECK(b.substr(w->QueryOffset, Calibration::QUERY_SIZE) ==
          Str(Calibration::Query()));
    // The query exactly once: the one that is marked.
    CHECK(Count(b, "\x1b[6n") == 1);
    CHECK(Count(b, "\x1b[c") == 1);
    // Before the query nothing is taken off; after it, every cell of the probe
    // is, with ECH, and the register is given its glyph first.
    const std::string before = b.substr(0, w->QueryOffset);
    const std::string after = b.substr(w->QueryOffset + 7);
    CHECK(Count(before, "\x1b[X") == 0);
    CHECK(Count(after, "\x1b[X") == 4);
    CHECK(after.starts_with("\x1b[5GB"));
    CHECK(after.find("\x1b[X") > after.find("B"));
    // The frame closes last, the cursor is shown again (DECTCEM is on).
    CHECK(b.ends_with("\x1b[?25h\x1b[?2026l"));
    CHECK(Count(b, "\x1b[?2026l") == 1);
    CHECK(Count(b, "\x1b[?2026h") == 1);
    // The cursor goes back to the column the probe began from.
    CHECK(after.find("\x1b[5G", after.rfind("\x1b[X")) != std::string::npos);
}

TEST_CASE("the cursor is shown again only when the mirror shows it") {
    Env e;
    e.C.Agent("\x1b[?25l");
    e.C.Undo();
    const auto w = e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic, 0);
    REQUIRE(w.has_value());
    const std::string b = Str(w->Bytes);
    CHECK(Count(b, "\x1b[?25h") == 0);
    CHECK(b.ends_with("\x1b[?2026l"));
}

TEST_CASE(
    "the glyphs are printed hidden, on the background of the first "
    "cell") {
    Env e;
    e.C.Agent("\x1b[44m\x1b[K\x1b[0m");  // cells from column 4: blue, empty
    e.C.Undo();
    const auto w = e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic, 0);
    REQUIRE(w.has_value());
    CHECK(Str(w->Bytes).starts_with(
        "\x1b[?2026h\x1b[?25l\x1b[5G\x1b[0;8m\x1b[44m"));

    // Fed up to the query, the terminal holds the glyphs, hidden, in the
    // cells of the probe.
    e.C.Real.Feed(w->Bytes.first(w->QueryOffset));
    const auto& g = *e.C.Real.G;
    CHECK(g.CellAt(0, 4).Codepoint == CYR_ZHE);
    CHECK(g.CellAt(0, 5).Codepoint == CYR_YO_LOWER);
    for (std::uint16_t col = 4; col < 6; ++col) {
        CHECK((g.CellAt(0, col).Attrs & zet::grid::Invisible) != 0);
        CHECK(g.CellAt(0, col).BgKind == zet::grid::EColorKind::Indexed);
        CHECK(g.CellAt(0, col).Bg[0] == 4);
    }
}

TEST_CASE(
    "the answer is the sum of the widths of the glyphs: narrow and "
    "wide") {
    for (const bool wide : {false, true}) {
        CAPTURE(wide);
        struct Case {
            EWidthClass Cls;
            char32_t Encountered;
            int Glyphs;
        };
        for (const Case& t :
             {Case{EWidthClass::Cyrillic, 0, 2},
              Case{EWidthClass::Cyrillic, U'А', 3},
              Case{EWidthClass::PrivateUseBmp, 0, 4},
              Case{EWidthClass::PrivateUseBmp, PUA_BMP_EXTRA, 5},
              Case{EWidthClass::PrivateUsePlanes, 0, 2},
              Case{EWidthClass::PrivateUsePlanes, U'\U0010FFFD', 3}}) {
            CAPTURE(static_cast<int>(t.Cls));
            Env e;
            zet::grid::WidthPolicy policy;
            if (wide) {
                policy.Cyrillic = 2;
                policy.PrivateUseBmp = 2;
                policy.PrivateUsePlanes = 2;
            }
            e.C.Real.G->SetWidthPolicy(policy);
            const ProbeRun run = Probe(e.C, *e.P, t.Cls, t.Encountered);
            CHECK(run.Write.Glyphs == t.Glyphs);
            CHECK(run.AnswerRow == run.Write.Row);
            // The answer minus the column the probe began from.
            CHECK(run.AnswerCol - run.Write.Col ==
                  (wide ? 2 : 1) * run.Write.Glyphs);
            CheckRestored(e.C);
        }
    }
}

TEST_CASE("after the probe the terminal is the one without it") {
    SUBCASE("plain prompt") {
        Env e;
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CheckRestored(e.C);
    }
    SUBCASE("each class, with its encountered point") {
        for (const auto& [cls, enc] :
             {std::pair{EWidthClass::Cyrillic, U'А'},
              std::pair{EWidthClass::PrivateUseBmp, PUA_BMP_EXTRA},
              std::pair{EWidthClass::PrivateUsePlanes, U'\U0010FFFD'}}) {
            Env e;
            (void)Probe(e.C, *e.P, cls, enc);
            CheckRestored(e.C);
        }
    }
    SUBCASE("the pen of the program, truecolor and attributes, is back") {
        Env e;
        e.C.Agent("\x1b[1;3;4;38;2;1;2;3;48;2;9;8;7mX");
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CheckRestored(e.C);
        CHECK(e.C.Real.G->PenAttrs() == e.C.Ref.G->PenAttrs());
        CHECK(e.C.Real.G->PenFg()[0] == 1);
        CHECK(e.C.Real.G->PenBg()[2] == 7);
    }
    SUBCASE("cells with a background of their own are put back with it") {
        Env e;
        // Four cells from the cursor, each empty and each another colour;
        // erased, not printed: the glyph would be a different thing.
        e.C.Agent(
            "\x1b[41m\x1b[X\x1b[6G\x1b[42m\x1b[X\x1b[7G\x1b[43m\x1b[X"
            "\x1b[8G\x1b[48;5;200m\x1b[X\x1b[0m\x1b[5G");
        (void)Probe(e.C, *e.P, EWidthClass::PrivateUseBmp);
        // Before CheckRestored: its REP prints over the cells.
        const auto& g = *e.C.Real.G;
        CHECK(g.CellAt(0, 4).Bg[0] == 1);
        CHECK(g.CellAt(0, 5).Bg[0] == 2);
        CHECK(g.CellAt(0, 6).Bg[0] == 3);
        CHECK(g.CellAt(0, 7).Bg[0] == 200);
        CHECK(g.CellAt(0, 8).BgKind == zet::grid::EColorKind::Default);
        CheckRestored(e.C);
    }
    SUBCASE("a program glyph that is not ASCII, printed in another row") {
        Env e;
        // U+00E9 marks only its own row; the cursor goes to the row below.
        e.C.Agent("\r\n\xC3\xA9\r\n$ ");
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CHECK(e.C.Real.G->LastPrinted().Codepoint == U' ');
        CheckRestored(e.C);
    }
    SUBCASE("the register holds a wide glyph of the program") {
        Env e;
        e.C.Agent("\r\n\xE4\xB8\xAD\r\n\x1b[3G");  // 中 one row up
        REQUIRE(WidthProbe::Check(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic,
                                  0) == EProbeGate::Clean);
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CHECK(e.C.Real.G->LastPrinted().Codepoint == U'\u4E2D');
        CheckRestored(e.C);
    }
    SUBCASE("the cursor is on the first column") {
        Env e;
        e.C.Agent("\r\n\r");
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CheckRestored(e.C);
    }
    SUBCASE("the program's frame is closed and the mirror shows the cursor") {
        Env e;
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CHECK(e.C.Real.G->ModeSet(zet::grid::EMode::Dectcem));
        CHECK_FALSE(e.C.Real.G->ModeSet(zet::grid::EMode::SyncOutput));
    }
    SUBCASE("the mirror is not fed the probe") {
        Env e;
        const auto cursor = e.C.R.G->CursorCol();
        const auto last = e.C.R.G->LastPrinted().Codepoint;
        (void)Probe(e.C, *e.P, EWidthClass::Cyrillic);
        CHECK(e.C.R.G->CursorCol() == cursor);
        CHECK(e.C.R.G->LastPrinted().Codepoint == last);
        CHECK(e.C.R.G->CellAt(0, 4).Codepoint == 0);
    }
}

TEST_CASE("the bytes of the largest probe fit their bound") {
    Env e;
    e.C.Agent(
        "\x1b[48;2;200;201;202m\x1b[K\x1b[1;2;3;4;5;7;9;38;2;250;251;252;"
        "48;2;253;254;255m\x1b[4:3;58;5;200m X");
    e.C.Undo();
    const auto w = e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::PrivateUseBmp,
                              PUA_BMP_EXTRA);
    REQUIRE(w.has_value());
    CHECK(w->Bytes.size() <= PROBE_BYTES_MAX);
    CHECK(w->Glyphs == PROBE_GLYPHS_MAX);
    // The bound itself holds for every class at the widest pen.
    CHECK(PROBE_BYTES_MAX >=
          OVERLAY_FRAME_BYTES_MAX + 10 * OVERLAY_CELL_BYTES_MAX + 20 + 7);
}

// --- The gates ------------------------------------------------------------

TEST_CASE("the point after a prompt and two keys is clean") {
    Mirror m;
    CHECK(m.Gate() == EProbeGate::Clean);
    CHECK(m.Gate(EWidthClass::PrivateUseBmp, PUA_BMP_EXTRA) ==
          EProbeGate::Clean);
    CHECK(m.Gate(EWidthClass::PrivateUsePlanes) == EProbeGate::Clean);
}

TEST_CASE(
    "no probe without a mirror, in the middle of a sequence or a "
    "character") {
    {
        Mirror m(40, 5, EDisplayMode::Never);
        CHECK(m.Gate() == EProbeGate::NoMirror);
    }
    {
        Mirror m;
        m.R.Output("\x1b[3");
        CHECK(m.Gate() == EProbeGate::NotGround);
    }
    {
        Mirror m;
        m.R.Output("\xD0");  // the first byte of a letter
        CHECK(m.Gate() == EProbeGate::NotGround);
    }
}

TEST_CASE("no probe on the alternate screen") {
    Mirror m;
    m.R.Output("\x1b[?1049h");
    CHECK(m.Gate() == EProbeGate::AltScreen);
}

TEST_CASE("no probe at a deferred wrap") {
    Mirror m;
    m.R.Output("\r" + std::string(40, 'x'));
    REQUIRE(m.R.G->PendingWrap());
    CHECK(m.Gate() == EProbeGate::PendingWrap);
}

TEST_CASE("no probe with origin mode or insert mode") {
    {
        Mirror m;
        m.R.Output("\x1b[?6h");
        CHECK(m.Gate() == EProbeGate::Decom);
    }
    {
        Mirror m;
        m.R.Output("\x1b[4h");
        CHECK(m.Gate() == EProbeGate::Irm);
    }
}

TEST_CASE("no probe with G0 other than ASCII, or with SO") {
    {
        Mirror m;
        m.R.Output("\x1b(0");
        CHECK(m.Gate() == EProbeGate::Charset);
        m.R.Output("\x1b(B");
        CHECK(m.Gate() == EProbeGate::Clean);
    }
    {
        Mirror m;
        m.R.Output("\x0e");
        CHECK(m.Gate() == EProbeGate::Charset);
        m.R.Output("\x0f");
        CHECK(m.Gate() == EProbeGate::Clean);
    }
}

TEST_CASE("no probe while the program's synchronised update is open") {
    Mirror m;
    m.R.Output("\x1b[?2026h");
    CHECK(m.Gate() == EProbeGate::SyncFrame);
    m.R.Output("\x1b[?2026l");
    CHECK(m.Gate() == EProbeGate::Clean);
}

TEST_CASE("no probe while a mode the mirror does not model is set") {
    for (const char* set :
         {"\x1b[?69h", "\x1b[?45h", "\x1b[?3h", "\x1b%@", "\x1b[?2l"}) {
        CAPTURE(set);
        Mirror m;
        m.R.Output(set);
        CHECK(m.Gate() == EProbeGate::UnmodelledMode);
    }
}

TEST_CASE("no probe while an SGR attribute the pen cannot hold is set") {
    for (const char* set : {"\x1b[53m", "\x1b[6m", "\x1b[58:2::1:2:3m"}) {
        CAPTURE(set);
        Mirror m;
        m.R.Output(set);
        CHECK(m.Gate() == EProbeGate::UnmodelledSgr);
        m.R.Output("\x1b[0m");
        CHECK(m.Gate() == EProbeGate::Clean);
    }
}

TEST_CASE(
    "no probe when the rows or the column of the mirror are not the "
    "terminal's") {
    {
        Mirror m(40, 5, EDisplayMode::Always, false);
        m.R.Output("\x1b[5A");  // up past the first row: the rows are lost
        REQUIRE_FALSE(m.R.G->RowsTrusted());
        CHECK(m.Gate() == EProbeGate::RowsUntrusted);
    }
    {
        Rig r(40, 5, EDisplayMode::Always);
        r.Output("abc");  // the column was never set absolutely
        REQUIRE(r.G->Resync(0, 3, true));  // the engine has not seen it yet
        CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
              EProbeGate::ColumnUntrusted);
        r.Output("\r\x1b[K$ ");
        CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
              EProbeGate::Clean);
    }
}

TEST_CASE("no probe when a mark sits anywhere in the cursor row") {
    for (const char* row : {"\x1b[30G\xC3\xA9\r$ ",         // é right of it
                            "\x1b[30G\xE2\x80\xA6\r$ ",     // …
                            "\x1b[30G\xD0\x96\r$ ",         // Ж, own class
                            "\x1b[30G\xEE\x82\xB0\r$ "}) {  // U+E0B0
        CAPTURE(row);
        Mirror m;
        m.R.Output(row);
        CHECK(m.Gate(EWidthClass::Cyrillic) == EProbeGate::RowMark);
        CHECK(m.Gate(EWidthClass::PrivateUseBmp) == EProbeGate::RowMark);
    }
    // Another row's mark is not this row's.
    Mirror m;
    m.R.Output("\x1b[30G\xC3\xA9\r\n$ ");
    CHECK(m.Gate() == EProbeGate::Clean);
}

TEST_CASE("no probe with CJK or an emoji left of the cursor") {
    for (const char* wide : {"\xE4\xB8\xAD", "\xF0\x9F\x98\x80"}) {
        CAPTURE(wide);
        Mirror m;
        // Absolute column after it: no path mark, but the glyph is there.
        m.R.Output(std::string("\r") + wide + "\x1b[8G");
        REQUIRE(m.R.G->CursorCol() == 7);
        CHECK(m.Gate() == EProbeGate::LeftOfCursor);
    }
    // Control: the same row with ASCII.
    Mirror m;
    m.R.Output("\rab\x1b[8G");
    CHECK(m.Gate() == EProbeGate::Clean);
}

TEST_CASE(
    "no probe when anything but ASCII was printed since the column was "
    "set") {
    {
        // x中, back two, ab: the glyph is gone and the terminal's column may
        // not be the mirror's.
        Mirror m;
        m.R.Output("\rx\xE4\xB8\xAD\x1b[2Dab");
        CHECK(m.R.G->CursorCol() == 3);
        CHECK(m.Gate() == EProbeGate::PathNotAscii);
        // CR sets the column absolutely again; the row is cleared.
        m.R.Output("\r\x1b[K");
        CHECK(m.Gate() == EProbeGate::Clean);
    }
    {
        // x中 erased with EL 1: the cells are blank, the mark stays.
        Mirror m;
        m.R.Output("\rx\xE4\xB8\xAD\x1b[1K");
        CHECK(m.Gate() == EProbeGate::PathNotAscii);
        m.R.Output("\r$ ");
        CHECK(m.Gate() == EProbeGate::Clean);
    }
    {
        // A zero-width mark the mirror dropped.
        Mirror m;
        m.R.Output("\ra\xCC\x81");
        CHECK(m.Gate() == EProbeGate::PathNotAscii);
    }
}

TEST_CASE(
    "a cell of the probe that is occupied or not the mirror's stops "
    "the probe") {
    {
        Mirror m;
        m.R.Output("\r$ abcdef\x1b[3G");
        CHECK(m.R.G->CursorCol() == 2);
        CHECK(m.Gate() == EProbeGate::ProbeOccupied);
    }
    {
        // A space is not an empty cell: ECH would not put it back.
        Mirror m;
        m.R.Output("\r$ \x1b[4G   \x1b[4G");
        CHECK(m.R.G->CursorCol() == 3);
        CHECK(m.Gate() == EProbeGate::ProbeOccupied);
    }
    {
        // The last cell of the probe is the one that counts.
        Mirror m;
        m.R.Output("\r$ ab\x1b[12Gz\x1b[5G");
        // Cyrillic is four cells from column 4: 4..7; z is at 11.
        CHECK(m.Gate(EWidthClass::Cyrillic) == EProbeGate::Clean);
        m.R.Output("\x1b[8Gz\x1b[5G");  // z at 7: the fourth cell
        CHECK(m.Gate(EWidthClass::Cyrillic) == EProbeGate::ProbeOccupied);
    }
    {
        // Not the mirror's: the row was synchronised from the terminal and its
        // contents there are not known.
        Mirror m;
        REQUIRE(m.R.G->Resync(0, 2, false));
        CHECK(m.Gate() == EProbeGate::ProbeForeign);
    }
}

TEST_CASE("the probe and the cursor after it stay inside the row") {
    // Cyrillic: four cells from the cursor, which is at column 4.
    {
        Mirror m(9, 5);  // 4 + 4 = 8 < 9
        CHECK(m.Gate(EWidthClass::Cyrillic) == EProbeGate::Clean);
    }
    {
        Mirror m(8, 5);  // 4 + 4 = 8 reaches the last column
        CHECK(m.Gate(EWidthClass::Cyrillic) == EProbeGate::RightEdge);
    }
    // Private use: eight cells, and ten with the encountered point.
    {
        Mirror m(13, 5);
        CHECK(m.Gate(EWidthClass::PrivateUseBmp) == EProbeGate::Clean);
        CHECK(m.Gate(EWidthClass::PrivateUseBmp, PUA_BMP_EXTRA) ==
              EProbeGate::RightEdge);
    }
    {
        Mirror m(12, 5);
        CHECK(m.Gate(EWidthClass::PrivateUseBmp) == EProbeGate::RightEdge);
    }
}

TEST_CASE("no probe until the program's last glyph is known") {
    {
        Rig r(40, 5, EDisplayMode::Always);
        REQUIRE(r.G->Resync(0, 0, true));
        r.Output("\x1b[3G");  // the column is set, nothing was printed
        CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
              EProbeGate::NoLastGlyph);
        r.Output("x");
        CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
              EProbeGate::Clean);
    }
    {
        // A mark after the glyph was dropped: the register holds more than
        // the mirror knows. The glyph is in another row, so the path is clean.
        Rig r(40, 5, EDisplayMode::Always);
        REQUIRE(r.G->Resync(0, 0, true));
        r.Output("\rab\xCC\x81\r\n");
        CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
              EProbeGate::NoLastGlyph);
    }
    {
        // A resync forgets the register: nothing to give it back.
        Mirror m;
        REQUIRE(m.R.G->Resync(0, 2, true));
        CHECK(m.Gate() == EProbeGate::NoLastGlyph);
    }
}

TEST_CASE("after a path B snapshot, without a resync, the probe goes ahead") {
    Mirror m(40, 5, EDisplayMode::Always, false);
    m.R.Output("\x1b[5A");
    REQUIRE(m.Gate() == EProbeGate::RowsUntrusted);

    // The agent's grid, as a snapshot says it.
    auto agent = MakeGrid(40, 5);
    zet::grid::VtParser agentParser;
    const std::string screen = "$ ab\r\n$ cd";
    agentParser.Feed(Bytes(screen).data(), screen.size(), *agent);
    auto snap = zet::grid::Snapshot::Create(*agent);
    REQUIRE(snap.has_value());
    const std::unique_ptr<zet::grid::Snapshot> owned(*snap);

    m.R.G = MakeGrid(40, 5);
    m.R.P.Reset();
    m.R.P.Feed(owned->Bytes().data(), owned->Bytes().size(), *m.R.G);
    m.R.E.OnSnapshot(0, m.R.Now, m.R.View());
    REQUIRE(m.R.G->RowsTrusted());
    CHECK(m.Gate() == EProbeGate::Clean);
}

TEST_CASE("no probe while the rows were never calibrated") {
    // A fresh mirror starts at row 0 whatever row the terminal's cursor is on.
    // Its rows are "trusted" in step 5's sense (nothing moved them apart yet)
    // but nothing has made them the terminal's: no resync, no snapshot.
    Rig r(40, 5, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    REQUIRE(r.G->RowsTrusted());
    CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
          EProbeGate::RowsUntrusted);
    REQUIRE(r.G->Resync(0, 0, true));
    r.Prompt();
    r.ConfirmEpoch();
    CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
          EProbeGate::Clean);
}

TEST_CASE(
    "a probe on an uncalibrated mirror would be answered on another row") {
    // The terminal printed two lines before zet started: its cursor is on
    // row 2, the mirror's on row 0.
    Client c(40, 5, EDisplayMode::Always);
    c.Before("old shell line\r\nanother\r\n");
    c.Start();
    c.Undo();
    auto p = WidthProbe::Create();
    REQUIRE(p != nullptr);
    CHECK(WidthProbe::Check(c.R.E, c.R.View(), EWidthClass::Cyrillic, 0) ==
          EProbeGate::RowsUntrusted);
    CHECK_FALSE(
        p->Build(c.R.E, c.R.View(), EWidthClass::Cyrillic, 0).has_value());

    // Once the terminal has answered a query, the rows are known and the
    // probe is asked on the right row.
    REQUIRE(c.R.G->Resync(2, 0, true));
    c.R.Output("$ x");
    const auto w = p->Build(c.R.E, c.R.View(), EWidthClass::Cyrillic, 0);
    REQUIRE(w.has_value());
    c.Real.Feed(w->Bytes.first(w->QueryOffset));
    CHECK(c.Real.G->CursorRow() == w->Row);
}

TEST_CASE("the rows count as calibrated after a reset of the terminal") {
    // RIS clears both screens and homes both cursors.
    Rig r(40, 5, EDisplayMode::Always);
    r.Output("old\r\n\r\n");
    REQUIRE_FALSE(r.G->RowsCalibrated());
    r.Output(
        "\x1b"
        "c\r$ x");
    CHECK(r.G->RowsCalibrated());
    CHECK(WidthProbe::Check(r.E, r.View(), EWidthClass::Cyrillic, 0) ==
          EProbeGate::Clean);
}

TEST_CASE("no probe after a left move over a mark, from another row") {
    // A left move over the row's mark sets the doubt; LF leaves that row, so
    // neither RowMark nor PathNotAscii catches it.
    Mirror m;
    m.R.Output("\x1b[30G\xC3\xA9\r\x1b[31C\x1b[31D\n$ ");
    REQUIRE(m.R.G->ColumnDoubt());
    REQUIRE_FALSE(m.R.G->PathHasNonAscii());
    REQUIRE(m.R.G->WidthDoubtClasses(m.R.G->CursorRow()) == 0);
    CHECK(m.Gate() == EProbeGate::ColumnUntrusted);
}

TEST_CASE("no probe for an encountered point outside the class") {
    Mirror m;
    CHECK(m.Gate(EWidthClass::Cyrillic, U'a') == EProbeGate::PointNotOfClass);
    CHECK(m.Gate(EWidthClass::Cyrillic, PUA_BMP_A) ==
          EProbeGate::PointNotOfClass);
    CHECK(m.Gate(EWidthClass::PrivateUseBmp, U'А') ==
          EProbeGate::PointNotOfClass);
    // None, a point of the class and a representative are fine.
    CHECK(m.Gate(EWidthClass::Cyrillic, 0) == EProbeGate::Clean);
    CHECK(m.Gate(EWidthClass::Cyrillic, U'А') == EProbeGate::Clean);
    CHECK(m.Gate(EWidthClass::Cyrillic, CYR_ZHE) == EProbeGate::Clean);
    auto p = WidthProbe::Create();
    REQUIRE(p != nullptr);
    CHECK_FALSE(
        p->Build(m.R.E, m.R.View(), EWidthClass::Cyrillic, U'a').has_value());
}

TEST_CASE("the write carries the exit tail, as the mirror had the cursor") {
    {
        Env e;
        e.C.Undo();
        const auto w =
            e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic, 0);
        REQUIRE(w.has_value());
        CHECK(Str(w->Tail) == "\x1b[0m\x1b[?25h\x1b[?2026l");
        CHECK(w->Tail.size() <= PROBE_TAIL_BYTES_MAX);
        // The mirror may change before the probe is written out: the tail is
        // what it was at Build.
        e.C.R.Output("\x1b[?25l");
        CHECK(Str(w->Tail) == "\x1b[0m\x1b[?25h\x1b[?2026l");
    }
    {
        Env e;
        e.C.Agent("\x1b[?25l");
        e.C.Undo();
        const auto w =
            e.P->Build(e.C.R.E, e.C.R.View(), EWidthClass::Cyrillic, 0);
        REQUIRE(w.has_value());
        CHECK(Str(w->Tail) == "\x1b[0m\x1b[?2026l");
    }
}

TEST_CASE("the write carries the size of the mirror it was built for") {
    Mirror m(13, 7);
    auto p = WidthProbe::Create();
    REQUIRE(p != nullptr);
    const auto w = p->Build(m.R.E, m.R.View(), EWidthClass::Cyrillic, 0);
    REQUIRE(w.has_value());
    CHECK(w->Cols == 13);
    CHECK(w->Rows == 7);
}

TEST_CASE("Build writes nothing where Check says no") {
    Mirror m;
    m.R.Output("\x1b[?1049h");
    auto p = WidthProbe::Create();
    REQUIRE(p != nullptr);
    CHECK_FALSE(
        p->Build(m.R.E, m.R.View(), EWidthClass::Cyrillic, 0).has_value());
}
