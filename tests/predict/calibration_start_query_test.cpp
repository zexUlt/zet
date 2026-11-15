// The start query and the terminal's tail of the cursor row (design.md §9,
// round C, "Пересинхронизация"). The start query is asked the moment the
// mirror begins, before any output reaches it, because only then is the tail of
// the cursor row known to be blank: the shell left the cursor on a fresh line.
// predict_fuzz once let the program's output come first, and its oracle failed
// on a terminal that had scrolled the shell's lines under the output.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

// The shell's three lines, the cursor on row 3 of a six-row terminal.
void ShellOnRow3(CalClient& t) {
    t.Before("ls $ file x ZZ\r\nxxxfilelsfilexZZ\r\nlsxZZfile $ file\r\n");
}

// What the fuzzer found: four line feeds scroll the shell's lines up, a
// restore-cursor with nothing saved homes the cursor, and the program
// overwrites the head of the row that now holds the shell's first line.
const std::string PROGRAM_OUTPUT(
    "\x8f\x00\n\x0b\x00\n\x00\x00\x04\x1b[2u\r\x1c#\x01\x00\x03\r$ \r", 23);
const std::string TYPED("2\xff\xff\xff\x00\x10\x00\x00\x00\xff\xff", 11);

void Run(CalClient& t) {
    t.Agent(PROGRAM_OUTPUT);
    t.Key("a");
    t.Agent("a");
    t.Key("B");
    t.Agent("B");
    t.Ack();
    t.Key(TYPED);
    // The overlay comes off with the next piece of output.
    t.Agent("");
    CHECK(SameScreen(*t.C.Real.G, *t.C.Ref.G));
}

}  // namespace

TEST_CASE("start query: asked before the output, the restore is exact") {
    CalClient t(34, 6, EDisplayMode::Always);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    REQUIRE(t.R().G->CursorRow() == 3);
    Run(t);
    // The shell's text left of the program's, under the overlay's column, is
    // still there.
    CHECK(t.C.Real.G->CellAt(0, 2).Codepoint ==
          t.C.Ref.G->CellAt(0, 2).Codepoint);
}
