#include <doctest/doctest.h>
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/vt_parser.hpp"
using namespace zet::grid;

static bool g_failAlloc = false;
static int g_failAt = -1;
static int g_cnt = 0;

// Override the regular throwing form so that ALL array allocations in this
// binary go through malloc.  Without this, doctest uses the standard new[]
// while delete[] calls free(), which ASAN reports as alloc-dealloc-mismatch.
// The fail-alloc counters are intentionally NOT checked here: doctest and
// other support code must not be disrupted by OOM injection.
void* operator new[](std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc{};
    return p;
}
// Grid uses new(nothrow)[] exclusively; this is the interception point for OOM.
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    if (g_failAlloc) return nullptr;
    if (g_failAt >= 0 && g_cnt++ == g_failAt) return nullptr;
    return std::malloc(n ? n : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
Grid* Mk(int c = 80, int r = 24, int sb = 100) {
    auto x = Grid::Create(static_cast<std::uint16_t>(c),
                          static_cast<std::uint16_t>(r),
                          static_cast<std::uint32_t>(sb));
    REQUIRE(x.has_value());
    return x.value();
}
void F(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}
char32_t CP(const Grid& g, int r, int c) {
    return g
        .CellAt(static_cast<std::uint16_t>(r), static_cast<std::uint16_t>(c))
        .Codepoint;
}
// returns empty string if ok
std::string Inv(const Grid& g) {
    for (int r = 0; r < g.Rows(); ++r)
        for (int c = 0; c < g.Cols(); ++c) {
            auto& x = g.CellAt(static_cast<std::uint16_t>(r),
                               static_cast<std::uint16_t>(c));
            if (x.Width == 2) {
                if (c + 1 >= g.Cols())
                    return "wide at last col r" + std::to_string(r);
                if (g.CellAt(static_cast<std::uint16_t>(r),
                             static_cast<std::uint16_t>(c + 1))
                        .Width != 0)
                    return "head w/o cont r" + std::to_string(r) + " c" +
                           std::to_string(c);
            } else if (x.Width == 0) {
                if (c == 0 || g.CellAt(static_cast<std::uint16_t>(r),
                                       static_cast<std::uint16_t>(c - 1))
                                      .Width != 2)
                    return "orphan cont r" + std::to_string(r) + " c" +
                           std::to_string(c);
            }
        }
    return "";
}
}  // namespace

TEST_CASE("P2 combining") {
    VtParser p;
    Grid* g = Mk();
    F(p, *g, "e\xcc\x81");
    CHECK(CP(*g, 0, 0) == 'e');
    CHECK(g->CursorCol() == 1);
    CHECK(CP(*g, 0, 1) == 0);
    delete g;
    g = Mk();
    p = VtParser();
    F(p, *g, "ab\xcc\x81");
    CHECK(CP(*g, 0, 0) == 'a');
    CHECK(CP(*g, 0, 1) == 'b');
    CHECK(g->CursorCol() == 2);
    delete g;
    g = Mk(5, 3);
    p = VtParser();
    F(p, *g, "abcde");
    CHECK(g->PendingWrap());
    F(p, *g, "\xcc\x81");
    CHECK(CP(*g, 0, 3) == 'd');
    CHECK(CP(*g, 0, 4) == 'e');
    CHECK(g->PendingWrap());
    CHECK(g->CursorCol() == 4);
    CHECK(g->CursorRow() == 0);
    F(p, *g, "x");
    CHECK(CP(*g, 1, 0) == 'x');
    delete g;
}
TEST_CASE("P3 csi") {
    VtParser p;
    Grid* g = Mk(80, 24);
    F(p, *g, "\x1b[5;10H\x1b[3;8r");
    auto t = g->ScrollTop(), b = g->ScrollBot();
    auto cr = g->CursorRow(), cc = g->CursorCol();
    F(p, *g, "\x1b[1;2;3;4;5$r");
    CHECK(g->ScrollTop() == t);
    CHECK(g->ScrollBot() == b);
    CHECK(g->CursorRow() == cr);
    CHECK(g->CursorCol() == cc);
    for (auto s : {"\x1b[>7l", "\x1b[<7l", "\x1b[=7l", "\x1b[?1;2$r"}) {
        F(p, *g, s);
        CHECK_MESSAGE(g->ModeSet(EMode::Awm), std::string(s + 1));
        CHECK(g->ScrollTop() == t);
    }
    F(p, *g, "\x1b[?7l");
    CHECK(!g->ModeSet(EMode::Awm));
    delete g;
}
TEST_CASE("P4 tabs") {
    VtParser p;
    Grid* g = Mk(10, 5);
    REQUIRE(g->Resize(40, 5).has_value());
    F(p, *g, "\r");
    std::vector<int> v;
    for (int i = 0; i < 6; i++) {
        F(p, *g, "\t");
        v.push_back(g->CursorCol());
    }
    CHECK(v == std::vector<int>{8, 16, 24, 32, 39, 39});
    delete g;
    g = Mk(10, 5);
    REQUIRE(g->Resize(40, 5));
    VtParser q;
    F(q, *g, "\x1b[3g\x1b[1;1H");
    delete g;
}
TEST_CASE("P5 cuu cud") {
    VtParser p;
    Grid* g = Mk(80, 24);
    F(p, *g, "\x1b[3;6r\x1b[5;1H\x1b[99A");
    CHECK(g->CursorRow() == 2);
    F(p, *g, "\x1b[99B");
    CHECK(g->CursorRow() == 5);
    F(p, *g, "\x1b[2;1H\x1b[99A");
    CHECK(g->CursorRow() == 0);
    F(p, *g, "\x1b[10;1H\x1b[99B");
    CHECK(g->CursorRow() == 23);
    F(p, *g, "\x1b[10;1H\x1b[99A");
    CHECK(g->CursorRow() ==
          6);  // below region going up: edge? region bottom+1 not crossing
    delete g;
}
TEST_CASE("P6 wide") {
    VtParser p;
    Grid* g = Mk(10, 3);
    F(p, *g,
      "a\xe4\xb8\xad"
      "b\x1b[1;3H\x1b[P");
    CHECK(Inv(*g) == "");
    delete g;
    g = Mk(10, 3);
    p = VtParser();
    F(p, *g,
      "a\xe4\xb8\xad"
      "b\x1b[1;2H\x1b[P");
    CHECK(Inv(*g) == "");
    delete g;
    g = Mk(10, 3);
    p = VtParser();
    F(p, *g,
      "a\xe4\xb8\xad"
      "b\x1b[1;3H\x1b[2P");
    CHECK(Inv(*g) == "");
    delete g;
    g = Mk(10, 3);
    p = VtParser();
    F(p, *g,
      "a\xe4\xb8\xad"
      "b\x1b[1;3H\x1b[@");
    CHECK(Inv(*g) == "");
    delete g;
    g = Mk(10, 3);
    p = VtParser();
    F(p, *g,
      "a\xe4\xb8\xad"
      "b\x1b[1;3H\x1b[X");
    CHECK(Inv(*g) == "");
    // resize cutting wide
    for (int cut = 1; cut <= 6; ++cut) {
        delete g;
        g = Mk(10, 3);
        p = VtParser();
        F(p, *g, "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad");  // 6 cols
        F(p, *g, "\x1b[?1049h\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\x1b[?1049l");
        REQUIRE(g->Resize(static_cast<std::uint16_t>(cut), 3).has_value());
        CHECK_MESSAGE(Inv(*g) == "", "cut " << cut << ": " << Inv(*g));
    }
    // scrollback history w/ wide then shrink cols
    delete g;
    g = Mk(6, 2, 10);
    p = VtParser();
    F(p, *g, "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\r\n\r\n\r\n");
    REQUIRE(g->ScrollbackSize() > 0);
    REQUIRE(g->Resize(5, 2).has_value());
    {
        auto* l = g->ScrollbackLine(0);
        INFO("hist widths " << static_cast<int>(l->Cells[3].Width) << " "
                            << static_cast<int>(l->Cells[4].Width));
        CHECK(l->Cells[4].Width != 2);
    }
    // alt screen active shrink
    delete g;
    g = Mk(6, 2, 10);
    p = VtParser();
    F(p, *g, "\x1b[?1049h\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad");
    REQUIRE(g->Resize(5, 2));
    CHECK(Inv(*g) == "");
    delete g;
}
TEST_CASE("P7 resize alloc fail") {
    Grid* g = Mk(20, 10, 50);
    VtParser p;
    F(p, *g, "hello\x1b[3;7r\x1b[5;9H\x1b[?1049h\x1b[?1049l");
    for (int i = 0; i < 30; i++) F(p, *g, "line\r\n");
    auto sz = g->ScrollbackSize();
    auto cr = g->CursorRow(), cc = g->CursorCol();
    auto t = g->ScrollTop(), b = g->ScrollBot();
    g_failAlloc = true;
    auto r = g->Resize(30, 5);
    auto r2 = g->Resize(10, 20);
    g_failAlloc = false;
    CHECK(!r.has_value());
    CHECK(!r2.has_value());
    CHECK(g->Cols() == 20);
    CHECK(g->Rows() == 10);
    CHECK(g->ScrollbackSize() == sz);
    CHECK(g->CursorRow() == cr);
    CHECK(g->CursorCol() == cc);
    CHECK(g->ScrollTop() == t);
    CHECK(g->ScrollBot() == b);
    CHECK(Inv(*g) == "");
    // Resize that fails only in history allocation: fail Nth alloc
    delete g;
}
TEST_CASE("P8 limits") {
    auto r = Grid::Create(4096, 1024, 1000);  // 4096*1000*sizeof(Cell)
    INFO("sizeof Cell " << sizeof(Cell));
    CHECK(!r.has_value());
    if (r.has_value()) delete r.value();
    auto r2 = Grid::Create(4096, 1024, 100000000);
    CHECK(!r2.has_value());
    auto r3 = Grid::Create(80, 24, 1000000);
    INFO("80x24 sb1M ok=" << r3.has_value());
    if (r3.has_value()) delete r3.value();
    // Resize growing cols with big scrollback: history can't be reallocated ->
    // error, state unchanged
    auto g = Mk(80, 24, 100000);
    auto rr = g->Resize(4096, 24);
    INFO("grow cols with sb100000: ok=" << rr.has_value());
    delete g;
}
TEST_CASE("misc defects") {
    // wide at last col, with Resize growing/shrinking
    VtParser p;
    Grid* g = Mk(4, 2);
    F(p, *g, "abc\xe4\xb8\xad");
    CHECK(Inv(*g) == "");
    F(p, *g, "\x1b[1;4H\xe4\xb8\xad");
    CHECK(Inv(*g) == "");
    delete g;
    // 1x1 wide
    g = Mk(1, 1, 0);
    F(p, *g,
      "\xe4\xb8\xad"
      "ab\x1b[P\x1b[@\x1b[1L\x1b[1M");
    CHECK(Inv(*g) == "");
    delete g;
    // 2 cols wide then RepeatChar/ECH
    g = Mk(3, 1, 0);
    VtParser q;
    F(q, *g, "a\xe4\xb8\xad\x1b[3b\x1b[1;2H\x1b[X\x1b[1;3H\x1b[1X");
    CHECK(Inv(*g) == "");
    delete g;
    // IRM insert wide
    g = Mk(5, 1, 0);
    VtParser q2;
    F(q2, *g, "\xe4\xb8\xad\xe4\xb8\xad\x1b[4h\x1b[1;1Hab\xe4\xb8\xad");
    CHECK(Inv(*g) == "");
    delete g;
    // saved cursor after resize
    g = Mk(20, 20, 0);
    VtParser q3;
    F(q3, *g,
      "\x1b[15;15H\x1b"
      "7");
    REQUIRE(g->Resize(5, 5));
    F(q3, *g,
      "\x1b"
      "8");
    CHECK(g->CursorRow() < 5);
    CHECK(g->CursorCol() < 5);
    delete g;
    // 0 args etc
    g = Mk(10, 5, 0);
    VtParser q4;
    F(q4, *g,
      "\x1b[0;0r\x1b[999;999r\x1b[5;2r\x1b[0A\x1b[0B\x1b[?1049h\x1b[?1049h\x1b["
      "?1049l\x1b[?1049l\x1b[99999999999A");
    CHECK(g->CursorRow() < 5);
    delete g;
}

#include <random>
TEST_CASE("Q targeted") {
    struct C {
        const char* name;
        const char* seq;
        int cols;
    };
    C cs[] = {
        {"overwrite head narrow",
         "\xe4\xb8\xad\r"
         "a",
         6},
        {"overwrite head wide", "\xe4\xb8\xad\r\xe4\xb8\xad", 6},
        {"overwrite cont narrow", "\xe4\xb8\xad\x1b[1;2Ha", 6},
        {"EL1 mid wide", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;2H\x1b[1K", 6},
        {"EL0 mid wide", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;2H\x1b[0K", 6},
        {"ECH on head", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;1H\x1b[1X", 6},
        {"ECH on cont", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;2H\x1b[1X", 6},
        {"ICH at wide", "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\x1b[1;1H\x1b[1@",
         6},
        {"ICH push head to last col",
         "ab\xe4\xb8\xad\xe4\xb8\xad\x1b[1;1H\x1b[1@", 6},
        {"ICH push cont off", "abcd\xe4\xb8\xad\x1b[1;1H\x1b[1@", 6},
        {"DCH 1 at col0 before wide", "a\xe4\xb8\xad\x1b[1;1H\x1b[1P", 6},
        {"DCH on cont",
         "a\xe4\xb8\xad"
         "bc\x1b[1;3H\x1b[1P",
         6},
        {"DCH big",
         "a\xe4\xb8\xad"
         "bc\x1b[1;3H\x1b[99P",
         6},
        {"REP wide", "\xe4\xb8\xad\x1b[5b", 6},
        {"IRM wide at edge", "\xe4\xb8\xad\xe4\xb8\xad\x1b[4h\x1b[1;1Hab", 5},
        {"IRM wide ins", "ab\x1b[4h\x1b[1;1H\xe4\xb8\xad", 5},
        {"ED1 mid wide", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;2H\x1b[1J", 6},
        {"wide AWM off at edge",
         "\x1b[?7l"
         "12345\xe4\xb8\xad",
         6},
        {"wide in cols=5 pending", "abcde\xe4\xb8\xad", 5},
        {"wide, cols 4 DCH", "\xe4\xb8\xad\xe4\xb8\xad\x1b[1;4H\x1b[1P", 4},
        {"tab over wide ok", "\xe4\xb8\xad\t", 6},
        {"IL/DL after", "\xe4\xb8\xad\x1b[1L\x1b[1M", 6},
        {"SGR wide", "\x1b[1m\xe4\xb8\xad\x1b[0m", 6},
    };
    for (auto& c : cs) {
        VtParser p;
        Grid* g = Mk(c.cols, 3, 0);
        F(p, *g, c.seq);
        CHECK_MESSAGE(Inv(*g) == "", std::string(c.name) << " -> " << Inv(*g));
        delete g;
    }
}
TEST_CASE("R random invariant fuzz") {
    std::mt19937 rng(12345);
    const char* toks[] = {"a",
                          "b",
                          "\xe4\xb8\xad",
                          "\xe4\xb8\xad",
                          "\xcc\x81",
                          "\r",
                          "\n",
                          "\t",
                          "\b",
                          "\x1b[P",
                          "\x1b[2P",
                          "\x1b[@",
                          "\x1b[3@",
                          "\x1b[X",
                          "\x1b[3X",
                          "\x1b[K",
                          "\x1b[1K",
                          "\x1b[2K",
                          "\x1b[J",
                          "\x1b[1J",
                          "\x1b[2J",
                          "\x1b[H",
                          "\x1b[1;2H",
                          "\x1b[2;3H",
                          "\x1b[1;3H",
                          "\x1b[2D",
                          "\x1b[C",
                          "\x1b[2C",
                          "\x1b[L",
                          "\x1b[M",
                          "\x1b[S",
                          "\x1b[T",
                          "\x1b[2;3r",
                          "\x1b[r",
                          "\x1b"
                          "7",
                          "\x1b"
                          "8",
                          "\x1b[?1049h",
                          "\x1b[?1049l",
                          "\x1b[4h",
                          "\x1b[4l",
                          "\x1b[?7l",
                          "\x1b[?7h",
                          "\x1b[5b",
                          "\x1b[?6h",
                          "\x1b[?6l",
                          "\x1b"
                          "M",
                          "\x1b"
                          "D",
                          "\x1b"
                          "E",
                          "\x1b[1m",
                          "\x1b[0m"};
    int n = static_cast<int>(sizeof(toks) / sizeof(*toks));
    std::map<std::string, int> kinds;
    std::string firstRepro[8];
    for (int iter = 0; iter < 20000; ++iter) {
        int cols = 1 + static_cast<int>(rng() % 9),
            rows = 1 + static_cast<int>(rng() % 6);
        Grid* g = Mk(cols, rows, static_cast<int>(rng() % 2) ? 0 : 10);
        VtParser p;
        std::string hist;
        for (int s = 0; s < 12; ++s) {
            std::string t;
            if (rng() % 10 == 0) {
                int nc = 1 + static_cast<int>(rng() % 9),
                    nr = 1 + static_cast<int>(rng() % 6);
                auto r = g->Resize(static_cast<std::uint16_t>(nc),
                                   static_cast<std::uint16_t>(nr));
                REQUIRE(r.has_value());
                t = "RESIZE(" + std::to_string(nc) + "x" + std::to_string(nr) +
                    ")";
            } else {
                t = toks[static_cast<std::size_t>(rng() %
                                                  static_cast<unsigned>(n))];
                F(p, *g, t);
            }
            hist +=
                (t[0] == '\x1b' ? "ESC" + t.substr(1)
                                : (t[0] == 'R' ? t
                                               : (t == "\r"             ? "CR"
                                                  : t == "\n"           ? "LF"
                                                  : t == "\t"           ? "TAB"
                                                  : t == "\b"           ? "BS"
                                                  : t == "\xe4\xb8\xad" ? "W"
                                                                        : t))) +
                " ";
            if (g->CursorRow() >= g->Rows() || g->CursorCol() >= g->Cols()) {
                kinds["cursor oob"]++;
            }
            if (g->PendingWrap() && g->CursorCol() != g->Cols() - 1)
                kinds["pending not last col"]++;
            auto e = Inv(*g);
            if (!e.empty()) {
                auto k = e.substr(0, e.find(" r"));
                if (kinds[k]++ == 0) {
                    MESSAGE(k << " [" << cols << "x" << rows << "]: " << hist);
                }
                break;
            }
        }
        delete g;
    }
    for (auto& [k, v] : kinds) MESSAGE("kind " << k << " count " << v);
}

TEST_CASE("S nth alloc fail in Resize") {
    for (int n = 0; n < 8; ++n) {
        Grid* g = Mk(20, 10, 50);
        VtParser p;
        for (int i = 0; i < 30; i++) F(p, *g, "line\r\n");
        F(p, *g, "\x1b[3;7r\x1b[5;9H");
        auto sz = g->ScrollbackSize();
        auto cr = g->CursorRow(), cc = g->CursorCol();
        auto t = g->ScrollTop(), b = g->ScrollBot();
        char32_t c0 = CP(*g, 0, 0);
        g_cnt = 0;
        g_failAt = n;
        auto r = g->Resize(30, 5);
        g_failAt = -1;
        INFO("fail at alloc " << n << " ok=" << r.has_value());
        if (!r.has_value()) {
            CHECK(g->Cols() == 20);
            CHECK(g->Rows() == 10);
            CHECK(g->ScrollbackSize() == sz);
            CHECK(g->CursorRow() == cr);
            CHECK(g->CursorCol() == cc);
            CHECK(g->ScrollTop() == t);
            CHECK(g->ScrollBot() == b);
            CHECK(CP(*g, 0, 0) == c0);
        }
        delete g;
    }
    // shrink rows w/ history: failing alloc must not push scrollback
    Grid* g = Mk(20, 10, 50);
    VtParser p;
    F(p, *g, "abc");
    g_failAlloc = true;
    auto r = g->Resize(20, 4);
    g_failAlloc = false;
    CHECK(!r);
    CHECK(g->ScrollbackSize() == 0);
    delete g;
}

TEST_CASE("T limits") {
    auto r = Grid::Create(4096, 1024, 10000);
    CHECK(!r.has_value());
    if (r) delete r.value();
    auto r2 = Grid::Create(4096, 1024, 0x7FFFFFFF);
    CHECK(!r2.has_value());
    auto r3 = Grid::Create(4096, 1, 8192);
    CHECK(r3.has_value() == false);  // 4096*8192*16=512MB
    if (r3) delete r3.value();
    Grid* g =
        Mk(80, 24,
           100000);  // 128MB budget: 100000*80*16=128,000,000 <= 134,217,728
    auto rr = g->Resize(100, 24);
    INFO("grow cols with sb100000 ok=" << rr.has_value()
                                       << " cols=" << g->Cols());
    CHECK(!rr.has_value());
    CHECK(g->Cols() == 80);
    delete g;
}
