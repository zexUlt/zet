/// Row synchronisation by the cursor position report (M7 round C3, design.md
/// §9), on the real client: the query reaches the terminal as an overlay
/// insert, the terminal's answer never reaches the shell, and the modes that do
/// not predict build nothing.
///
/// Kills: the demultiplexer built for --predict=never or for ver=1; the query
/// missing from the overlay log (WithoutOverlay no longer the agent's stream);
/// the terminal's reply passed to the shell; a prediction drawn while the
/// mirror waits for the answer.

#include "client_predict_rig.hpp"

namespace {

using zet::predict::EDisplayMode;

constexpr std::string_view QUERY = "\x1b[6n\x1b[c";

bool ShellSawEscape(const ClientRig& rig) {
    return rig.ShellInput().find('\x1b') != std::string::npos;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: the first echo-ack makes one query, the answer is "
                  "stripped, the query is an overlay insert") {
    ClientRig rig{221, {.Mode = EDisplayMode::Always, .Calibrate = true}};
    rig.Warm();
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    rig.Pump(200ms);
    rig.Finish();
    CHECK(rig.QueriesWritten() == 1);
    CHECK(rig.CalibrationCreated());
    CHECK_FALSE(ShellSawEscape(rig));
    std::size_t queryInserts = 0;
    for (const OverlayInsert& i : rig.Inserts()) {
        if (rig.InsertBytes(i) == QUERY) ++queryInserts;
    }
    CHECK(queryInserts == 1);
    CHECK(rig.WithoutOverlay() == rig.AgentStream());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: --predict=never builds no demultiplexer and asks "
                  "nothing, the stream is byte-identical") {
    ClientRig rig{222, {.Mode = EDisplayMode::Never, .Calibrate = true}};
    rig.Warm();
    rig.Keys("c");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Print("c");
    rig.Pump(300ms);
    rig.Finish();
    CHECK_FALSE(rig.CalibrationCreated());
    CHECK(rig.QueriesWritten() == 0);
    CHECK(rig.Inserts().empty());
    CHECK(rig.Out() == rig.AgentStream());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a protocol ceiling of 1 builds no demultiplexer") {
    ClientRig rig{223,
                  {.Mode = EDisplayMode::Always,
                   .Ceiling = zet::wire::PROTOCOL_VERSION_MIN,
                   .Calibrate = true}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.Pump(300ms);
    rig.Finish();
    CHECK_FALSE(rig.CalibrationCreated());
    CHECK(rig.QueriesWritten() == 0);
    CHECK(rig.Out() == rig.AgentStream());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: no calibration without the option, as when stdout "
                  "is not a tty") {
    ClientRig rig{224, {.Mode = EDisplayMode::Always}};
    rig.Warm();
    rig.Pump(200ms);
    rig.Finish();
    CHECK_FALSE(rig.CalibrationCreated());
    CHECK(rig.QueriesWritten() == 0);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a silent terminal draws nothing while the mirror "
                  "waits, keys reach the shell at once, a prediction comes "
                  "after the timeout") {
    ClientRig rig{
        225,
        {.Mode = EDisplayMode::Always, .Calibrate = true, .Answer = false}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    const std::size_t inserts = rig.Inserts().size();
    const std::size_t seen = rig.ShellInput().size();
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return rig.ShellInput().size() > seen; }, 400ms));
    rig.Pump(200ms);
    // Frozen: the key is typed and nothing was drawn for it.
    CHECK(rig.Inserts().size() == inserts);
    // After CPR_TIMEOUT the mirror thaws without calibration and predicts.
    rig.Pump(1200ms);
    CHECK(rig.Inserts().size() == inserts);
    rig.TypeAndEcho('d');
    rig.TypeAndEcho('e');
    rig.Keys("f");
    CHECK(rig.WaitFor([&] { return rig.Inserts().size() > inserts; }, 2000ms));
    rig.Finish();
    CHECK(rig.QueriesWritten() == 1);
    CHECK_FALSE(ShellSawEscape(rig));
    CHECK(rig.WithoutOverlay() == rig.AgentStream());
}
