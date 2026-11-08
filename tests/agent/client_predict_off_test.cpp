/// When the client does not predict (M7, design.md §9, "Согласование и
/// совместимость"): --predict=never, a protocol ceiling of 1, adaptive on a
/// path with no delay. The stream reaches the terminal as it always did, and
/// with `never` the Hello says nothing of ECHO_ACK.
///
/// Kills: the echoAck argument of ClientConnection in Client::FinishConnecting
/// true for never (the TLV is offered); the mirror and engine started for never
/// (an overlay appears); the SRTT thresholds ignored in adaptive.

#include "client_predict_rig.hpp"

namespace {

constexpr std::uint16_t TLV_GRID = zet::wire::TLV_GRID_CAPABLE;
constexpr std::uint16_t TLV_ECHO = zet::wire::TLV_ECHO_ACK;

/// Types c at a warmed-up prompt, lets the shell wait `wait`, and answers.
void TypeAhead(ClientRig& rig, std::chrono::milliseconds wait) {
    rig.Warm();
    rig.Keys("c");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Pump(wait);
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: --predict=never offers no ECHO_ACK and the stream "
                  "is byte-identical") {
    ClientRig rig{211, {.Mode = zet::predict::EDisplayMode::Never}};
    TypeAhead(rig, 400ms);
    rig.Print("c");
    rig.Pump(300ms);
    CHECK(rig.HelloOffers(TLV_GRID));  // the probe itself finds a TLV
    CHECK_FALSE(rig.HelloOffers(TLV_ECHO));
    rig.Finish();
    CHECK(rig.Inserts().empty());
    CHECK(rig.Out() == "\r$ aBc");
    CHECK(rig.Out() == rig.AgentStream());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: adaptive offers ECHO_ACK and draws nothing at low "
                  "RTT") {
    ClientRig rig{212, {.Mode = zet::predict::EDisplayMode::Adaptive}};
    TypeAhead(rig, 120ms);  // under the glitch threshold
    CHECK(rig.HelloOffers(TLV_ECHO));
    CHECK(rig.Inserts().empty());
    rig.Print("c");
    rig.Pump(300ms);
    rig.Finish();
    CHECK(rig.Inserts().empty());
    CHECK(rig.Out() == "\r$ aBc");
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: adaptive shows a prediction nobody answers for "
                  "longer than the glitch threshold") {
    ClientRig rig{213, {.Mode = zet::predict::EDisplayMode::Adaptive}};
    rig.Warm();
    // The agent's answers, EchoAck included, do not get through: a program
    // that is slow, as far as the client can tell.
    rig.Proxy().Blackhole(true);
    rig.Keys("c");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Pump(100ms);
    CHECK(rig.Inserts().empty());
    // The client wakes by itself at the threshold, with nothing else to do.
    CHECK(rig.WaitFor([&] { return !rig.Inserts().empty(); }, 3000ms));
    rig.Finish();
    CHECK(rig.Out().find("\x1b[5Gc") != std::string::npos);
    CHECK(rig.WithoutOverlay() == "\r$ aB");
    const ScreenOf shown{rig.Out()};
    CHECK(shown.Row(0) == "$ aB");  // taken off when the client left
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a protocol ceiling of 1 is as never, and always "
                  "says so once") {
    ClientRig rig{214,
                  {.Mode = zet::predict::EDisplayMode::Always,
                   .Ceiling = zet::wire::PROTOCOL_VERSION_MIN}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    rig.Keys("c");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Pump(300ms);
    rig.Print("c");
    rig.Pump(200ms);
    CHECK_FALSE(rig.HelloOffers(TLV_ECHO));
    rig.Finish();
    CHECK(rig.Inserts().empty());
    CHECK(rig.Out() == "\r$ aBc");
    const std::string err = rig.ErrText();
    CHECK(err.find("predictive echo is off") != std::string::npos);
    CHECK(err.find("predictive echo is off") ==
          err.rfind("predictive echo is off"));
}
