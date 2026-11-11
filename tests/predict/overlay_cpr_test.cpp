// Cursor position calibration on the overlay rig (design.md §9, round C, C3):
// the query, the freeze, the answer applied at the query's point.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

/// A terminal that shows the local shell on row 3, cursor after "$ ".
void ShellOnRow3(CalClient& t) { t.Before("\r\n\r\n\r\n$ "); }

/// A sequence the parser counts as unknown (checked by the test using it).
const std::string UNKNOWN = "\x1b[99z";
const std::string DCS = "\x1bP1$rabc\x1b\\";
const std::string DECRQSS = "\x1bP$qm\x1b\\";
const std::string XTGETTCAP = "\x1bP+q544e\x1b\\";
const std::string ALT_ON = "\x1b[?1049h";
const std::string ALT_OFF = "\x1b[?1049l";

/// Calibrated at the start, the epoch confirmed and a prediction visible.
void Warm(CalClient& t) {
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Key("a");
    t.Agent("a");
    t.Ack();
    t.Key("b");
    t.Agent("b");
    t.Ack();
    t.Key("c");
    REQUIRE(t.R().AnyVisible());
}

}  // namespace

TEST_CASE("The start asks once and the answer sets rows and cursor") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Resynced() == 1);
    CHECK_FALSE(t.Frozen());
    const auto& g = *t.R().G;
    CHECK(g.CursorRow() == 3);
    CHECK(g.CursorCol() == 2);
    CHECK(g.RowsTrusted());
    CHECK(g.KnownFrom(0) == 20);
    CHECK(g.KnownFrom(3) == 2);
    CHECK(t.ToAgent.empty());
    CHECK(t.WireWithoutInserts() == t.AgentStream);
    CHECK(t.R().E.ColumnTrusted());
}

TEST_CASE("A mirror seeded from a snapshot does not ask at the start") {
    CalClient t(20, 6);
    t.Begin(true);
    CHECK(t.Queries() == 0);
    t.Agent("hello");
    CHECK(t.Queries() == 0);
}

TEST_CASE("The unknown sequence the tests use is one to the parser") {
    CalClient t(20, 6);
    const auto before = t.R().P.UnknownSeqCount();
    t.Agent(UNKNOWN);
    CHECK(t.R().P.UnknownSeqCount() == before + 1);
}

TEST_CASE("Each gate holds the query back until it opens, then one goes") {
    struct Gate {
        const char* Name;
        std::string Close;
        std::string Open;
    };
    const Gate gates[] = {
        {"parser not in Ground", "\x1b[1", "m"},
        {"inside a UTF-8 character", "\xe2\x82", "\xac"},
        {"alternate screen", ALT_ON, ALT_OFF},
        {"pending wrap", "\x1b[1;20Hx", "\r"},
        {"DECOM", "\x1b[?6h", "\x1b[?6l"},
        {"synchronized output", "\x1b[?2026h", "\x1b[?2026l"},
    };
    for (const Gate& gate : gates) {
        CAPTURE(gate.Name);
        CalClient t(20, 6);
        ShellOnRow3(t);
        t.Begin(false, false);
        t.Agent(gate.Close);
        CHECK(t.Queries() == 0);
        t.Agent(gate.Open);
        CHECK(t.Queries() == 1);
        CHECK_FALSE(t.Frozen());
    }
}

TEST_CASE("No room in the stdout queue defers the query") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.QueueRoom = false;
    t.Begin();
    CHECK(t.Queries() == 0);
    t.Agent("x");
    CHECK(t.Queries() == 0);
    t.QueueRoom = true;
    t.Settle();
    CHECK(t.Queries() == 1);
}

TEST_CASE("The overlay is off the terminal before the query") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    Warm(t);
    REQUIRE_FALSE(t.Inserts.empty());
    const std::size_t draws = t.C.Draws;
    REQUIRE(draws > 0);
    t.Resize(20, 6);
    REQUIRE(t.Queries() == 2);
    // The undo is the insert right before the query; the rig also checks, as
    // the query is written, that Real is what Ref is.
    const auto& q = t.QueryInserts.back();
    const auto& before = t.Inserts[t.Inserts.size() - 2];
    CHECK(before.first + before.second == q.first);
}

TEST_CASE("Only one query is out at a time; reasons meanwhile make one more") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    REQUIRE(t.Queries() == 1);
    t.Agent(UNKNOWN);
    t.Agent(ALT_ON + ALT_OFF);
    t.Agent(DCS);
    CHECK(t.Queries() == 1);
    t.Tick(milliseconds(50));
    // The answer is in; what arose during the wait is asked once.
    CHECK(t.Queries() == 2);
    CHECK(t.Frozen());
    t.Tick(milliseconds(50));
    CHECK(t.Queries() == 2);
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Reasons() == 0);
    CHECK(t.ToAgent.empty());
}

TEST_CASE("The mirror stands still at the query's point until the answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Agent("xy");
    CHECK(t.R().G->CursorRow() == 0);
    CHECK(t.R().G->CursorCol() == 0);
    CHECK(t.C.Real.G->CursorCol() == 4);
    t.Tick(milliseconds(50));
    CHECK_FALSE(t.Frozen());
    // Calibrated where the query stood (row 3, column 2), then the output.
    CHECK(t.R().G->CursorRow() == 3);
    CHECK(t.R().G->CursorCol() == 4);
    CHECK(t.R().G->CellAt(3, 2).Codepoint == U'x');
    CHECK(t.R().G->CellAt(3, 3).Codepoint == U'y');
    CHECK(t.R().E.OutputOffset() == 2);
}

TEST_CASE("No overlay is drawn while the mirror waits") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    Warm(t);
    t.Model.Delay = milliseconds(50);
    t.Resize(20, 6);
    REQUIRE(t.Frozen());
    const std::size_t inserts = t.Inserts.size();
    t.Key("d");
    t.Key("e");
    t.Agent("de");
    t.Ack();
    CHECK(t.Inserts.size() == inserts);
    CHECK(t.ToAgent.ends_with("de"));
}

TEST_CASE("A prediction pending when the query goes out is not redrawn") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    Warm(t);
    t.Model.Delay = milliseconds(50);
    // A reason that has to wait for room, and keys typed meanwhile.
    t.QueueRoom = false;
    t.Resize(20, 6);
    REQUIRE(t.Queries() == 1);
    t.Agent("\x1b[2K\r$ ");
    t.Key("d");
    t.Agent("d");
    t.Ack();
    t.Key("e");
    t.Agent("e");
    t.Ack();
    t.Key("f");
    REQUIRE(t.R().AnyVisible());
    t.QueueRoom = true;
    t.Settle();
    REQUIRE(t.Frozen());
    // The query is the last thing on the wire: the undo took the prediction
    // off, and the mirror that waits shows nothing new.
    CHECK(t.Inserts.back() == t.QueryInserts.back());
    t.Tick(milliseconds(10));
    CHECK(t.Inserts.back() == t.QueryInserts.back());
}

TEST_CASE("Keys during the wait reach the agent at once, in order") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    const auto epoch = t.R().E.PredictionEpoch();
    t.Key("a");
    CHECK(t.ToAgent == "a");
    t.Key("b");
    CHECK(t.ToAgent == "ab");
    CHECK(t.R().E.Predictions().empty());
    CHECK(t.R().E.PredictionEpoch() > epoch);
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent == "ab");
}

TEST_CASE("The latest EchoAck is held while the mirror waits") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    Warm(t);
    t.Model.Delay = milliseconds(50);
    // A prediction that is still in flight when the query goes out.
    t.QueueRoom = false;
    t.Resize(20, 6);
    t.Agent("\x1b[2K\r$ ");
    t.Key("z");
    t.Agent("z");
    REQUIRE(t.R().E.Predictions().size() == 1);
    t.QueueRoom = true;
    t.Settle();
    REQUIRE(t.Frozen());
    t.Ack();
    t.Ack();
    CHECK(t.R().E.Predictions().size() == 1);
    t.Tick(milliseconds(50));
    CHECK_FALSE(t.Frozen());
    CHECK(t.R().E.Predictions().empty());
}

TEST_CASE("A Shift-F3 typed during the wait is not taken for the answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Stdin("\x1b[1;2R");
    t.Tick(milliseconds(50));
    CHECK_FALSE(t.Frozen());
    CHECK(t.ToAgent == "\x1b[1;2R");
    CHECK(t.Cal->Resynced() == 1);
    CHECK(t.R().G->CursorRow() == 3);
    CHECK(t.R().G->CursorCol() == 2);
}

TEST_CASE("Ctrl-F3 and Alt-F3 survive a wait that times out") {
    for (const char* key : {"\x1b[1;5R", "\x1b[1;3R"}) {
        CAPTURE(key);
        CalClient t(20, 6);
        ShellOnRow3(t);
        t.Model.Reply = EReply::Never;
        t.Begin();
        REQUIRE(t.Frozen());
        t.Stdin(key);
        t.Tick(seconds(1));
        CHECK_FALSE(t.Frozen());
        // From the timeout on, the hold of a beginning of an answer is short.
        t.Tick(CPR_HOLD);
        CHECK(t.ToAgent == key);
    }
}

TEST_CASE("Keys and the answer in one read") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Model.Reply = EReply::Never;
    t.Replies.clear();
    t.Stdin("ab" + Cpr(4, 3) + DA1 + "cd");
    CHECK(t.ToAgent == "abcd");
    CHECK_FALSE(t.Frozen());
    CHECK(t.R().G->CursorRow() == 3);
    CHECK(t.R().G->CursorCol() == 2);
}

TEST_CASE("A mirror the terminal left behind is put right by the answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Cal->Resynced() == 1);
    // The terminal moves where the mirror was not told.
    t.C.Real.Feed("\x1b[2;7H");
    t.C.Ref.Feed("\x1b[2;7H");
    t.Agent(UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Resynced() == 2);
    CHECK(t.R().G->CursorRow() == t.C.Real.G->CursorRow());
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    CHECK(t.R().G->CursorRow() == 1);
    CHECK(t.R().G->CursorCol() == 6);
    // The rest of the row is the terminal's, not the mirror's.
    CHECK(t.R().G->KnownFrom(1) == 20);
}

TEST_CASE("A matching answer changes nothing") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    const auto known = t.R().G->KnownFrom(3);
    t.Agent(UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Matched() == 1);
    CHECK(t.Cal->Resynced() == 1);
    CHECK(t.R().G->KnownFrom(3) == known);
    CHECK(t.R().G->CursorRow() == 3);
    CHECK(t.R().G->CursorCol() == 2);
}

TEST_CASE("Each reason asks exactly once") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);

    t.Resize(20, 6);
    CHECK(t.Queries() == 2);

    t.Agent(ALT_ON);
    CHECK(t.Queries() == 2);
    t.Agent(ALT_OFF);
    CHECK(t.Queries() == 3);

    t.Agent(DCS);
    CHECK(t.Queries() == 4);

    t.Agent(UNKNOWN);
    CHECK(t.Queries() == 5);

    // Questions to the terminal change nothing on the screen.
    t.Agent(DECRQSS);
    t.Agent(XTGETTCAP);
    CHECK(t.Queries() == 5);

    CHECK(t.ToAgent.empty());
    CHECK(t.WireWithoutInserts() == t.AgentStream);
}

TEST_CASE("A DCS string split over portions asks when it ends") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    t.Agent("\x1bP1$rab");
    CHECK(t.Queries() == 1);
    t.Agent("c\x1b\\");
    CHECK(t.Queries() == 2);
}

TEST_CASE("A resize during the wait ends it; the late answer is stripped") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Resize(20, 6);
    CHECK_FALSE(t.Frozen());
    // The dead entry stands until the late answer comes.
    CHECK(t.Queries() == 1);
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Queries() == 2);
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 1);
    CHECK(t.R().G->RowsTrusted());
}

TEST_CASE(
    "A silent terminal: step 5 rules after the timeout, a resize retries") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Reply = EReply::Never;
    t.Begin();
    REQUIRE(t.Queries() == 1);
    REQUIRE(t.Frozen());
    t.Key("a");
    CHECK(t.ToAgent == "a");
    t.Tick(milliseconds(999));
    CHECK(t.Frozen());
    t.Tick(milliseconds(1));
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Demux().Silent());
    CHECK(t.Cal->Resynced() == 0);
    CHECK(t.R().G->CursorRow() == 0);
    // Not asked again while silent.
    t.Agent(UNKNOWN);
    t.Agent(ALT_ON + ALT_OFF);
    CHECK(t.Queries() == 1);
    // A resize clears the mark, but the dead entry still blocks.
    t.Model.Reply = EReply::Normal;
    t.Resize(20, 6);
    CHECK_FALSE(t.Cal->Demux().Silent());
    CHECK(t.Queries() == 1);
    t.Tick(seconds(10));
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Resynced() == 1);
    CHECK_FALSE(t.Frozen());
    CHECK(t.R().G->RowsTrusted());
    CHECK(t.ToAgent == "a");
}

TEST_CASE("A terminal that answers the position only is not asked again") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Reply = EReply::CprOnly;
    t.Begin();
    REQUIRE(t.Frozen());
    t.Tick(seconds(1));
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Demux().Silent());
    t.Agent(UNKNOWN);
    CHECK(t.Queries() == 1);
}

TEST_CASE("Without calibration nothing is inserted and stdin is untouched") {
    CalClient t(20, 6, EDisplayMode::Never);
    ShellOnRow3(t);
    t.Agent("hello");
    t.Key("\x1b[1;2R");
    t.Key("abc");
    t.Resize(20, 6);
    t.Agent(UNKNOWN + ALT_ON + ALT_OFF + DCS);
    CHECK(t.Queries() == 0);
    CHECK(t.Inserts.empty());
    CHECK(t.Wire == t.AgentStream);
    CHECK(t.ToAgent == "\x1b[1;2Rabc");
}

TEST_CASE("Output past the hold limit gives no calibration") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    const std::string line(4094, 'x');
    std::size_t sent = 0;
    while (sent <= CPR_LAG_BYTES_MAX) {
        t.Agent(line + "\r\n");
        sent += line.size() + 2;
    }
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Abandoned() == 1);
    CHECK(t.Cal->Resynced() == 0);
    // The mirror has the output all the same.
    CHECK(t.R().E.OutputOffset() == t.R().Out);
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 0);
    CHECK(t.Cal->Matched() == 0);
}

TEST_CASE("A snapshot during the wait cancels it") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    t.Agent("held");
    t.Snapshot("\x1b[2J\x1b[1;1Hhi");
    CHECK_FALSE(t.Frozen());
    CHECK(t.R().G->CellAt(0, 0).Codepoint == U'h');
    CHECK(t.R().G->CursorCol() == 2);
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Resynced() == 0);
    CHECK(t.R().G->CursorRow() == 0);
    CHECK(t.R().G->CursorCol() == 2);
    // And nothing asks for the snapshot's sake.
    t.Tick(seconds(11));
    CHECK(t.Queries() == 1);
}

TEST_CASE("A snapshot while the query waits in the queue") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Blocked = true;
    t.Begin();
    REQUIRE(t.Cal->QueryUnwritten());
    t.Snapshot("\x1b[2J\x1b[1;1Hhi");
    CHECK_FALSE(t.Frozen());
    t.Unblock();
    t.Tick(milliseconds(10));
    // The query went out with the queue; its answer is stripped and ignored.
    CHECK(t.Queries() == 1);
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 0);
    CHECK(t.R().G->CursorCol() == 2);
}

TEST_CASE(
    "The query's inserts are in the log; the stream without them is the "
    "agent's") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    Warm(t);
    t.Resize(20, 6);
    t.Agent(UNKNOWN);
    t.Key("x");
    t.Agent("x");
    REQUIRE(t.Queries() >= 3);
    for (const auto& [from, len] : t.QueryInserts) {
        CHECK(t.Wire.substr(from, len) == "\x1b[6n\x1b[c");
    }
    CHECK(t.WireWithoutInserts() == t.AgentStream);
    CHECK(t.ToAgent == "abcx");
}

TEST_CASE("The client's own query is not a program's device attributes query") {
    // If it were counted as one, the terminal's answer would be taken for the
    // program's and reach the agent.
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Begin();
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Demux().Ahead() == 0);
    CHECK(t.Cal->Demux().Behind() == 0);
}

TEST_CASE("A program's own device attributes query keeps its answer") {
    CalClient t(20, 6);
    ShellOnRow3(t);
    t.Model.Delay = milliseconds(50);
    t.Begin();
    REQUIRE(t.Frozen());
    // The program asks after our query; the terminal answers in order.
    t.Agent("\x1b[c");
    t.Tick(milliseconds(50));
    CHECK(t.ToAgent.empty());
    CHECK(t.Cal->Resynced() == 1);
    t.Stdin("\x1b[?62;c");
    CHECK(t.ToAgent == "\x1b[?62;c");
}
