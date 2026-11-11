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

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a prediction pending when the query goes out is not "
                  "redrawn while the mirror waits") {
    // The start is answered; the second query meets silence, so the wait
    // lasts CPR_TIMEOUT, long enough to look.
    ClientRig rig{
        226, {.Mode = EDisplayMode::Always, .Calibrate = true, .AnswerMax = 1}};
    rig.Warm();
    REQUIRE(rig.QueriesSeen() == 1);
    // A reason (alt-screen exit) that has to wait: DECOM is on. The shell
    // redraws its prompt and two keys are echoed; a third is predicted and
    // drawn, and the shell does not answer it.
    rig.Print("\x1b[?6h\x1b[?1049h");
    rig.Pump(50ms);
    rig.Print("\x1b[?1049l\x1b[2K\r$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('d');
    rig.TypeAndEcho('e');
    const std::size_t before = rig.Inserts().size();
    rig.Keys("f");
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == before + 1; }));
    REQUIRE(rig.QueriesSeen() == 1);
    // The gate opens: the overlay comes off, the query goes out.
    rig.Print("\x1b[?6l");
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 2; }));
    rig.Pump(300ms);
    const auto inserts = rig.Inserts();
    // The overlay came off, then the query went out: the query is not
    // written over the prediction.
    REQUIRE(inserts.size() >= before + 3);
    // The query is the last thing the client wrote: nothing put the
    // prediction back during the wait.
    CHECK(rig.InsertBytes(inserts.back()) == QUERY);
    CHECK(rig.InsertBytes(inserts[inserts.size() - 2]).find("\x1b[X") !=
          std::string::npos);
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: a lone ESC typed while the answer is awaited is "
                  "released by the timer, not by the next key") {
    ClientRig rig{
        227,
        {.Mode = EDisplayMode::Always, .Calibrate = true, .Answer = false}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ ");
    rig.Pump(100ms);
    rig.TypeAndEcho('a');
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() == 1; }));
    // The demultiplexer holds the ESC for CPR_HOLD (50 ms) in case it starts
    // the answer; nothing else wakes the client.
    rig.Keys("\x1b");
    CHECK(rig.WaitFor(
        [&] { return rig.ShellInput().find('\x1b') != std::string::npos; },
        400ms));
    rig.Finish();
}

TEST_CASE_FIXTURE(SodiumFixture, "Client: a resize is a reason to ask again") {
    const BlockWinch block;
    std::atomic<bool> asked{false};
    ClientRig rig{
        228,
        {.Mode = EDisplayMode::Always,
         .SizeSource =
             [&asked] {
                 asked = true;
                 return std::optional{wire::ResizeMessage{
                     .Rows = 24, .Cols = 100, .XPixel = 0, .YPixel = 0}};
             },
         .Calibrate = true}};
    rig.Warm();
    REQUIRE(rig.QueriesSeen() == 1);
    REQUIRE(::kill(::getpid(), SIGWINCH) == 0);
    REQUIRE(rig.WaitFor([&] { return asked.load(); }));
    CHECK(rig.WaitFor([&] { return rig.QueriesSeen() == 2; }));
    rig.Finish();
    CHECK(rig.QueriesWritten() == 2);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "Client: the start query goes out before the prompt that came "
    "with the first echo-ack") {
    ClientRig rig{231, {.Mode = EDisplayMode::Always, .Calibrate = true}};
    // The shell's prompt is already in the ring when the client connects, so
    // it arrives in the pass of the first EchoAck. ESC = is zsh's smkx.
    rig.Print("$ \x1b=");
    REQUIRE(rig.WaitFor([&] { return rig.QueriesSeen() >= 1; }));
    rig.Pump(100ms);
    const auto q = rig.Out().find(QUERY);
    const auto p = rig.Out().find("$ ");
    REQUIRE(q != std::string::npos);
    REQUIRE(p != std::string::npos);
    CHECK(q < p);
    // The first prompt line predicts at once, not only after Enter.
    rig.TypeAndEcho('a');
    rig.TypeAndEcho('B');
    rig.Keys("c");
    CHECK(rig.WaitFor([&] {
        for (const OverlayInsert& i : rig.Inserts()) {
            const std::string b = rig.InsertBytes(i);
            if (b != QUERY && b.find('c') != std::string::npos) return true;
        }
        return false;
    }));
    rig.Finish();
}
