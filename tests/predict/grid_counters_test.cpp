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

namespace {

/// Cells changed by `second` after `first` had been fed.
std::uint64_t Changed(std::string_view first, std::string_view second,
                      int cols = 20, int rows = 3) {
    auto g = MakeGrid(cols, rows);
    zet::grid::VtParser p;
    p.Feed(Bytes(first).data(), first.size(), *g);
    const std::uint64_t before = g->CellChangeCount();
    p.Feed(Bytes(second).data(), second.size(), *g);
    return g->CellChangeCount() - before;
}

}  // namespace

TEST_CASE("a cell that gets other content counts, the same content does not") {
    CHECK(Changed("", "a") == 1);
    CHECK(Changed("", "abc") == 3);
    CHECK(Changed("abc", "\rabc") == 0);  // identical rewrite
    CHECK(Changed("abc", "\rabd") == 1);
    CHECK(Changed("abc", "\r\x1b[1mabc") == 3);  // attributes are content
    CHECK(Changed("abc", "\r\x1b[31mabc") == 3);
}

TEST_CASE("an empty cell and a space are the same content") {
    CHECK(Changed("", "   ") == 0);
    CHECK(Changed("ab", "\b \b") == 1);
    CHECK(Changed("", "\x1b[K") == 0);
    CHECK(Changed("ab", "\r\x1b[K") == 2);
    CHECK(Changed("ab", "\x1b[K") == 0);
}

TEST_CASE("erases and shifts count the cells they change") {
    CHECK(Changed("abc", "\x1b[2K") == 3);
    CHECK(Changed("abc", "\x1b[2J") == 3);
    CHECK(Changed("abc", "\r\x1b[2P") == 3);  // abc -> c + blanks
    CHECK(Changed("abc", "\r\x1b[2@") == 5);  // abc -> blank blank abc
}

TEST_CASE("wide glyphs count both cells, a wide overwrite counts once more") {
    CHECK(Changed("", "\xE4\xBD\xA0") == 2);
    CHECK(Changed("\xE4\xBD\xA0", "\r\xE5\xA5\xBD") == 1);  // head only
    CHECK(Changed("\xE4\xBD\xA0", "\r\xE4\xBD\xA0") == 0);
}

TEST_CASE("cells of another row count, and so does the alternate screen") {
    CHECK(Changed("",
                  "\x1b"
                  "7\x1b[3;1HPass\x1b"
                  "8") == 4);
    // Entering the alternate screen changes no cell by itself; what is
    // written there is counted.
    CHECK(Changed("abc", "\x1b[?47h") == 0);
    CHECK(Changed("abc", "\x1b[?47hxy\x1b[?47l") == 2);
}

TEST_CASE("scrolling moves rows without counting them, only the blanked ones") {
    CHECK(Changed("abc\r\n\r\n\r\n", "\n") == 0);
}
