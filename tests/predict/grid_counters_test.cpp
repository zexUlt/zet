// The grid and parser counters the prediction engine reads (design.md §9).
#include <doctest/doctest.h>

#include <string_view>

#include "predict_rig.hpp"

using namespace zet::predict::test;

namespace {

struct Counters {
    std::uint64_t Scroll;
    std::uint64_t Column;
    std::uint64_t Dcs;
};

Counters Run(std::string_view s, int cols = 20, int rows = 3) {
    auto g = MakeGrid(cols, rows);
    zet::grid::VtParser p;
    p.Feed(Bytes(s).data(), s.size(), *g);
    return {g->ScrollCount(), g->ColumnSetCount(), p.DcsApcCount()};
}

}  // namespace

TEST_CASE("plain text moves no counter") {
    const auto c = Run("hello");
    CHECK(c.Scroll == 0);
    CHECK(c.Column == 0);
    CHECK(c.Dcs == 0);
}

TEST_CASE("sequences that set the column count as column sets") {
    CHECK(Run("a\r").Column == 1);
    CHECK(Run("\x1b[2;3H").Column == 1);
    CHECK(Run("\x1b[5G").Column == 1);
    CHECK(Run("\x1b[1E").Column == 1);
    CHECK(Run("\x1b[1F").Column == 1);
    CHECK(Run("\x1b"
              "c")
              .Column == 1);
}

TEST_CASE("line feed sets the column only under LNM") {
    CHECK(Run("a\n").Column == 0);
    CHECK(Run("\x1b[20h"
              "a\n")
              .Column == 1);
}

TEST_CASE("relative moves do not set the column") {
    CHECK(Run("\x1b[3C\x1b[1D\x1b[1A\x1b[1B").Column == 0);
}

TEST_CASE("scrolling counts, a no-op scroll does not") {
    CHECK(Run("\n\n\n").Scroll == 1);
    CHECK(Run("\x1b[1S").Scroll == 1);
    CHECK(Run("\x1b[1T").Scroll == 1);
    CHECK(Run("\x1b[0S").Scroll == 1);  // CSI 0 S is one line
    CHECK(Run("a\nb").Scroll == 0);
}

TEST_CASE("DCS and APC count once each, OSC not at all") {
    CHECK(Run("\x1bPq\x1b\\").Dcs == 1);
    CHECK(Run("\x1b_x\x1b\\").Dcs == 1);
    CHECK(Run("\x1b]0;t\x07").Dcs == 0);
}
