/// The client keeps the width verdicts of a session across a reconnect (M7
/// round C4): a path-B reconnect rebuilds the mirror from a snapshot, whose
/// grid knows no policy, and the verdicts must be put back.
///
/// Kills: Calibration::OnSnapshot not re-applying the verdicts; the verdicts
/// dropped with the old mirror.

#include "client_predict_rig.hpp"

namespace {

using zet::predict::EDisplayMode;

// "привет> " after a carriage return.
constexpr std::string_view RU_LINE =
    "\r\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82> ";

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a width verdict survives a path-B reconnect") {
    ClientRig rig{237,
                  {.Mode = EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .ForcePath = session::EForcePath::B,
                   .Calibrate = true,
                   .TerminalPolicy = {.Ambiguous = 2, .Cyrillic = 2}}};
    rig.Warm();
    rig.Pump(1500ms);
    REQUIRE(rig.QueriesSeen() == 0);
    // Cyrillic in the prompt: the terminal draws it two cells wide, and the
    // client asks once.
    rig.Print(RU_LINE);
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    rig.Pump(300ms);
    rig.Proxy().Cut();
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    rig.Pump(1500ms);
    // The same line again on the new mirror: the verdict is known, no query.
    rig.Print(RU_LINE);
    rig.Pump(800ms);
    CHECK(rig.QueriesSeen() == 1);
    rig.Finish();
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "Client: a path-B snapshot is laid out by the width verdict") {
    ClientRig rig{238,
                  {.Mode = EDisplayMode::Always,
                   .GiveUp = 5000ms,
                   .ForcePath = session::EForcePath::B,
                   .Calibrate = true,
                   .TerminalPolicy = {.Ambiguous = 2, .Cyrillic = 2}}};
    rig.Warm();
    rig.Pump(1500ms);
    rig.Print(RU_LINE);
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    rig.Pump(300ms);
    rig.Proxy().Cut();
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    rig.Pump(1500ms);
    // An epoch is confirmed on the row below the snapshot's line.
    rig.Print("\r\n$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    rig.Print("\r\x1b[K$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    // The mirror draws on this row: the control for the check below.
    rig.Keys("c");
    CHECK(rig.WaitFor([&] {
        for (const OverlayInsert& i : rig.Inserts()) {
            if (rig.InsertBytes(i).find("\x1b[5Gc") != std::string::npos) {
                return true;
            }
        }
        return false;
    }));
    rig.Print("c");
    rig.Pump(200ms);
    // Back on the snapshot's line, past its end: the terminal's cursor is at
    // column 14 (CHA 15), after six Cyrillic letters of two cells and "> ".
    // Two echoes there confirm an epoch again.
    rig.Print("\x1b[A\x1b[15G");
    rig.Pump(200ms);
    rig.TypeAndEcho('p');
    rig.TypeAndEcho('q');
    // The mirror holds the verdict for the letters of the snapshot's line, so
    // the key is drawn at column 16. One that laid the snapshot out at the
    // default width keeps the line under doubt and draws nothing.
    rig.Keys("d");
    CHECK(rig.WaitFor([&] {
        for (const OverlayInsert& i : rig.Inserts()) {
            if (rig.InsertBytes(i).find("\x1b[17Gd") != std::string::npos) {
                return true;
            }
        }
        return false;
    }));
    CHECK(rig.QueriesSeen() == 1);
    rig.Finish();
}
