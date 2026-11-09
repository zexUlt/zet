/// A resize that the terminal's reflow can have turned against the mirror
/// leaves the overlay alone (design.md §9): with a drawn column outside the
/// new width, taking the overlay off would write over the wrong cell. The
/// glyph stays until the program repaints, and no byte is written.
///
/// Kills: the overlay taken off in Client::Resize whatever the new size is.

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a resize that leaves a drawn column outside the "
                  "terminal writes no restore") {
    const BlockWinch block;
    std::atomic<bool> asked{false};
    ClientRig rig{
        236,
        {.Mode = zet::predict::EDisplayMode::Always, .SizeSource = [&asked] {
             asked = true;
             return std::optional{wire::ResizeMessage{
                 .Rows = 8, .Cols = 4, .XPixel = 0, .YPixel = 0}};
         }}};
    rig.Warm();
    rig.Keys("c");  // drawn at column 4
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));

    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load(); }));
    rig.Pump(200ms);
    // CHA 5 on a four-column terminal is column 4: the restore would hit '$'
    // or ' ', not the cell that was drawn on.
    CHECK(rig.Inserts().size() == 1);
    rig.Finish();
}
