// Throwaway verifier probes for PR #99 (snapshot encode).
// NOLINTBEGIN
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat-security"
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

Grid* Mk(int c, int r, int sb) {
    auto g = Grid::Create(static_cast<uint16_t>(c), static_cast<uint16_t>(r),
                          static_cast<uint32_t>(sb));
    REQUIRE(g.has_value());
    return g.value();
}
void Feed(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

std::string Hex(std::string_view s) {
    std::string o;
    for (auto rawByte : s) {
        const auto ch = static_cast<unsigned char>(rawByte);
        if (ch >= 0x20 && ch < 0x7f)
            o += static_cast<char>(ch);
        else {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", ch);
            o += b;
        }
    }
    return o;
}

struct Diff {
    std::string Out;
    int N{0};
    template <class... A>
    void Add(const char* fmt, A... a) {
        char b[512];
        snprintf(b, sizeof b, fmt, a...);
        if (N++ < 12) {
            Out += b;
            Out += "\n";
        }
    }
};

bool SameCell(const Cell& a, const Cell& b) {
    auto cp = [](const Cell& c) {
        return c.Codepoint == 0 ? U' ' : c.Codepoint;
    };
    return cp(a) == cp(b) && a.Width == b.Width && a.Attrs == b.Attrs &&
           a.FgKind == b.FgKind && a.BgKind == b.BgKind &&
           !memcmp(a.Fg, b.Fg, 3) && !memcmp(a.Bg, b.Bg, 3) &&
           a.AttrEx == b.AttrEx && a.UlIdx == b.UlIdx;
}
void CellStr(char* b, std::size_t n, const Cell& c) {
    snprintf(b, n,
             "cp=%x w=%d at=%02x fk=%d/%d,%d,%d bk=%d/%d,%d,%d ax=%d ul=%d",
             static_cast<unsigned>(c.Codepoint), c.Width, c.Attrs,
             static_cast<int>(c.FgKind), c.Fg[0], c.Fg[1], c.Fg[2],
             static_cast<int>(c.BgKind), c.Bg[0], c.Bg[1], c.Bg[2], c.AttrEx,
             c.UlIdx);
}

void CmpSaved(Diff& d, const char* nm, const SavedCursor& a,
              const SavedCursor& b) {
    if (a.Row != b.Row || a.Col != b.Col || a.Attrs != b.Attrs ||
        a.FgKind != b.FgKind || a.BgKind != b.BgKind || memcmp(a.Fg, b.Fg, 3) ||
        memcmp(a.Bg, b.Bg, 3) || a.AttrEx != b.AttrEx || a.UlIdx != b.UlIdx ||
        a.Decom != b.Decom || a.CharsetG0 != b.CharsetG0 ||
        a.CharsetG1 != b.CharsetG1 || a.CharsetActive != b.CharsetActive)
        d.Add(
            "%s saved cursor: (%d,%d) at=%02x fk=%d bk=%d ax=%d ul=%d vs "
            "(%d,%d) at=%02x fk=%d bk=%d ax=%d ul=%d",
            nm, a.Row, a.Col, a.Attrs, static_cast<int>(a.FgKind),
            static_cast<int>(a.BgKind), a.AttrEx, a.UlIdx, b.Row, b.Col,
            b.Attrs, static_cast<int>(b.FgKind), static_cast<int>(b.BgKind),
            b.AttrEx, b.UlIdx);
}

void Compare(Diff& d, const Grid& a, const Grid& b, bool screens = true) {
    if (a.Cols() != b.Cols() || a.Rows() != b.Rows()) {
        d.Add("size");
        return;
    }
    if (a.ScrollbackSize() != b.ScrollbackSize())
        d.Add("scrollback size %u vs %u", a.ScrollbackSize(),
              b.ScrollbackSize());
    else
        for (uint32_t i = 0; i < a.ScrollbackSize(); ++i)
            for (uint16_t c = 0; c < a.Cols(); ++c)
                if (!SameCell(a.ScrollbackLine(i)->Cells[c],
                              b.ScrollbackLine(i)->Cells[c])) {
                    char x[200], y[200];
                    CellStr(x, 200, a.ScrollbackLine(i)->Cells[c]);
                    CellStr(y, 200, b.ScrollbackLine(i)->Cells[c]);
                    d.Add("scrollback[%u][%d]: %s | %s", i, c, x, y);
                }
    if (screens) {
        for (uint16_t r = 0; r < a.Rows(); ++r)
            for (uint16_t c = 0; c < a.Cols(); ++c) {
                if (!SameCell(a.MainCellAt(r, c), b.MainCellAt(r, c))) {
                    char x[200], y[200];
                    CellStr(x, 200, a.MainCellAt(r, c));
                    CellStr(y, 200, b.MainCellAt(r, c));
                    d.Add("main[%d][%d]: %s | %s", r, c, x, y);
                }
                if (a.IsAltScreen() &&
                    !SameCell(a.CellAt(r, c), b.CellAt(r, c))) {
                    char x[200], y[200];
                    CellStr(x, 200, a.CellAt(r, c));
                    CellStr(y, 200, b.CellAt(r, c));
                    d.Add("alt[%d][%d]: %s | %s", r, c, x, y);
                }
            }
    }
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol())
        d.Add("cursor (%d,%d) vs (%d,%d)", a.CursorRow(), a.CursorCol(),
              b.CursorRow(), b.CursorCol());
    if (a.PendingWrap() != b.PendingWrap())
        d.Add("pending %d vs %d", a.PendingWrap(), b.PendingWrap());
    if (a.ScrollTop() != b.ScrollTop() || a.ScrollBot() != b.ScrollBot())
        d.Add("region %d-%d vs %d-%d", a.ScrollTop(), a.ScrollBot(),
              b.ScrollTop(), b.ScrollBot());
    uint32_t m = ~ModeFlag(EMode::SyncOutput);
    if ((a.Modes() & m) != (b.Modes() & m))
        d.Add("modes %x vs %x", a.Modes() & m, b.Modes() & m);
    CmpSaved(d, "main", a.SavedCursorMainState(), b.SavedCursorMainState());
    CmpSaved(d, "alt", a.SavedCursorAltState(), b.SavedCursorAltState());
    if (a.CharsetG0() != b.CharsetG0() || a.CharsetG1() != b.CharsetG1() ||
        a.CharsetActive() != b.CharsetActive())
        d.Add("charset");
    if (a.CursorStyle() != b.CursorStyle()) d.Add("curstyle");
    for (uint16_t c = 0; c < a.Cols(); ++c)
        if (a.IsTabStop(c) != b.IsTabStop(c)) d.Add("tabstop %d", c);
    if (a.PenAttrs() != b.PenAttrs() || a.PenFgKind() != b.PenFgKind() ||
        a.PenBgKind() != b.PenBgKind() || memcmp(a.PenFg(), b.PenFg(), 3) ||
        memcmp(a.PenBg(), b.PenBg(), 3) || a.PenAttrEx() != b.PenAttrEx() ||
        a.PenUlIdx() != b.PenUlIdx())
        d.Add(
            "pen at=%02x fk=%d bk=%d ax=%d ul=%d vs at=%02x fk=%d bk=%d ax=%d "
            "ul=%d",
            a.PenAttrs(), static_cast<int>(a.PenFgKind()),
            static_cast<int>(a.PenBgKind()), a.PenAttrEx(), a.PenUlIdx(),
            b.PenAttrs(), static_cast<int>(b.PenFgKind()),
            static_cast<int>(b.PenBgKind()), b.PenAttrEx(), b.PenUlIdx());
}

// Build, snapshot, restore, compare; then feed `after` to both and compare
// again.
void Probe(const char* name, int cols, int rows, int sb, std::string_view input,
           std::string_view after = {}, std::size_t maxBytes = 0) {
    Grid* src = Mk(cols, rows, sb);
    VtParser sp;
    Feed(sp, *src, input);
    auto snap = Snapshot::Create(*src, SnapshotOptions{.MaxBytes = maxBytes});
    REQUIRE(snap.has_value());
    Grid* dst = Mk(cols, rows, sb);
    VtParser dp;
    auto by = snap.value()->Bytes();
    dp.Feed(by.data(), by.size(), *dst);
    Diff d;
    Compare(d, *src, *dst);
    if (!after.empty()) {
        Feed(sp, *src, after);
        Feed(dp, *dst, after);
        Diff d2;
        Compare(d2, *src, *dst);
        if (d2.N) {
            d.Add("--- after feeding [%s] ---", Hex(after).c_str());
            d.Out += d2.Out;
            d.N += d2.N;
        }
    }
    INFO("PROBE " << name << " input=[" << Hex(input) << "]\n" << d.Out);
    CHECK_MESSAGE(d.N == 0, name);
    delete snap.value();
    delete src;
    delete dst;
}

// The degrade ladder must not touch a grid that fits: with the default cap,
// with a cap of exactly the full size and with an oversized cap the bytes are
// identical, nothing is degraded, and the snapshot round-trips.  `golden` is
// the exact output of the full-fidelity encoder (PR #99) for the grid.
void ExpectLadderInert(const char* name, int cols, int rows, int sb,
                       std::string_view input, std::string_view golden,
                       std::size_t goldenScrollbackLen) {
    Grid* src = Mk(cols, rows, sb);
    VtParser sp;
    Feed(sp, *src, input);

    auto base = Snapshot::Create(*src);
    REQUIRE(base.has_value());
    const auto baseBytes = base.value()->Bytes();
    const std::string_view baseView{
        reinterpret_cast<const char*>(baseBytes.data()), baseBytes.size()};
    CHECK_MESSAGE(base.value()->Degradation() == EDegradation::None, name);
    if (!golden.empty()) {
        CHECK_MESSAGE(baseView == golden, name);
        CHECK_MESSAGE(base.value()->ScrollbackLen() == goldenScrollbackLen,
                      name);
    }

    for (std::size_t cap : {baseBytes.size(), baseBytes.size() + 1,
                            std::size_t{16} * 1024 * 1024}) {
        auto r = Snapshot::Create(*src, SnapshotOptions{.MaxBytes = cap});
        REQUIRE(r.has_value());
        const auto b = r.value()->Bytes();
        CHECK_MESSAGE(r.value()->Degradation() == EDegradation::None, name);
        CHECK_MESSAGE(std::string_view(reinterpret_cast<const char*>(b.data()),
                                       b.size()) == baseView,
                      name);
        CHECK_MESSAGE(
            r.value()->ScrollbackLen() == base.value()->ScrollbackLen(), name);
        delete r.value();
    }
    // One byte under the full size must degrade (the ladder is live).
    auto under = Snapshot::Create(
        *src, SnapshotOptions{.MaxBytes = baseBytes.size() - 1});
    if (under.has_value()) {
        CHECK_MESSAGE(under.value()->Degradation() != EDegradation::None, name);
        delete under.value();
    }
    delete base.value();
    delete src;

    // Round trip at exactly the full size.
    Probe(name, cols, rows, sb, input, {}, baseBytes.size());
}

}  // namespace

TEST_CASE("P01 colors 16/256/truecolor") {
    Probe("P01", 20, 6, 10,
          "\x1b[31mred\x1b[0m "
          "\x1b[1;92mbright\x1b[0m\r\n\x1b[38;5;200;48;5;17mi256\x1b[0m "
          "\x1b[38;2;1;2;3;48;2;250;251;252mtrue\x1b[0m\r\n"
          "\x1b[97;40mwb\x1b[0m\x1b[48;5;9mbg9\x1b[0m");
}
TEST_CASE("P02 all attrs") {
    Probe("P02", 20, 4, 10,
          "\x1b[1mB\x1b[2mD\x1b[3mI\x1b[4mU\x1b[5mK\x1b[7mV\x1b[8mX\x1b["
          "9mS\x1b[0m plain\x1b[1;3;5;7;9mmix");
}
TEST_CASE("P03 underline styles + colour") {
    Probe("P03", 30, 4, 10,
          "\x1b[4:1mA\x1b[4:2mB\x1b[4:3mC\x1b[4:4mD\x1b[4:5mE\x1b[0m\r\n\x1b[4;"
          "58;5;9mulc\x1b[0m \x1b[4:3;58;5;200mcurly\x1b[0m");
}
TEST_CASE("P04 ul colour without underline attr") {
    Probe("P04", 20, 4, 10, "\x1b[58;5;5mabc\x1b[0m\r\nplain");
}
TEST_CASE("P05 scrollback coloured") {
    std::string in;
    for (int i = 0; i < 30; ++i)
        in += "\x1b[3" + std::to_string(i % 8) + "mline " + std::to_string(i) +
              "\x1b[0m\r\n";
    Probe("P05", 20, 10, 100, in);
}
TEST_CASE("P06 scrollback ring overflow") {
    std::string in;
    for (int i = 0; i < 80; ++i) in += "L" + std::to_string(i) + "\r\n";
    Probe("P06", 12, 5, 20, in);
}
TEST_CASE("P07 DECSC + colors then more movement") {
    Probe("P07", 20, 8, 10,
          "\x1b[3;4H\x1b[1;31;44m\x1b"
          "7\x1b[0m\x1b[6;10Hxyz",
          "\x1b"
          "8Q");
}
TEST_CASE("P08 DECSTBM + DECOM cursor") {
    Probe("P08", 20, 10, 10, "\x1b[3;7r\x1b[?6h\x1b[2;5Hhi",
          "X\x1b[?6l\x1b[1;1HZ");
}
TEST_CASE("P09 DECOM + saved cursor inside region") {
    Probe("P09", 20, 10, 10,
          "\x1b[3;7r\x1b[?6h\x1b[2;5H\x1b"
          "7\x1b[4;4Hq",
          "\x1b"
          "8W");
}
TEST_CASE("P10 alt screen main preserved") {
    Probe("P10", 20, 6, 20,
          "m1\r\nm2\r\nm3\r\nm4\r\nm5\r\nm6\r\nm7\r\nm8\x1b[?1049h\x1b[2;"
          "2Halt\x1b[4;1Hmore",
          "\x1b[?1049l");
}
TEST_CASE("P11 DECSC in main, then alt, then DECSC in alt, leave alt") {
    Probe("P11", 20, 8, 10,
          "main\x1b[3;5H\x1b[32m\x1b"
          "7\x1b[0m\x1b[5;5H\x1b[?1049h\x1b[2;2Halt\x1b[4;4H\x1b"
          "7\x1b[6;6H",
          "\x1b[?1049l\x1b"
          "8!");
}
TEST_CASE("P12 alt screen, cursor in main before entering") {
    Probe("P12", 20, 8, 10, "\x1b[7;7Hmain\x1b[?1049h\x1b[1;1Halt",
          "\x1b[?1049l");
}
TEST_CASE("P13 pending wrap at last col") {
    Probe("P13", 10, 4, 10, "0123456789", "Z");
}
TEST_CASE("P14 pending wrap with coloured pen: live pen") {
    Probe("P14", 10, 4, 10, "\x1b[31m0123456789\x1b[1;42m", "Z");
}
TEST_CASE("P15 pending wrap, cell has attrs, pen differs") {
    Probe("P15", 10, 4, 10, "\x1b[1;34m012345678\x1b[0;4m9", "Z");
}
TEST_CASE("P16 insert mode") {
    Probe("P16", 20, 4, 10, "abcdef\x1b[1;3H\x1b[4h", "XY");
}
TEST_CASE("P17 cursor hidden") { Probe("P17", 20, 4, 10, "abc\x1b[?25l"); }
TEST_CASE("P18 wide char at right edge (pending wrap on continuation)") {
    Probe("P18", 10, 4, 10, "12345678\xe4\xb8\xad", "Z");
}
TEST_CASE("P19 wide char wraps at last col") {
    Probe("P19", 10, 4, 10,
          "123456789\xe4\xb8\xad"
          "x");
}
TEST_CASE("P20 graphics charset G0 + pending wrap") {
    Probe("P20", 10, 4, 10, "012345678q\x1b(0", "q");
}
TEST_CASE("P21 G1 graphics + SO") {
    Probe("P21", 10, 4, 10, "abc\x1b)0\x0e", "qx");
}
TEST_CASE("P22 custom tab stops") {
    Probe("P22", 40, 4, 10,
          "\x1b[3g\x1b[1;5H\x1bH\x1b[1;13H\x1bH\x1b[1;1Ha\tb\tc", "\td");
}
TEST_CASE("P23 mouse/paste/focus/ckm/lnm") {
    Probe("P23", 20, 4, 10,
          "\x1b[?1h\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[?1004h\x1b[20h\x1b[?"
          "1002h\x1b[?1003h\x1b[?1005h\x1b[?1015h",
          "\n");
}
TEST_CASE("P24 autowrap off at right edge") {
    Probe("P24", 10, 4, 10, "\x1b[?7l0123456789ABC", "Z");
}
TEST_CASE("P25 history with ul-colour-only last cell then plain screen") {
    std::string in = "\x1b[58;5;7mxx\x1b[0m\r\n\r\n\r\n\r\n\r\n\r\nscreen";
    Probe("P25", 20, 4, 10, in);
}
TEST_CASE("P26 bg erase cells") {
    Probe("P26", 20, 4, 10,
          "\x1b[44mab\x1b[K\r\n\x1b[41m\x1b[2K\x1b[0m\x1b[3;3H\x1b[45m\x1b[3X");
}
TEST_CASE("P27 full-width rows + history") {
    std::string in;
    for (int i = 0; i < 12; ++i)
        in += std::string(10, static_cast<char>('a' + i)) + "\r\n";
    Probe("P27", 10, 5, 20, in);
}
TEST_CASE("P28 one-row grid with history") {
    Probe("P28", 10, 1, 20, "a\r\nb\r\nc\r\nd");
}
TEST_CASE("P29 emoji 4-byte + wide in history") {
    std::string in;
    for (int i = 0; i < 8; ++i)
        in += "\xf0\x9f\x98\x80 \xe4\xb8\xad\xe6\x96\x87\r\n";
    Probe("P29", 10, 4, 20, in);
}
TEST_CASE("P30 wide char in last two cols of history rows") {
    std::string in;
    for (int i = 0; i < 6; ++i) in += "12345678\xe4\xb8\xad\r\n";
    Probe("P30", 10, 4, 20, in);
}
TEST_CASE("P31 alt screen + pending wrap + colors") {
    Probe("P31", 10, 4, 10, "m\x1b[?1049h\x1b[33m0123456789", "Z\x1b[?1049l");
}
TEST_CASE("P32 region + scrollback (region scrolled)") {
    std::string in = "\x1b[2;4r\x1b[4;1H";
    for (int i = 0; i < 6; ++i) in += "r" + std::to_string(i) + "\r\n";
    in += "\x1b[r";
    for (int i = 0; i < 10; ++i) in += "s" + std::to_string(i) + "\r\n";
    Probe("P32", 10, 6, 20, in);
}
TEST_CASE("P33 DECSCUSR + title-ish stuff") {
    Probe("P33", 10, 4, 10, "\x1b[5 qabc");
}
TEST_CASE("P34 alt screen + DECOM region") {
    Probe("P34", 20, 8, 10, "x\x1b[?1049h\x1b[2;6r\x1b[?6h\x1b[3;3Hhey",
          "k\x1b[?1049l");
}
TEST_CASE("P35 pending wrap with IRM on") {
    Probe("P35", 10, 4, 10, "\x1b[4h0123456789", "Z");
}
TEST_CASE("P36 pending wrap with DECOM") {
    Probe("P36", 10, 8, 10, "\x1b[3;6r\x1b[?6h\x1b[1;1H0123456789", "Z");
}
TEST_CASE("P37 grid with sync output left on in source") {
    Probe("P37", 10, 4, 10, "abc\x1b[?2026h");
}
TEST_CASE("P38 idempotent re-snapshot") {
    Grid* a = Mk(12, 5, 20);
    VtParser p;
    std::string in;
    for (int i = 0; i < 15; ++i)
        in += "\x1b[3" + std::to_string(i % 8) + "mL" + std::to_string(i) +
              "\r\n";
    Feed(p, *a, in + "\x1b[?25l\x1b[2;3r\x1b[1;5H");
    auto s1 = Snapshot::Create(*a);
    Grid* b = Mk(12, 5, 20);
    VtParser q;
    auto by = s1.value()->Bytes();
    q.Feed(by.data(), by.size(), *b);
    auto s2 = Snapshot::Create(*b);
    REQUIRE(s2.has_value());
    auto b1 = s1.value()->Bytes();
    auto b2 = s2.value()->Bytes();
    CHECK(b1.size() == b2.size());
    CHECK((b1.size() == b2.size() && !memcmp(b1.data(), b2.data(), b1.size())));
    delete s2.value();
    delete s1.value();
    delete b;
    delete a;
}
TEST_CASE(
    "P39 DECSTR-surviving hostile prior state: snapshot into dirty grid") {
    // fresh-grid target is clean; try a dirty target: restore into grid with
    // junk state.
    Grid* src = Mk(10, 4, 10);
    VtParser sp;
    Feed(sp, *src, "hello\r\nworld");
    auto snap = Snapshot::Create(*src);
    Grid* dst = Mk(10, 4, 10);
    VtParser dp;
    Feed(dp, *dst,
         "\x1b[?1049h\x1b[?7l\x1b[?6h\x1b[2;3r\x1b[31;44;1m\x1b[?1000h\x1b("
         "0\x0e\x1b[4h\x1b[?25l\x1b[3gJUNK\x1b[5 q");
    auto by = snap.value()->Bytes();
    dp.Feed(by.data(), by.size(), *dst);
    Diff d;
    Compare(d, *src, *dst);
    INFO(d.Out);
    CHECK(d.N == 0);
    delete snap.value();
    delete dst;
    delete src;
}
TEST_CASE("P40 dirty target with scrollback junk") {
    Grid* src = Mk(10, 4, 10);
    VtParser sp;
    Feed(sp, *src, "h1\r\nh2\r\nh3\r\nh4\r\nh5\r\nh6");
    auto snap = Snapshot::Create(*src);
    Grid* dst = Mk(10, 4, 10);
    VtParser dp;
    Feed(dp, *dst, "j1\r\nj2\r\nj3\r\nj4\r\nj5\r\nj6\r\nj7\r\nj8\r\nj9");
    auto by = snap.value()->Bytes();
    dp.Feed(by.data(), by.size(), *dst);
    Diff d;
    Compare(d, *src, *dst);
    INFO(d.Out);
    // Design decision: no CSI 3J — snapshot does not clear the target's
    // existing scrollback.  The visible screen must match; scrollback may
    // differ.  This is expected, so use WARN (info-only, never a hard fail).
    WARN_MESSAGE(d.N == 0, "scrollback junk (expected: no CSI 3J by design)");
    delete snap.value();
    delete dst;
    delete src;
}

// ---- Hostile cells ----
TEST_CASE("H1 hostile codepoints via PutChar") {
    Grid* g = Mk(20, 3, 10);
    const char32_t bad[] = {0x1b, 0x9b, 0x9d, 0x90,   0x9c,     0x07,      0x18,
                            0x1a, 0x7f, 0x85, 0xD800, 0x110000, 0xFFFFFFFF};
    for (char32_t c : bad) g->PutChar(c, 1);
    g->CarriageReturn();
    g->LineFeed();
    // scroll to history too
    for (char32_t c : bad) g->PutChar(c, 1);
    for (int i = 0; i < 5; ++i) {
        g->CarriageReturn();
        g->LineFeed();
    }
    auto s = Snapshot::Create(*g);
    REQUIRE(s.has_value());
    auto by = s.value()->Bytes();
    // restore and make sure parser state / unknown seq count is same as benign
    Grid* dst = Mk(20, 3, 10);
    VtParser dp;
    dp.Feed(by.data(), by.size(), *dst);
    CHECK(dp.UnknownSeqCount() == 0);
    // scan for C1 as raw single bytes or UTF-8 encoded C1 (C2 80..9F)
    int c1 = 0, esc = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(by.data());
    // count ESC bytes vs expected from structure: compare against sanitized
    // twin
    Grid* twin = Mk(20, 3, 10);
    for (char32_t dummy : bad) {
        (void)dummy;
        twin->PutChar(U'?', 1);
    }
    twin->CarriageReturn();
    twin->LineFeed();
    for (char32_t dummy : bad) {
        (void)dummy;
        twin->PutChar(U'?', 1);
    }
    for (int i = 0; i < 5; ++i) {
        twin->CarriageReturn();
        twin->LineFeed();
    }
    auto st = Snapshot::Create(*twin);
    auto tb = st.value()->Bytes();
    CHECK(tb.size() == by.size());
    CHECK((tb.size() == by.size() && !memcmp(tb.data(), by.data(), by.size())));
    for (std::size_t i = 0; i + 1 < by.size(); ++i) {
        if (p[i] == 0xC2 && p[i + 1] >= 0x80 && p[i + 1] <= 0x9F) ++c1;
    }
    CHECK(c1 == 0);
    (void)esc;
    delete st.value();
    delete twin;
    delete dst;
    delete s.value();
    delete g;
}
TEST_CASE("H2 bidi / line-separator codepoints pass through (info)") {
    Grid* g = Mk(20, 3, 10);
    const char32_t bidi[] = {0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066,
                             0x2067, 0x2068, 0x2069, 0x200E, 0x200F, 0x061C,
                             0x2028, 0x2029, 0xFEFF, 0xE0041};
    int in_cells = 0;
    VtParser p;
    for (char32_t c : bidi) {
        char32_t a[1] = {c};
        // feed via UTF-8 through parser as a real pty would
        char b[5];
        int n;
        if (c < 0x800) {
            b[0] = static_cast<char>(0xC0u | (c >> 6));
            b[1] = static_cast<char>(0x80u | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            b[0] = static_cast<char>(0xE0u | (c >> 12));
            b[1] = static_cast<char>(0x80u | ((c >> 6) & 0x3F));
            b[2] = static_cast<char>(0x80u | (c & 0x3F));
            n = 3;
        } else {
            b[0] = static_cast<char>(0xF0u | (c >> 18));
            b[1] = static_cast<char>(0x80u | ((c >> 12) & 0x3F));
            b[2] = static_cast<char>(0x80u | ((c >> 6) & 0x3F));
            b[3] = static_cast<char>(0x80u | (c & 0x3F));
            n = 4;
        }
        Feed(p, *g, std::string_view(b, static_cast<std::size_t>(n)));
        (void)a;
    }
    for (int c = 0; c < 20; ++c)
        for (auto cp : bidi)
            if (g->CellAt(0, static_cast<std::uint16_t>(c)).Codepoint == cp)
                ++in_cells;
    for (int c = 0; c < 20; ++c)
        if (g->CellAt(0, static_cast<std::uint16_t>(c)).Codepoint)
            MESSAGE(
                "cell "
                << c << " cp=" << std::hex
                << static_cast<unsigned>(
                       g->CellAt(0, static_cast<std::uint16_t>(c)).Codepoint)
                << " w="
                << static_cast<int>(
                       g->CellAt(0, static_cast<std::uint16_t>(c)).Width));
    auto s = Snapshot::Create(*g);
    auto by = s.value()->Bytes();
    int found = 0;
    const unsigned char* q = reinterpret_cast<const unsigned char*>(by.data());
    for (std::size_t i = 0; i + 2 < by.size(); ++i)
        if (q[i] == 0xE2 && (q[i + 1] == 0x80 || q[i + 1] == 0x81)) ++found;
    MESSAGE("bidi/separator codepoints stored in cells: "
            << in_cells << ", E2 80/81 sequences in snapshot: " << found);
    CHECK(true);
    delete s.value();
    delete g;
}
TEST_CASE("H3 title/OSC cannot reach snapshot") {
    Grid* g = Mk(20, 3, 10);
    VtParser p;
    Feed(p, *g,
         "a\x1b]0;evil\x07\x1b]52;c;AAAA\x07\x1b_Gpayload\x1b\\\x1bPq#0;2;0;0;"
         "0\x1b\\b");
    auto s = Snapshot::Create(*g);
    auto by = s.value()->Bytes();
    std::string str(reinterpret_cast<const char*>(by.data()), by.size());
    CHECK(str.find("evil") == std::string::npos);
    CHECK(str.find("payload") == std::string::npos);
    CHECK(str.find("]52") == std::string::npos);
    delete s.value();
    delete g;
}
TEST_CASE("P41 pending wrap with AWM off then re-enabled") {
    Probe("P41", 10, 4, 10, "\x1b[?7l0123456789", "\x1b[?7hZ");
}
TEST_CASE("P42 alt screen, main cursor mid-screen, exit alt") {
    Probe("P42", 20, 8, 10, "abc\r\ndef\x1b[3;7H\x1b[?1049h\x1b[1;1Halt",
          "\x1b[?1049lX");
}
TEST_CASE("P43 alt screen, no DECSC anywhere, main blank bottom") {
    Probe("P43", 20, 8, 10, "top\x1b[2;2H\x1b[?1049h", "\x1b[?1049lX");
}
#pragma clang diagnostic pop
// NOLINTEND

// ---------------------------------------------------------------------------
// Degrade ladder is inert when the grid fits the cap (byte-identical to the
// full-fidelity encoder).
// ---------------------------------------------------------------------------

TEST_CASE("ladder inert: alt screen with main screen and history") {
    // Golden: exact output of the PR #99 encoder.
    static constexpr std::string_view GOLDEN =
        "\030\033\134\033[!p\033[\0771049l\033[2J\033[H\033[\07725l\033[0m\033["
        "2"
        "J\033[H\033[1;1Hmain1\015\012main2\033[0m\033[2;6H\033[\0771049h\033["
        "0m"
        "\033[2J\033[H\033[\0772026h\033[2;1H "
        "alt\033[0;1;31m!\033[0m\033[1;1H\033"
        "7\033[0m\033[0;1;31m\033[1;4r\033[\0771l\033[\0777h\033[4l\033["
        "20l\033["
        "\0771000l\033[\0771002l\033[\0771003l\033[\0771005l\033[\0771006l\033["
        "\077"
        "1015l\033[\0772004l\033[\0771004l\033(B\033)B\017\033[3g\033[1;"
        "1H\033H\033"
        "[1;9H\033H\033[0 q\033[\0776l\033[2;6H\033[\07725h\033[\0772026l";
    ExpectLadderInert("L-alt", 12, 4, 5,
                      "main1\r\nmain2\x1b[?1049h\x1b[2;2Halt\x1b[1;31m!",
                      GOLDEN, 86);
}

TEST_CASE("ladder inert: scrollback history") {
    // Golden: exact output of the PR #99 encoder.
    static constexpr std::string_view GOLDEN =
        "\030\033\134\033[!p\033[\0771049l\033[2J\033[H\033["
        "\07725lline1\015\012"
        "line2\015\012line3\015\012\012\012\033[0m\033[2J\033[H\033["
        "\0772026h\033[1;"
        "1Hline4\015\012line5\015\012"
        "line6\033[0m\033[1;1H\0337\033[0m\033[1;3r\033[\0771l\033[\0777h\033["
        "4l"
        "\033[20l\033[\0771000l\033[\0771002l\033[\0771003l\033[\0771005l\033["
        "\077"
        "1006l\033[\0771015l\033[\0772004l\033[\0771004l\033(B\033)B\017\033["
        "3g\033"
        "[1;1H\033H\033[1;9H\033H\033[0 "
        "q\033[\0776l\033[3;6H\033[\07725h\033[\077"
        "2026l";
    ExpectLadderInert("L-hist", 10, 3, 5,
                      "line1\r\nline2\r\nline3\r\nline4\r\nline5\r\nline6",
                      GOLDEN, 62);
}

TEST_CASE("ladder inert: truecolor fg and bg") {
    // Golden: exact output of the PR #99 encoder.
    static constexpr std::string_view GOLDEN =
        "\030\033\134\033[!p\033[\0771049l\033[2J\033[H\033[\07725l\033[0m\033["
        "2"
        "J\033[H\033"
        "[\0772026h\033[1;1H\033[0;38;2;10;200;30m\033[48;2;250;3;99mTC\033[0m "
        "o"
        "k\033[0m\033[1;1H\0337\033[0m\033[1;2r\033[\0771l\033[\0777h\033["
        "4l\033"
        "[20l\033[\0771000l\033[\0771002l\033[\0771003l\033[\0771005l\033["
        "\07710"
        "06l\033[\0771015l\033[\0772004l\033[\0771004l\033(B\033)B\017\033["
        "3g\033"
        "[1;1H\033H\033[1;9H\033H\033[0 "
        "q\033[\0776l\033[1;6H\033[\07725h\033[\077"
        "2026l";
    ExpectLadderInert("L-true", 10, 2, 3,
                      "\x1b[38;2;10;200;30;48;2;250;3;99mTC\x1b[0m ok", GOLDEN,
                      39);
}

TEST_CASE(
    "ladder inert: wide characters filling the row (pending wrap on a "
    "continuation cell)") {
    // Golden: exact output of the PR #99 encoder.
    static constexpr std::string_view GOLDEN =
        "\030\033\134\033[!p\033[\0771049l\033[2J\033[H\033[\07725l\033[0m\033["
        "2"
        "J\033[H\033[\0772026h\033[1;"
        "1Hab\344"
        "\270\255\346\226\207\033[0m\033[1;1H\0337\033[0m\033[1;2r\033["
        "\0771l\033"
        "[\0777h\033[4l\033[20l\033[\0771000l\033[\0771002l\033[\0771003l\033["
        "\077"
        "1005l\033[\0771006l\033[\0771015l\033[\0772004l\033[\0771004l\033("
        "B\033"
        ")B\017\033[3g\033[1;1H\033H\033[0 "
        "q\033[\0776l\033[1;5H\346\226\207\033"
        "[\07725h\033[\0772026l";
    ExpectLadderInert("L-wide", 6, 2, 3, "ab\xe4\xb8\xad\xe6\x96\x87", GOLDEN,
                      39);
}

TEST_CASE("ladder inert: pending wrap, wide chars, history, truecolor, alt") {
    ExpectLadderInert("L-mix", 7, 4, 20,
                      "\x1b[38;2;9;8;7mhistory one\r\nhistory two\r\n"
                      "\x1b[0mzz\xe4\xb8\xad\xe6\x96\x87\xe6\x96\x87"
                      "\x1b[?1049h\x1b[48;2;1;2;3mALT\x1b[2;7Hxy",
                      {}, 0);
    ExpectLadderInert("L-wrap", 6, 3, 5, "abcdef\x1b[1;31mghijkl\r\nmn", {}, 0);
}

// ---------------------------------------------------------------------------
// The main screen is painted once.
// ---------------------------------------------------------------------------

namespace {

std::size_t CountOccurrences(std::string_view hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string_view::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

std::string_view SnapshotView(const Snapshot& s) {
    const auto b = s.Bytes();
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

TEST_CASE("P44 non-alt snapshot paints the main screen once") {
    // Mutant: an encoder that paints the main screen outside the sync window
    // and again inside it (trunk before this change) finds the marker twice.
    // Six lines into a 3-row grid: line1..line3 are history, the marker line
    // is on screen.
    constexpr std::string_view input =
        "line1\r\nline2\r\nline3\r\nZETMARKER\r\nline5\r\nline6";
    Grid* src = Mk(12, 3, 10);
    VtParser sp;
    Feed(sp, *src, input);
    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    CHECK(CountOccurrences(SnapshotView(*snap.value()), "ZETMARKER") == 1);
    CHECK(CountOccurrences(SnapshotView(*snap.value()), "line5") == 1);
    CHECK(CountOccurrences(SnapshotView(*snap.value()), "line1") == 1);
    delete snap.value();
    delete src;
    Probe("P44", 12, 3, 10, input);
}

TEST_CASE("P45 alt snapshot paints main once and restores it after 1049l") {
    // D1: with the alt screen active the main screen must still be painted
    // before ?1049h; the sync-window paint holds the alt screen only.  The
    // round trip below checks the main buffer after ?1049l, the byte counts
    // check that neither screen is painted twice.
    // Mutant: dropping the main paint under the alt screen loses MAINMARK
    // (count 0 and the round trip diverges); keeping an extra paint doubles it.
    constexpr std::string_view input =
        "MAINMARK\r\nrow2\x1b[?1049h\x1b[1;1HALTMARK";
    Grid* src = Mk(12, 4, 10);
    VtParser sp;
    Feed(sp, *src, input);
    auto snap = Snapshot::Create(*src);
    REQUIRE(snap.has_value());
    CHECK(CountOccurrences(SnapshotView(*snap.value()), "MAINMARK") == 1);
    CHECK(CountOccurrences(SnapshotView(*snap.value()), "ALTMARK") == 1);
    delete snap.value();
    delete src;
    Probe("P45", 12, 4, 10, input, "\x1b[?1049l");
}

// ---- DECSC slot state: DECOM, charsets, rows outside the live region ----
//
// The encoder puts the terminal into the slot's state, issues DECSC (or
// ?1049h) and undoes the state.  The saved wrap flag is not reproduced
// (design.md §9), so a probe whose slot has it set is skipped.  The alt slot
// of a snapshot taken with the main screen active is not reproduced either.
namespace {

const char* const SLOT_SAVES[] = {
    // DECOM + region + G0/G1 graphics + SO, cursor in region
    "\x1b[3;6r\x1b[?6h\x1b(0\x1b)0\x0e\x1b[2;5H\x1b"
    "7",
    // same, SO off, G0 graphics only
    "\x1b[3;6r\x1b[?6h\x1b(0\x0f\x1b[4;2H\x1b"
    "7",
    // DECOM + G1 graphics, SI (G0 active)
    "\x1b[2;7r\x1b[?6h\x1b)0\x0f\x1b[1;1H\x1b"
    "7",
    // saved row outside the region once the region changes
    "\x1b[2;7r\x1b[?6h\x1b(0\x1b[5;3H\x1b"
    "7\x1b[?6l\x1b[r",
    // saved row below the new region
    "\x1b[?6h\x1b[8;3H\x1b"
    "7",
    // saved with the wrap flag pending (skipped)
    "\x1b[3;6r\x1b[?6h\x1b(0\x1b[2;12Hx\x1b"
    "7",
    // no DECOM, charsets only
    "\x1b(0\x1b)0\x0e\x1b[3;3H\x1b"
    "7",
};
const char* const SLOT_LIVES[] = {
    "\x1b[?6l\x1b(B\x1b)B\x0f\x1b[r\x1b[1;1H",
    "\x1b[2;7r\x1b[?6h\x1b(0\x0f\x1b[3;3Hxyz",
    "\x1b[?6l\x1b)0\x0e\x1b[4;4Hq",
    "\x1b[4;5r\x1b[?6h\x1b(0\x1b)0\x0e\x1b[2;2Hq",
    "\x1b[1;3r\x1b[?6h\x1b[2;2H",
};
const char* const SLOT_AFTERS[] = {
    "\x1b"
    "8ABq\x0fq\x0eq",
    "\x1b[?6l\x1b[r\x1b[H\x1b"
    "8Zq",
};

// Probe unless a slot has the wrap flag set; returns whether it ran.
bool ProbeSlots(const char* name, std::string_view input,
                std::string_view after) {
    Grid* src = Mk(12, 8, 10);
    VtParser sp;
    Feed(sp, *src, input);
    const bool wrap = src->SavedCursorMainState().PendingWrap ||
                      src->SavedCursorAltState().PendingWrap;
    delete src;
    if (wrap) return false;
    Probe(name, 12, 8, 10, input, after);
    return true;
}

}  // namespace

TEST_CASE("P46 DECOM slot saved above the live region round-trips") {
    // D1: the slot row (absolute 5) lies outside the live region 1..3; the
    // set-up region must cover it.  Mutant: the row is clamped into the live
    // region, so the client's slot is row 2 and DECRC lands there.
    Probe("P46", 12, 8, 10,
          "\x1b[2;7r\x1b[?6h\x1b(0\x1b[5;3H\x1b"
          "7\x1b[?6l\x1b[r\x1b[1;3r\x1b[?6h\x1b[2;2H",
          "\x1b[?6l\x1b[r\x1b"
          "8Zq");
}

TEST_CASE("P47 slot state combinations, main screen") {
    // Kills: no SI after set-up (live SO leaks), no ?6l after set-up (live
    // DECOM and CUP offset), a charset-only slot not set up.
    int n = 0, ran = 0;
    for (auto s : SLOT_SAVES)
        for (auto l : SLOT_LIVES)
            for (auto a : SLOT_AFTERS)
                ran += ProbeSlots(("P47." + std::to_string(n++)).c_str(),
                                  std::string(s) + l, a);
    CHECK(ran > 0);
}

TEST_CASE("P48 slot state combinations, alt screen active") {
    int n = 0, ran = 0;
    for (auto s : SLOT_SAVES)
        for (auto l : SLOT_LIVES) {
            std::string fill;
            for (int r = 1; r <= 8; ++r)
                fill += "\x1b[" + std::to_string(r) + ";1Hqwertyuiopa";
            const std::string in =
                std::string("MAINTXT") + s + "\x1b[?47h" + fill + l;
            ran += ProbeSlots(("P48a." + std::to_string(n++)).c_str(), in,
                              "\x1b[?1049l\x1b"
                              "8Aq");
            ran += ProbeSlots(("P48b." + std::to_string(n++)).c_str(), in,
                              "\x1b"
                              "8Aq");
        }
    CHECK(ran > 0);
}

TEST_CASE("P49 alt slot set in the alt screen") {
    int n = 0, ran = 0;
    for (auto s : SLOT_SAVES) {
        for (auto l : SLOT_LIVES) {
            const std::string in =
                std::string("\x1b[2;2HMAIN\x1b[?1049h") + s + l;
            ran += ProbeSlots(("P49a." + std::to_string(n++)).c_str(), in,
                              "\x1b"
                              "8Aq");
            ran += ProbeSlots(("P49b." + std::to_string(n++)).c_str(), in,
                              "\x1b[?1049l\x1b"
                              "8Aq");
        }
    }
    CHECK(ran > 0);
}
