// The width solver of Calibration (design.md §9, round C4) on the calibration
// rig: the cases the overlay tests do not reach, and the verdicts outliving
// the mirror's snapshot, the resize and the reconnect.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

const std::string RU = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
const std::string PL = "\xee\x82\xb0";
const std::string ZWJ = "\xe2\x80\x8d";

void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

}  // namespace

TEST_CASE("A difference outside 0 and n gives no verdict") {
    CalClient t(40, 6);
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    t.Agent("\r" + RU + "> ");
    REQUIRE(t.Frozen());
    // Six glyphs of A: the answer is 6 columns further, 2 more than any
    // verdict explains (d is 0 or 6; here 3).
    t.Scripted(0, 2 + 6 + 2 + 3);
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
    CHECK(t.Cal->ColumnFixed() == 1);
    CHECK(t.R().G->CursorCol() == 13);
}

TEST_CASE("A cluster on the path with a difference of n gives no verdict") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + RU + "x" + ZWJ + "> ");
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
}

TEST_CASE("A broken path sends no width query") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    // An unknown sequence breaks the path.
    t.Agent("\r" + RU + "\x1b[99z> ");
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
}

TEST_CASE("Verdicts survive a snapshot, a resize and a second snapshot") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 2, true));
    Start(t);
    t.Agent("\r" + RU + "> ");
    t.Agent("\r" + PL + "> ");
    t.Agent("\r\xe2\x98\x81\xef\xb8\x8f> ");
    for (const EWidthClass c : {EWidthClass::Ambiguous, EWidthClass::PrivateUse,
                                EWidthClass::Emoji}) {
        REQUIRE(t.Cal->Verdict(c) == EWidthVerdict::Wide);
    }
    t.Snapshot("\x1b[2J\x1b[H$ ");
    CHECK(t.R().G->Policy() == t.Cal->Policy());
    CHECK(t.R().G->Policy().Ambiguous == 2);
    t.Resize(30, 8);
    CHECK(t.R().G->Policy() == t.Cal->Policy());
    t.Snapshot("\x1b[2J\x1b[H$ ");
    CHECK(t.R().G->Policy() == t.Cal->Policy());
    // The snapshot and the resize may ask for the position; the width never.
    t.Agent("\r" + RU + PL + "> ");
    const std::size_t asked = t.Queries();
    t.Agent("\r" + RU + PL + "> ");
    CHECK(t.Queries() == asked);
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Wide);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
}

TEST_CASE("A Narrow verdict survives a snapshot: the class stays off") {
    CalClient t(40, 6);
    Start(t);
    t.Agent("\r" + RU + "> ");
    REQUIRE(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Narrow);
    const std::size_t asked = t.Queries();
    t.Snapshot("\x1b[2J\x1b[H$ ");
    t.Agent("\r" + RU + "> ");
    CHECK(t.Queries() == asked);
    CHECK(t.R().G->WidthDoubtClasses(0) == 0);
}
