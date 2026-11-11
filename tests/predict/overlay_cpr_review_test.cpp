// Review additions for C3 (rv147b): tests that kill mutants the author's suite
// lets through.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {
void ShellOnRow3(CalClient& t) { t.Before("\r\n\r\n\r\n$ "); }
const std::string UNKNOWN = "\x1b[99z";
const std::string ALT_ON = "\x1b[?1049h";
const std::string ALT_OFF = "\x1b[?1049l";
}  // namespace

TEST_CASE("RV alt screen entered and left in one portion is a reason too") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Agent(ALT_ON + "x" + ALT_OFF);
    CHECK(t.Queries() == 2);
}

TEST_CASE("RV a parser that gave up on a string asks no more") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    std::string s = "\x1b]0;";
    s.append(grid::BYPASS_LIMIT_BYTES + 10, 'a');
    t.Agent(s);
    t.Agent(UNKNOWN);
    t.Agent(ALT_ON);
    t.Agent(ALT_OFF);
    CHECK(t.Queries() == 1);
}

TEST_CASE("RV a notice gives no reason") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Cal->OnTerminalBytes(Bytes(UNKNOWN), t.R().Out, t.R().Now, t.T(), true);
    t.Settle();
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Reasons() == 0);
}

TEST_CASE("RV a snapshot that holds an unknown sequence gives no reason") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Snapshot("\x1b[2J\x1b[1;1Hhi" + UNKNOWN);
    t.Agent("x");
    t.Tick(seconds(11));
    CHECK(t.Queries() == 1);
}

TEST_CASE("RV overflow while the query waits in the queue strips the answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Blocked = true;
    t.Begin();
    REQUIRE(t.Cal->QueryUnwritten());
    const std::string line(4094, 'x');
    std::size_t sent = 0;
    while (sent <= CPR_LAG_BYTES_MAX) {
        t.Agent(line + "\r\n");
        sent += line.size() + 2;
    }
    CHECK_FALSE(t.Frozen());
    t.Unblock();
    t.Tick(milliseconds(10));
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 0);
}

TEST_CASE("RV resize while the query waits in the queue strips the answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Blocked = true;
    t.Begin();
    REQUIRE(t.Cal->QueryUnwritten());
    t.Resize(20, 6);
    t.Unblock();
    t.Tick(milliseconds(10));
    CHECK(t.ToAgent.empty());
    // The resize is a reason of its own: the second query is the one that
    // calibrates.
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Resynced() == 1);
}

TEST_CASE("RV EchoAck after the wait reaches the engine once") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Tick(milliseconds(50));
    REQUIRE_FALSE(t.Frozen());
    t.Key("a");
    t.Agent("a");
    t.Ack();
    CHECK(t.R().E.Predictions().empty());
}

TEST_CASE(
    "RV a mirror whose path broke is put right even if the cursor matches") {
    CalClient t(20, 6);
    t.Begin(true);
    // A relative move the mirror cannot place breaks the path; at the top row
    // it leaves the terminal's cursor where the mirror's is.
    t.Agent("\x1b[A");
    REQUIRE_FALSE(t.R().G->RowsTrusted());
    REQUIRE(t.Queries() == 0);
    t.Agent(ALT_ON);
    t.Agent(ALT_OFF);
    REQUIRE(t.Queries() == 1);
    CHECK(t.Cal->Resynced() == 1);
    CHECK(t.Cal->Matched() == 0);
    CHECK(t.R().G->RowsTrusted());
}

TEST_CASE("RV a DCS string always resyncs, even when the cursor matches") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Cal->Resynced() == 1);
    t.Agent("\x1bP1$rabc\x1b\\");
    REQUIRE(t.Queries() == 2);
    CHECK(t.Cal->Resynced() == 2);
    CHECK(t.Cal->Matched() == 0);
}

TEST_CASE("RV output that exactly fills the hold buffer still calibrates") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    const std::string line(4094, 'x');
    for (std::size_t sent = 0; sent < CPR_LAG_BYTES_MAX;
         sent += line.size() + 2) {
        t.Agent(line + "\r\n");
    }
    CHECK(t.Frozen());
    t.Tick(milliseconds(50));
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Resynced() == 1);
    CHECK(t.Cal->Abandoned() == 0);
}

TEST_CASE("RV the start query goes before a prompt that holds ESC =") {
    CalClient t(20, 6);
    t.Before("\r\n\r\n\r\n");
    t.Begin();  // the client queries at once, ahead of the first Data
    REQUIRE(t.Queries() == 1);
    t.Agent("$ \x1b=");  // zsh: echoti smkx
    const auto& g = *t.R().G;
    CHECK(g.CursorRow() == 3);
    CHECK(g.CursorCol() == 2);
    CHECK(g.KnownFrom(3) <= g.CursorCol());
    t.Key("a");
    t.Agent("a");
    t.Ack();
    t.Key("b");
    t.Agent("b");
    t.Ack();
    t.Key("c");
    CHECK(t.R().AnyVisible());
}

TEST_CASE("RV zsh smkx and rmkx around a command line ask nothing") {
    CalClient t(20, 6);
    t.Before("\r\n\r\n\r\n");
    t.Begin();
    t.Agent("$ ");
    const auto q0 = t.Queries();
    t.Agent("\x1b[?1h\x1b=");  // zle-line-init: echoti smkx
    CHECK(t.Queries() == q0);
    t.Agent("\x1b[?1l\x1b>\r\n$ ");  // zle-line-finish: rmkx, then a prompt
    CHECK(t.Queries() == q0);
}

TEST_CASE("RV alt screen in and out in two portions asks once") {
    CalClient t(20, 6);
    t.Before("\r\n\r\n\r\n");
    t.Begin();
    t.Agent("$ ");
    const auto q0 = t.Queries();
    t.Agent(ALT_ON + "X");
    t.Agent(ALT_OFF);
    CHECK(t.Queries() == q0 + 1);
}

TEST_CASE("RV alt screen in and out inside the wait asks after it") {
    CalClient t(20, 6);
    t.Before("\r\n\r\n\r\n");
    t.Model.Delay = milliseconds(100);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Agent("$ ");
    t.Agent(ALT_ON + "X");
    t.Agent(ALT_OFF);
    // The answer ends the wait; the exit seen meanwhile asks again at once.
    t.Tick(milliseconds(100));
    CHECK(t.Queries() == 2);
}

TEST_CASE(
    "RV a snapshot during the wait leaves no EchoAck and no output held") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Key("z");
    t.Agent("held");
    t.Ack();
    CHECK(t.Cal->HoldsDeferredAck());
    CHECK(t.Cal->HoldsLag());
    t.Snapshot("\x1b[2J\x1b[1;1Hhi");
    CHECK_FALSE(t.Frozen());
    CHECK_FALSE(t.Cal->HoldsDeferredAck());
    CHECK_FALSE(t.Cal->HoldsLag());
}

TEST_CASE("RV a resize leaves a reason to ask, and the query goes out") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    // Held in the queue: the reason is there to look at.
    t.QueueRoom = false;
    t.Resize(20, 6);
    CHECK((t.Cal->Reasons() & CalResize) != 0);
    t.QueueRoom = true;
    t.Settle();
    CHECK(t.Queries() == 2);
}

TEST_CASE("RV a resize during the wait leaves a reason too") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.QueueRoom = false;
    t.Resize(20, 6);
    CHECK_FALSE(t.Frozen());
    CHECK((t.Cal->Reasons() & CalResize) != 0);
}

TEST_CASE("RV output past a small hold limit gives no calibration") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.LagMax = 40;
    t.Begin();
    REQUIRE(t.Frozen());
    t.Agent("0123456789012345678901234567890123456789");  // exactly fits
    CHECK(t.Frozen());
    t.Agent("x");
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Abandoned() == 1);
    CHECK_FALSE(t.Cal->HoldsLag());
    // The mirror has the output all the same, in order.
    CHECK(t.R().E.OutputOffset() == t.R().Out);
    CHECK(t.R().G->CellAt(0, 0).Codepoint == U'0');
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 0);
}
