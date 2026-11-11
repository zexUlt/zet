/// The client's own wiring of the calibration (M7 round C3), one case for each
/// call the rig cannot see because the rig makes the same calls itself: the
/// room check before the query, the snapshot, the notice, losing the mirror.
///
/// Kills: the room check removed from MaybeQuery; Calibration::OnSnapshot not
/// called for a path-B snapshot; a notice fed as output of the program; the
/// calibration not detached when the mirror is lost.

#include "client_predict_rig.hpp"

namespace {

using zet::predict::EDisplayMode;

constexpr std::string_view QUERY = "\x1b[6n\x1b[c";

std::size_t Count(const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos;
         at = text.find(what, at + what.size())) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE_FIXTURE(
    SodiumFixture,
    "Client: a query that waits for room in a jammed stdout queue "
    "goes out once, and loses nothing") {
    const BlockWinch block;
    std::atomic<bool> asked{false};
    ClientRig rig{234,
                  {.Mode = EDisplayMode::Always,
                   .SizeSource =
                       [&asked] {
                           asked = true;
                           return std::optional{
                               wire::ResizeMessage{.Rows = 24, .Cols = 100}};
                       },
                   .Calibrate = true}};
    rig.Warm();
    REQUIRE(rig.QueriesSeen() == 1);
    // The terminal reads nothing: the stdout queue fills up and the client
    // stops reading the socket, with the end of the output still outside.
    std::string filler;
    for (int i = 0; i < 3 * 1024 * 1024 / 42; ++i) {
        filler += "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n";
    }
    rig.PrintAsync(filler + "THE-END");
    // The output stops going in when everything between the shell and the
    // terminal is full.
    std::size_t seenDone = 0;
    int still = 0;
    for (int i = 0; i < 300 && still < 4; ++i) {
        std::this_thread::sleep_for(200ms);
        still = rig.PrinterDone() == seenDone ? still + 1 : 0;
        seenDone = rig.PrinterDone();
    }
    REQUIRE(still == 4);
    REQUIRE(rig.PrinterDone() < rig.PrinterTotal());
    // A resize is a reason to ask, and it comes from the client's side.
    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load(); }, 5000ms));
    std::this_thread::sleep_for(300ms);
    // The terminal wakes up slowly, in steps smaller than the room the query
    // needs, so the room check is what decides when it goes out.
    const auto until = std::chrono::steady_clock::now() + 90s;
    while (rig.Out().find("THE-END") == std::string::npos &&
           std::chrono::steady_clock::now() < until) {
        rig.DrainSome(1024);
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(rig.Out().find("THE-END") != std::string::npos);
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 2; }, 10000ms));
    // The query went in once, into the stream, and nothing around it was
    // lost: the filler and the end are all there, in order.
    std::string rest = rig.Out();
    const std::size_t second = rest.find(QUERY, rest.find(QUERY) + 1);
    REQUIRE(second != std::string::npos);
    rest.erase(second, QUERY.size());
    rest.erase(rest.find(QUERY), QUERY.size());
    CHECK(rest.find(QUERY) == std::string::npos);
    CHECK(rest.size() >= filler.size() + 7);
    CHECK(rest.rfind("THE-END") == rest.size() - 7);
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a snapshot clears the reasons that were waiting") {
    ClientRig rig{235,
                  {.Mode = EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .ForcePath = session::EForcePath::B,
                   .Calibrate = true}};
    // The first connection is path B too: the mirror is seeded and the start
    // asks nothing.
    rig.Warm();
    // The engine starts with the first echo-ack, a heartbeat away.
    rig.Pump(1500ms);
    REQUIRE(rig.QueriesSeen() == 0);
    // An unknown sequence is a reason, DECOM keeps the query back.
    rig.Print("\x1b[?6h\x1b[99z");
    rig.Pump(200ms);
    REQUIRE(rig.QueriesSeen() == 0);
    rig.Proxy().Blackhole(true);
    rig.Print("Z");
    rig.Pump(300ms);
    rig.Proxy().Blackhole(false);
    rig.Proxy().Cut();
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.Out().find("Z") != std::string::npos; }, 8000ms));
    rig.Pump(500ms);
    // The screen was restored from the agent's: whatever made the mirror
    // doubt itself is gone with the old one.
    rig.Print("\x1b[?6l");
    rig.Pump(600ms);
    CHECK(rig.QueriesSeen() == 0);
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a notice that finishes the program's sequence is "
                  "no reason to ask") {
    ClientRig rig{236,
                  {.Mode = EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .NoticesInline = true,
                   .Calibrate = true}};
    rig.Warm();
    REQUIRE(rig.QueriesSeen() == 1);
    // The program stops inside a CSI; "zet: ..." of the notice is its final
    // byte, and the parser counts an unknown sequence, in the notice.
    rig.Print("\x1b[99");
    rig.Pump(200ms);
    rig.Proxy().Cut();
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.Out().find("reconnected") != std::string::npos; },
        8000ms));
    rig.Pump(600ms);
    CHECK(rig.QueriesSeen() == 1);
    CHECK(Count(rig.Out(), QUERY) == 1);
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a mirror that was lost takes its pending query "
                  "with it, and a key that looks like the answer is not held "
                  "for the whole timeout") {
    const BlockWinch block;
    std::atomic<int> asked{0};
    ClientRig rig{237,
                  {.Mode = EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .ForcePath = session::EForcePath::B,
                   .SizeSource =
                       [&asked] {
                           ++asked;
                           return std::optional{
                               wire::ResizeMessage{.Rows = 30, .Cols = 90}};
                       },
                   .Calibrate = true,
                   .Answer = false,
                   .LoseMirrorAtSnapshot = 2}};
    // The first snapshot seeds the mirror, the resize asks, and the terminal
    // says nothing.
    rig.Warm();
    // The engine starts with the first echo-ack, a heartbeat away.
    rig.Pump(1500ms);
    const int before = asked.load();
    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load() > before; }));
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    // The second snapshot cannot be applied: the client has no mirror, and the
    // query in the air must not go on holding what looks like its answer.
    rig.Proxy().Cut();
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    rig.Pump(100ms);
    rig.Keys("\x1b[1;2R");
    CHECK(rig.WaitFor(
        [&] { return rig.ShellInput().find("\x1b[1;2R") != std::string::npos; },
        300ms));
    rig.Finish();
}
