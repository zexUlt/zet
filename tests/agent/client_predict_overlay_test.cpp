/// The overlay of the real client against the real agent (M7, design.md §9):
/// a prediction is on the terminal before the echo, and what it drew is taken
/// off exactly, leaving the agent's stream as it was.
///
/// Kills: the restore before the agent's bytes left out of Deliver in
/// Client::PumpEvents (the predicted glyph stays when the program answers
/// otherwise); the Ground check left out of Overlay::BuildUpdate (the overlay
/// lands inside a sequence), see tests/predict/overlay_bytes_test.cpp.

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: always puts the predicted glyph on the terminal "
                  "before the echo and takes it off exactly") {
    ClientRig rig{201, {.Mode = zet::predict::EDisplayMode::Always}};
    rig.Warm();
    REQUIRE(rig.Inserts().empty());  // nothing is shown before an epoch is

    // The user types c; the shell does not answer yet.
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return !rig.Inserts().empty(); }));
    REQUIRE(rig.Inserts().size() == 1);
    const std::string drawn = rig.InsertBytes(rig.Inserts()[0]);
    // The glyph, then the program's last glyph B again at its own cell, so
    // that a REP from the shell repeats B and not c.
    CHECK(drawn ==
          "\x1b[?2026h\x1b[?25l\x1b[5Gc\x1b[4GB\x1b[6G\x1b[?25h\x1b[?2026l");
    // It is on the terminal before the shell has printed anything for c.
    CHECK(rig.AgentStream() == "\r$ aB");

    // The shell prints something else where the prediction was.
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Print("X");
    rig.Pump(400ms);
    rig.Finish();

    // The agent's stream is untouched by the overlay.
    CHECK(rig.WithoutOverlay() == "\r$ aBX");
    CHECK(rig.AgentStream() == "\r$ aBX");

    // The first insert is the drawing, the next one takes it off, written
    // before the X.
    const auto inserts = rig.Inserts();
    REQUIRE(inserts.size() >= 2);
    CHECK(rig.InsertBytes(inserts[1]) ==
          "\x1b[?2026h\x1b[?25l\x1b[5G\x1b[X\x1b[5G\x1b[?25h\x1b[?2026l");

    // What the terminal shows ends as the agent's stream alone shows it.
    const ScreenOf shown{rig.Out()};
    const ScreenOf plain{rig.AgentStream()};
    CHECK(shown.Row(0) == "$ aBX");
    CHECK(shown.Row(0) == plain.Row(0));
    CHECK(shown.Grid->CursorCol() == plain.Grid->CursorCol());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a correct prediction needs no redraw and the echo "
                  "ends with the agent's bytes in order") {
    ClientRig rig{202, {.Mode = zet::predict::EDisplayMode::Always}};
    rig.Warm();
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return !rig.Inserts().empty(); }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Print("c");
    rig.Pump(400ms);
    rig.Finish();
    CHECK(rig.WithoutOverlay() == "\r$ aBc");
    const ScreenOf shown{rig.Out()};
    CHECK(shown.Row(0) == "$ aBc");
}
