/// The width policy of the real client (M7 round C4d, design.md §9): a
/// calibrated client takes Cyrillic for one cell wide and predicts it, one that
/// asks nothing does not, a silent terminal or a wide one puts it back for the
/// session, and the policy stays through a resize and a reconnect on either
/// path.
///
/// Kills: the policy not set after the calibration is built; set without it;
/// a snapshot of the reconnect path B that drops the policy; a tripwire that
/// does not fire on the answer of a wide terminal or on a silent one; Cyrillic
/// back after a resize or a reconnect.
///
/// An echoed Cyrillic glyph the engine did not predict ends the epoch, so
/// every check of ASCII comes before the check of Cyrillic, and an epoch is
/// built again between two checks.

#include "client_predict_rig.hpp"

namespace {

using zet::predict::EDisplayMode;

const std::string YA = "\xD1\x8F";  // U+044F

std::string Escaped(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F) {
            out += c;
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", u);
            out += buf;
        }
    }
    return out;
}

/// Whether the client drew `text` as an overlay insert after the first `from`.
bool Drew(ClientRig& rig, std::size_t from, std::string_view text) {
    const auto inserts = rig.Inserts();
    for (std::size_t i = from; i < inserts.size(); ++i) {
        if (rig.InsertBytes(inserts[i]).find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A key typed, the shell echoes it, and whether the client predicted it.
bool Predicts(ClientRig& rig, std::string_view key) {
    const std::size_t before = rig.Inserts().size();
    const std::size_t seen = rig.ShellInput().size();
    rig.Keys(key);
    REQUIRE(rig.WaitFor([&] { return rig.ShellInput().size() > seen; }));
    const bool drawn =
        rig.WaitFor([&] { return Drew(rig, before, key); }, 600ms);
    if (!drawn) {
        MESSAGE("not drawn: ", key, " queries=", rig.QueriesSeen(), " tail=",
                Escaped(rig.Out().substr(
                    rig.Out().size() > 300 ? rig.Out().size() - 300 : 0)));
    }
    rig.Print(key);
    rig.Pump(350ms);
    return drawn;
}

/// How many snapshots the client has written (each one clears the screen).
std::size_t Snapshots(const ClientRig& rig) {
    std::size_t count = 0;
    for (std::size_t at = rig.Out().find("\x1b[2J"); at != std::string::npos;
         at = rig.Out().find("\x1b[2J", at + 1)) {
        ++count;
    }
    return count;
}

/// The connection is up: on path B once its snapshot is written, so the
/// mirror exists before the test types.
void Connected(ClientRig& rig, session::EForcePath path) {
    if (path == session::EForcePath::B) {
        REQUIRE(rig.WaitFor([&] { return Snapshots(rig) >= 1; }, 8000ms));
        rig.Pump(300ms);
    }
}

/// The connection is cut and comes back: path B writes another snapshot.
void Reconnected(ClientRig& rig, session::EForcePath path) {
    const std::size_t snapshots = Snapshots(rig);
    rig.Proxy().Cut();
    REQUIRE(
        rig.WaitFor([&] { return rig.Proxy().Connections() >= 2; }, 8000ms));
    if (path == session::EForcePath::B) {
        REQUIRE(
            rig.WaitFor([&] { return Snapshots(rig) > snapshots; }, 8000ms));
    }
    rig.Pump(500ms);
}

/// A fresh prompt line and two echoed keys: an epoch confirmed again.
void Epoch(ClientRig& rig) {
    rig.Print("\r\n$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a calibrated client predicts typed Cyrillic") {
    ClientRig rig{241, {.Mode = EDisplayMode::Always, .Calibrate = true}};
    rig.Warm();
    CHECK(Predicts(rig, YA));
    rig.Finish();
    CHECK(rig.CalibrationCreated());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: without calibration typed Cyrillic is not "
                  "predicted, ASCII is") {
    ClientRig rig{242, {.Mode = EDisplayMode::Always}};
    rig.Warm();
    CHECK(Predicts(rig, "c"));
    Epoch(rig);
    CHECK_FALSE(Predicts(rig, YA));
    rig.Finish();
    CHECK_FALSE(rig.CalibrationCreated());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a silent terminal puts Cyrillic back after the "
                  "timeout, ASCII is still predicted") {
    ClientRig rig{
        243,
        {.Mode = EDisplayMode::Always, .Calibrate = true, .Answer = false}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    rig.Pump(1500ms);
    rig.TypeAndEcho('d');
    rig.TypeAndEcho('e');
    CHECK(Predicts(rig, "f"));
    Epoch(rig);
    CHECK_FALSE(Predicts(rig, YA));
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: the answer of a terminal that draws Cyrillic wide "
                  "puts it back, ASCII is still predicted") {
    ClientRig rig{
        244,
        {.Mode = EDisplayMode::Always, .Calibrate = true, .TermCyrillic = 2}};
    rig.Warm();
    // Narrow until the first answer with Cyrillic on the path.
    CHECK(Predicts(rig, YA));
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() >= 2; }));
    rig.Pump(200ms);
    Epoch(rig);
    CHECK(Predicts(rig, "c"));
    Epoch(rig);
    CHECK_FALSE(Predicts(rig, YA));
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: the narrow policy stays through a reconnect on "
                  "path A and path B") {
    for (const auto path :
         {session::EForcePath::None, session::EForcePath::B}) {
        CAPTURE(static_cast<int>(path));
        ClientRig rig{245,
                      {.Mode = EDisplayMode::Always,
                       .GiveUp = 5000ms,
                       .ForcePath = path,
                       .Calibrate = true}};
        Connected(rig, path);
        rig.Warm();
        // The engine starts with the first echo-ack, a heartbeat away.
        rig.Pump(1500ms);
        REQUIRE(Predicts(rig, YA));
        Reconnected(rig, path);
        Epoch(rig);
        REQUIRE(Predicts(rig, "c"));
        Epoch(rig);
        CHECK(Predicts(rig, YA));
        rig.Finish();
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: Cyrillic put back by a wide terminal stays off "
                  "through a reconnect on path A and path B") {
    for (const auto path :
         {session::EForcePath::None, session::EForcePath::B}) {
        CAPTURE(static_cast<int>(path));
        ClientRig rig{246,
                      {.Mode = EDisplayMode::Always,
                       .GiveUp = 5000ms,
                       .ForcePath = path,
                       .Calibrate = true,
                       .TermCyrillic = 2}};
        Connected(rig, path);
        rig.Warm();
        rig.Pump(1500ms);
        std::ignore = Predicts(rig, YA);
        REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() >= 1; }));
        rig.Pump(500ms);
        Reconnected(rig, path);
        Epoch(rig);
        REQUIRE(Predicts(rig, "c"));
        Epoch(rig);
        CHECK_FALSE(Predicts(rig, YA));
        rig.Finish();
    }
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: the narrow policy stays through a resize") {
    const BlockWinch block;
    std::atomic<bool> asked{false};
    ClientRig rig{
        247,
        {.Mode = EDisplayMode::Always,
         .SizeSource =
             [&asked] {
                 asked = true;
                 return std::optional{wire::ResizeMessage{
                     .Rows = 24, .Cols = 100, .XPixel = 0, .YPixel = 0}};
             },
         .Calibrate = true}};
    rig.Warm();
    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load(); }));
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 2; }));
    rig.Pump(300ms);
    Epoch(rig);
    REQUIRE(Predicts(rig, "c"));
    Epoch(rig);
    CHECK(Predicts(rig, YA));
    rig.Finish();
}
