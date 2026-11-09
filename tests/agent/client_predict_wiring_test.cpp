/// Wiring of the predictive echo in Client that the other client_predict_*
/// tests leave unchecked: the resize, the snapshot of path B and the inline
/// notice each reach the engine, and the overlay is off the terminal first.
///
/// Kills: the undo left out of Client::Resize (the predicted glyph stays on
/// the terminal); Predict_->OnResize left out of Client::Resize (a key typed
/// after the resize is still drawn); Predict_->OnSnapshot left out of the
/// snapshot branch of Client::PumpEvents (a key typed after path B is drawn on
/// the old epoch); the undo left out of Client::Notice.

#include <unistd.h>
#include <csignal>

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a resize takes the overlay off before the mirror "
                  "changes and stops predicting until an epoch is confirmed") {
    const BlockWinch block;
    std::atomic<bool> asked{false};
    ClientRig rig{
        231,
        {.Mode = zet::predict::EDisplayMode::Always, .SizeSource = [&asked] {
             asked = true;
             return std::optional{wire::ResizeMessage{
                 .Rows = 8, .Cols = 40, .XPixel = 0, .YPixel = 0}};
         }}};
    rig.Warm();
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));
    CHECK(ScreenOf{rig.Out()}.Row(0) == "$ aBc");  // the prediction is up

    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load(); }));
    rig.Pump(100ms);
    // Taken off, not left behind: the shell has not answered.
    CHECK(rig.Inserts().size() == 2);
    CHECK(ScreenOf{rig.Out()}.Row(0) == "$ aB");

    // The engine is reset: the next key is not drawn.
    rig.Keys("d");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('d') != std::string::npos; }));
    rig.Pump(100ms);
    CHECK(rig.Inserts().size() == 2);
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: after a snapshot (path B) the engine starts over "
                  "and a key is not drawn before an epoch is confirmed") {
    ClientRig rig{232,
                  {.Mode = zet::predict::EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .ForcePath = session::EForcePath::B}};
    // The first connection is path B too: the snapshot of what the shell has
    // printed arrives first, and the mirror starts after it with no column
    // known. A prompt drawn after it sets the column, and two echoes confirm
    // an epoch.
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    REQUIRE(rig.WaitFor(
        [&] { return rig.Out().find("$ aB") != std::string::npos; }));
    rig.Print("\r\x1b[K$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    rig.Keys("c");  // on the terminal when the link goes
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));
    // The shell prints while the client cannot hear it, so there is something
    // to replay, and path B is forced over it.
    rig.Proxy().Blackhole(true);
    rig.Print("Z");
    rig.Pump(300ms);
    rig.Proxy().Blackhole(false);
    rig.Proxy().Cut();
    rig.Pump(100ms);
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.Out().find("Z") != std::string::npos; }, 8000ms));
    rig.Pump(500ms);  // GridApplied, baseline EchoAck
    // The snapshot took the overlay off before it, whatever it repaints.
    REQUIRE(rig.Inserts().size() == 2);
    CHECK(rig.InsertBytes(rig.Inserts()[1]).find("\x1b[X") !=
          std::string::npos);
    const std::size_t before = rig.Inserts().size();

    rig.Keys("d");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('d') != std::string::npos; }));
    rig.Pump(150ms);
    CHECK(rig.Inserts().size() == before);
    rig.Print("d");
    rig.Pump(300ms);
    rig.Finish();
    CHECK(ScreenOf{rig.Out()}.Row(0).find("$ aBZd") != std::string::npos);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: an inline notice takes the overlay off first") {
    ClientRig rig{233,
                  {.Mode = zet::predict::EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .NoticesInline = true}};
    rig.Warm();
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));
    rig.Proxy().Cut();  // the notice that the link is down
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() >= 2; }));
    const auto inserts = rig.Inserts();
    // The second insert is the restore, and it comes before the notice text.
    const std::string undo = rig.InsertBytes(inserts[1]);
    CHECK(undo.find("\x1b[X") != std::string::npos);
    const std::size_t noticeAt = rig.Out().find("zet: ");
    REQUIRE(noticeAt != std::string::npos);
    CHECK(inserts[1].Offset + inserts[1].Length <= noticeAt);
    rig.Pump(600ms);
    rig.Finish();
}
