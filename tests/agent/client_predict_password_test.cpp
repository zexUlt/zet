/// With echo off nothing is drawn (M7, design.md §9, "Пароли"), through the
/// real client and agent: the keys of a password go to the agent and none of
/// them is ever on the terminal.
///
/// The same run with echo on, in client_predict_overlay_test.cpp, draws: a
/// client that never started predicting would pass this test for nothing.

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: keys typed with echo off are never drawn") {
    ClientRig rig{203, {.Mode = zet::predict::EDisplayMode::Always}};
    rig.Warm();

    // Enter, and a prompt that turns the echo off.
    rig.Keys("\r");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\r') != std::string::npos; }));
    rig.Print("\r\nPassword: ");
    rig.Pump(300ms);

    std::size_t seen = rig.ShellInput().size();
    for (const char key : std::string_view{"hunter2"}) {
        rig.Keys(std::string_view{&key, 1});
        REQUIRE(rig.WaitFor([&] { return rig.ShellInput().size() > seen; }));
        ++seen;
        rig.Pump(60ms);
    }
    rig.Pump(400ms);
    rig.Finish();

    CHECK(rig.Inserts().empty());
    CHECK(rig.Out() == rig.AgentStream());
    CHECK(rig.Out().find("hunter2") == std::string::npos);
}
