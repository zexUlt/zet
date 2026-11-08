// The client keeps OVERLAY_UNDO_BYTES_MAX free in its stdout queue ahead of
// every read, because the restore before the agent's bytes cannot be left out
// (design.md §9). This is the bound that makes the reserve enough: the worst
// restore there can be, PREDICTIONS_MAX cells each in a pen of its own with
// every attribute and truecolours, fits in it.
#include <doctest/doctest.h>

#include <string>

#include "predict_rig.hpp"
#include "zet/predict/overlay.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("the worst undo fits in OVERLAY_UNDO_BYTES_MAX") {
    constexpr int COLS = PREDICTIONS_MAX + 8;
    Rig r(COLS, 3, EDisplayMode::Always);
    auto overlay = Overlay::Create();
    REQUIRE(overlay != nullptr);

    // A row of blanks, each in other colours, with every attribute: what a
    // restore of an empty cell has to say again for every one of them.
    std::string row = "\r";
    for (int c = 0; c < COLS; ++c) {
        const std::string n = std::to_string(c % 200 + 30);
        row += "\x1b[0;1;2;3;4;5;7;9;38;2;" + n + ";" + n + ";250;48;2;" + n +
               ";9;" + n + ";58;2;" + n + ";" + n + ";1m ";
    }
    row += "\x1b[0m\r";
    r.Output(row);
    r.Output("\x1b[1G");  // the column is known
    // Two echoed keys of different glyphs confirm an epoch.
    r.Type("a");
    r.Output("a");
    r.Ack();
    r.Type("B");
    r.Output("B");
    r.Ack();
    // Then every column of the row is typed over, alternating glyphs.
    std::string keys;
    for (std::size_t i = 0; i < PREDICTIONS_MAX; ++i) {
        keys.push_back(static_cast<char>('c' + i % 20));
    }
    for (const char k : keys) r.Type(std::string_view{&k, 1});
    REQUIRE(r.Count() > 100);

    const auto drawn = overlay->BuildUpdate(r.E, r.View());
    REQUIRE_FALSE(drawn.empty());
    overlay->Commit(r.E);
    CHECK(drawn.size() <= OVERLAY_BYTES_MAX);

    const auto undo = overlay->BuildUndo(r.View());
    CHECK_FALSE(undo.empty());
    CHECK(undo.size() > 8000);  // a pen of its own for each of 256 cells
    CHECK(undo.size() <= OVERLAY_UNDO_BYTES_MAX);
    // A restore that did not fit would be returned empty (Overflow): the
    // drawn cells are all still there to take off.
    overlay->Commit(r.E);
    CHECK_FALSE(overlay->Drawn());
}
