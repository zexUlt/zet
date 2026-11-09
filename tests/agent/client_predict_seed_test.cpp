/// A session whose first connection goes by path B (the output overflowed
/// before the client came, or ZET_TEST_FORCE_PATH): the snapshot arrives
/// before the first EchoAck, which is what starts the mirror. The mirror is
/// started from the snapshot, whose cursor is exact, so the column is known
/// and keys are predicted without a prompt having to be printed first
/// (design.md §9).
///
/// Kills: the mirror not started by the snapshot (the column stays unknown
/// until the stream sets it, and nothing is drawn).

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a first connection by path B starts the mirror "
                  "from the snapshot, column known") {
    ClientRig rig{235,
                  {.Mode = zet::predict::EDisplayMode::Always,
                   .ForcePath = session::EForcePath::B}};
    // The connection is up and has taken the snapshot of the empty screen.
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 1; }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.Out().find("\x1b[2J") != std::string::npos; },
        6000ms));
    rig.Pump(300ms);  // the snapshot is written whole, the baseline taken
    // No prompt, no CR, nothing that sets a column: the shell just echoes.
    rig.Keys("a");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('a') != std::string::npos; }));
    rig.Print("a");
    rig.Pump(350ms);
    rig.TypeAndEcho('B');
    rig.Keys("c");
    CHECK(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));
    rig.Pump(200ms);
    rig.Finish();
}
