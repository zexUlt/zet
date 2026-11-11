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
                   .TerminalPolicy = {.Ambiguous = 2}}};
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
