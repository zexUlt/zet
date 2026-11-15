// The Cyrillic tripwire of the calibration on the overlay rig (design.md §9,
// round C, C4d): the mirror takes Cyrillic for one cell wide from the start,
// the engine predicts it, and the calibration puts it back under the marks of
// step 5, for the rest of the session, on an answer whose column disagrees with
// Cyrillic on the path and in every state in which it stops asking. The
// terminal is a real grid whose Cyrillic is 1 or 2 cells wide, so what the
// answers say is what that terminal draws.
//
// Kills: a tripwire that fires on a row mismatch alone, without Cyrillic on the
// path, or on a reason with no expected position (start, resize, DCS); one that
// does not fire on a wide terminal, a silent one, a parser that gave up or a
// mirror with no hold buffer; Cyrillic that comes back; a confirmation that
// does not end the query reason, or that a match without Cyrillic on the path
// ends; a match that leaves the path alone; held output laid out before the
// tripwire fires.
#include <doctest/doctest.h>

#include <chrono>
#include <limits>
#include <string>

#include "calibration_rig.hpp"
#include "zet/grid/limits.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

constexpr std::uint8_t CYR_BIT =
    zet::grid::WidthClassBit(zet::grid::EWidthClass::Cyrillic);

const std::string UNKNOWN = "\x1b[99z";
const std::string DCS = "\x1bP1$rabc\x1b\\";
// U+E000, a private use glyph some terminals draw two cells wide.
const std::string PUA = "\xee\x80\x80";

/// A terminal that shows the local shell on row 3, cursor after "$ ", and the
/// client's mirror that takes Cyrillic for narrow.
void Shell(CalClient& t) {
    t.Before("\r\n\r\n\r\n$ ");
    t.CyrNarrow = true;
}

/// Confirms an epoch on a fresh prompt line, as the client's Warm does.
void Confirmed(CalClient& t) {
    t.Key("a");
    t.Agent("a");
    t.Ack();
    t.Key("b");
    t.Agent("b");
    t.Ack();
}

/// After the overlay is off, the terminal is what it would be without it.
void RealIsRef(CalClient& t) {
    t.UndoOverlay();
    CHECK(SameScreen(*t.C.Real.G, *t.C.Ref.G));
}

/// The mirror's cells right of KnownFrom are the terminal's.
void MirrorIsRef(CalClient& t) {
    const auto& m = *t.R().G;
    const auto& ref = *t.C.Ref.G;
    for (std::uint16_t r = 0; r < m.Rows(); ++r) {
        for (std::uint16_t c = m.KnownFrom(r); c < m.Cols(); ++c) {
            const auto& x = m.CellAt(r, c);
            const auto& y = ref.CellAt(r, c);
            CAPTURE(r);
            CAPTURE(c);
            CHECK(Blank(x) == Blank(y));
            if (!Blank(x) && !Blank(y)) CHECK(x.Codepoint == y.Codepoint);
        }
    }
}

}  // namespace

TEST_CASE("Cyrillic on a narrow terminal: the answer confirms it, once") {
    CalClient t(30, 8);
    Shell(t);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    CHECK_FALSE(t.CyrDoubted());
    CHECK_FALSE(t.Cal->CyrConfirmed());

    // Cyrillic on the path: one query, the answer matches, no marks.
    t.Agent("\r$ при");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Matched() == 1);
    CHECK(t.Cal->CyrConfirmed());
    CHECK_FALSE(t.CyrDoubted());
    CHECK(t.R().G->WidthDoubtClasses(3) == 0);
    CHECK(t.R().G->CursorCol() == 5);
    RealIsRef(t);
    MirrorIsRef(t);

    // Neither more lines of it nor the same line again ask.
    t.Agent("\r\n$ привет мир");
    t.Agent("\r\nпривет");
    t.Agent(" ещё");
    CHECK(t.Queries() == 2);
    CHECK_FALSE(t.CyrDoubted());
    RealIsRef(t);
    MirrorIsRef(t);

    // Typed Cyrillic is predicted, and its echo asks nothing.
    t.Agent("\r\n$ ");
    Confirmed(t);
    t.Key("я");
    REQUIRE(t.R().E.Predictions().size() == 1);
    CHECK(t.R().E.Predictions()[0].Codepoint == U'я');
    CHECK(t.R().AnyVisible());
    t.Agent("я");
    t.Ack();
    CHECK(t.Queries() == 2);
    RealIsRef(t);
    MirrorIsRef(t);
    CHECK_FALSE(t.CyrDoubted());
    CHECK(t.WireWithoutInserts() == t.AgentStream);
}

TEST_CASE(
    "Without the client's policy Cyrillic asks nothing and is not typed") {
    CalClient t(30, 8);
    t.Before("\r\n\r\n\r\n$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    CHECK(t.CyrDoubted());
    t.Agent("\r$ при");
    CHECK(t.Queries() == 1);
    CHECK(t.R().G->WidthDoubtClasses(3) == CYR_BIT);
    t.Agent("\r\n$ ");
    Confirmed(t);
    t.Key("я");
    CHECK(t.R().E.Predictions().empty());
}

TEST_CASE("Cyrillic on a wide terminal: the first answer turns it off") {
    CalClient t(30, 8);
    Shell(t);
    t.TerminalWidths(2);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    REQUIRE_FALSE(t.CyrDoubted());

    t.Agent("\r$ при");
    CHECK(t.Queries() == 2);
    CHECK(t.CyrDoubted());
    CHECK(t.Cal->Matched() == 0);
    CHECK(t.Cal->Resynced() == 2);
    CHECK_FALSE(t.Cal->CyrConfirmed());
    // The resync puts the cursor where the terminal says: 2 + 3 wide glyphs.
    CHECK(t.R().G->CursorRow() == 3);
    CHECK(t.R().G->CursorCol() == 8);
    CHECK((t.Cal->Reasons() & CalCyr) == 0);

    // The line is foreign left of the cursor; nothing is predicted on it, and
    // Cyrillic is not predicted anywhere on the main screen.
    t.Key("a");
    CHECK(t.R().E.Predictions().empty());
    t.Agent("\r\n$ ");
    Confirmed(t);
    t.Key("c");
    CHECK(t.R().E.Predictions().size() == 1);
    t.Key("я");
    CHECK(t.R().E.Predictions().size() == 1);
    CHECK(t.CyrDoubted());
}

TEST_CASE("The output held during the wait and the next line are marked") {
    CalClient t(30, 8);
    Shell(t);
    t.TerminalWidths(2);
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Model.Delay = milliseconds(50);

    t.Agent("\r$ при");
    REQUIRE(t.Frozen());
    t.Agent("вет");
    CHECK_FALSE(t.CyrDoubted());
    t.Tick(milliseconds(50));
    CHECK_FALSE(t.Frozen());
    REQUIRE(t.CyrDoubted());
    // The held output went in after the policy changed: it carries marks, and
    // the row left of the mirror's cursor is doubtful.
    CHECK(t.R().G->WidthDoubtClasses(3) == CYR_BIT);
    CHECK(t.R().G->WidthDoubtBefore(3, 11));
    CHECK(t.R().G->CursorCol() == 11);
    // The next line with Cyrillic is marked too.
    t.Agent("\r\nпривет");
    CHECK(t.R().G->WidthDoubtClasses(4) == CYR_BIT);
    CHECK(t.R().G->WidthDoubtBefore(4, 6));
    CHECK(t.Queries() == 2);
}

TEST_CASE("The tripwire stays off through a resize and a snapshot") {
    CalClient t(30, 8);
    Shell(t);
    t.TerminalWidths(2);
    t.Begin();
    t.Agent("\r$ при");
    REQUIRE(t.CyrDoubted());

    t.Resize(30, 8);
    CHECK(t.CyrDoubted());
    t.Resize(40, 10);
    CHECK(t.CyrDoubted());

    t.Snapshot("\x1b[1;1Hпривет\x1b[2;1H$ ");
    CHECK(t.CyrDoubted());
    // The rows of the snapshot that hold Cyrillic are marked.
    CHECK(t.R().G->WidthDoubtClasses(0) == CYR_BIT);
    CHECK(t.R().G->WidthDoubtClasses(1) == 0);
    t.Agent("\r\nещё");
    CHECK(t.R().G->WidthDoubtClasses(2) == CYR_BIT);
    CHECK(t.CyrDoubted());
}

TEST_CASE("A snapshot of a narrow session keeps Cyrillic narrow and unmarked") {
    CalClient t(30, 8);
    Shell(t);
    t.Begin();
    t.Snapshot("\x1b[1;1Hпривет\x1b[2;1H$ ");
    CHECK_FALSE(t.CyrDoubted());
    CHECK(t.R().G->WidthDoubtClasses(0) == 0);
    CHECK(t.R().G->CursorCol() == 2);
}

TEST_CASE("A mismatch without Cyrillic on the path does not fire it") {
    CalClient t(30, 8);
    Shell(t);
    t.TerminalWidths(1, 2);
    t.Begin();
    REQUIRE(t.Queries() == 1);

    // A wide private use glyph, then a sequence the mirror does not know: the
    // answer's column differs, and Cyrillic is not what differs.
    t.Agent("\r$ " + PUA + UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Matched() == 0);
    CHECK(t.Cal->Resynced() == 2);
    CHECK_FALSE(t.CyrDoubted());
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    CHECK(t.R().G->CursorCol() == 4);

    // The mismatch is also no confirmation.
    CHECK_FALSE(t.Cal->CyrConfirmed());
    t.Agent("\r\n$ при");
    CHECK(t.Queries() == 3);
    CHECK_FALSE(t.CyrDoubted());
}

TEST_CASE("A wide private use glyph and Cyrillic on one path turn it off") {
    CalClient t(30, 8);
    Shell(t);
    t.TerminalWidths(1, 2);
    t.Begin();
    t.Agent("\r$ " + PUA + "я" + UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.CyrDoubted());
    CHECK(t.Cal->Matched() == 0);
}

TEST_CASE("A reason with no expected position does not fire the tripwire") {
    SUBCASE("the start") {
        CalClient t(30, 8);
        Shell(t);
        t.TerminalWidths(2);
        t.Begin(false, false);
        t.Agent("\r$ при");
        CHECK(t.Queries() == 1);
        CHECK(t.Cal->Resynced() == 1);
        CHECK_FALSE(t.CyrDoubted());
        CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    }
    SUBCASE("a resize") {
        CalClient t(30, 8);
        Shell(t);
        t.TerminalWidths(2);
        t.Begin();
        REQUIRE(t.Queries() == 1);
        t.QueueRoom = false;
        t.Agent("\r$ при");
        REQUIRE(t.Queries() == 1);
        t.Resize(30, 8);
        t.QueueRoom = true;
        t.Settle();
        CHECK(t.Queries() == 2);
        CHECK_FALSE(t.CyrDoubted());
    }
    SUBCASE("a DCS string") {
        CalClient t(30, 8);
        Shell(t);
        t.TerminalWidths(2);
        t.Begin();
        t.Agent("\r$ при" + DCS);
        CHECK(t.Queries() == 2);
        CHECK(t.Cal->Resynced() == 2);
        CHECK_FALSE(t.CyrDoubted());
    }
}

TEST_CASE("A matching answer starts the path again") {
    CalClient t(30, 8);
    Shell(t);
    t.Begin();
    t.Agent("\r$ при" + UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Matched() == 1);
    // The unknown sequence is the reason; Cyrillic was on the path, and the
    // match ends the path and the query reason for the session.
    CHECK_FALSE(t.R().G->PathHasCyrillic());
    CHECK(t.Cal->CyrConfirmed());
    CHECK_FALSE(t.CyrDoubted());
}

TEST_CASE("A match without Cyrillic on the path leaves the reason alive") {
    CalClient t(30, 8);
    Shell(t);
    t.Begin();
    t.Agent(UNKNOWN);
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Matched() == 1);
    CHECK_FALSE(t.Cal->CyrConfirmed());
    // Cyrillic after it, on the same line, asks.
    t.Agent("при");
    CHECK(t.Queries() == 3);
    CHECK(t.Cal->CyrConfirmed());
    // And no more after that.
    t.Agent(" ещё\r\nещё");
    CHECK(t.Queries() == 3);
}

TEST_CASE(
    "A silent terminal turns Cyrillic off, a resize does not bring it back") {
    CalClient t(30, 8);
    Shell(t);
    t.Model.Reply = EReply::Never;
    t.Begin();
    REQUIRE(t.Frozen());
    CHECK_FALSE(t.CyrDoubted());
    t.Tick(seconds(1));
    CHECK_FALSE(t.Frozen());
    CHECK(t.CyrDoubted());

    // The terminal answers from now on, and the answer matches.
    t.Model.Reply = EReply::Normal;
    t.Resize(30, 8);
    t.Tick(seconds(10));
    t.Tick(seconds(1));
    t.Agent("\r$ при" + UNKNOWN);
    CHECK(t.CyrDoubted());
    CHECK(t.R().G->WidthDoubtClasses(3) == CYR_BIT);
    t.Agent("\r\n$ ");
    Confirmed(t);
    t.Key("c");
    CHECK(t.R().E.Predictions().size() == 1);
    t.Key("я");
    CHECK(t.R().E.Predictions().size() == 1);
}

TEST_CASE("A terminal that answers the end marker alone is silent") {
    CalClient t(30, 8);
    Shell(t);
    t.Model.Reply = EReply::Da1Only;
    t.Begin();
    CHECK(t.CyrDoubted());
}

TEST_CASE("A parser that gave up turns Cyrillic off") {
    CalClient t(30, 8);
    Shell(t);
    t.Begin();
    REQUIRE_FALSE(t.CyrDoubted());
    // A DCS string longer than the parser follows: the terminal is inside it.
    t.Agent("\x1bP" + std::string(zet::grid::BYPASS_LIMIT_BYTES + 16, 'x'));
    CHECK(t.CyrDoubted());
    CHECK((t.Cal->Reasons() & CalCyr) == 0);
}

TEST_CASE("A mirror that cannot hold the wait turns Cyrillic off") {
    CalClient t(30, 8);
    Shell(t);
    t.LagMax = std::numeric_limits<std::size_t>::max() / 2;
    t.Before("");
    t.Begin();
    CHECK(t.Queries() == 0);
    CHECK(t.CyrDoubted());
}
