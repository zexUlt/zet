// Cases that kill mutants of the C4 width code the first tests let live (the
// policy a snapshot re-applies, a width reason gone stale, a broken path that
// still asks, every unknown sequence of the parser breaking the path, a class
// given up on that must not hold back another's Wide verdict).
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
const std::string ZH = "\xd0\xb6";

void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

void StartScripted(CalClient& t) {
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    REQUIRE_FALSE(t.Frozen());
}

/// Answers whatever the terminal is asked with its own cursor.
void AnswerAsTerminal(CalClient& t) {
    for (int guard = 0; t.Frozen() && guard < 4; ++guard) {
        t.Scripted(t.C.Real.G->CursorRow(), t.C.Real.G->CursorCol());
    }
}

/// A given up on: three answers that fit no width.
void GiveUpOnA(CalClient& t) {
    for (int i = 0; i < CPR_SOLVE_FAILS_MAX; ++i) {
        t.Agent("\r" + RU + "> ");
        REQUIRE(t.Frozen());
        t.Scripted(0, 8 + 1);
    }
    REQUIRE(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unsolved);
}

constexpr std::uint8_t A_BIT = grid::WidthClassBit(EWidthClass::Cyrillic);

}  // namespace

TEST_CASE("A class given up on stays in doubt after a snapshot") {
    CalClient t(40, 6);
    StartScripted(t);
    GiveUpOnA(t);
    t.Snapshot("\x1b[2J\x1b[H$ ");
    AnswerAsTerminal(t);
    t.Agent("\r" + ZH);
    CHECK((t.R().G->WidthDoubtClasses(0) & A_BIT) != 0);
}

TEST_CASE("A class given up on stays in doubt when another gets a verdict") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(1, 2, false));
    StartScripted(t);
    GiveUpOnA(t);
    t.Model.Reply = EReply::Normal;
    t.Agent("\r" + PL + "> ");
    REQUIRE(t.Cal->Verdict(EWidthClass::PrivateUseBmp) == EWidthVerdict::Wide);
    t.Agent("\r\n" + ZH);
    CHECK((t.R().G->WidthDoubtClasses(1) & A_BIT) != 0);
}

TEST_CASE("A class given up on elsewhere does not hold back another's Wide") {
    // A is Unsolved, but not on this path: P alone is on it, and d is its n.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(1, 2, false));
    StartScripted(t);
    GiveUpOnA(t);
    t.Model.Reply = EReply::Normal;
    t.Agent("\r" + PL + PL + "> ");
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUseBmp) == EWidthVerdict::Wide);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unsolved);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
}

TEST_CASE("A snapshot re-applies every class's verdict to its rows") {
    SUBCASE("Narrow P: the marks of its glyphs in the snapshot go") {
        CalClient t(40, 6);
        Start(t);
        t.Agent("\r" + PL + "> ");
        REQUIRE(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
                EWidthVerdict::Narrow);
        t.Snapshot("\x1b[2J\x1b[H$ " + PL);
        CHECK(t.R().G->WidthDoubtClasses(0) == 0);
    }
    SUBCASE(
        "Wide P: the snapshot is laid out at the verdict, its rows are known") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 2, false));
        Start(t);
        t.Agent("\r" + PL + "> ");
        REQUIRE(t.Cal->Verdict(EWidthClass::PrivateUseBmp) ==
                EWidthVerdict::Wide);
        t.Snapshot("\x1b[2J\x1b[H$ " + PL);
        // "$ " and a glyph of two cells: the cursor is where the terminal's is.
        CHECK(t.R().G->KnownFrom(0) == 0);
        CHECK(t.R().G->CursorCol() == 4);
        CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    }
}

TEST_CASE("A width reason gone stale asks nothing") {
    // The path was restarted (CR) between the end of the portion and the
    // moment the query is allowed: there is nothing left to learn.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.QueueRoom = false;
    t.Agent("\r" + RU + "> ");
    REQUIRE((t.Cal->Reasons() & CalWidth) != 0);
    t.Agent("\r");
    t.QueueRoom = true;
    t.Settle();
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
}

TEST_CASE("A width reason gone stale is dropped when another reason asks") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.QueueRoom = false;
    t.Agent("\r" + RU + "> ");
    REQUIRE((t.Cal->Reasons() & CalWidth) != 0);
    t.Resize(40, 6);
    t.Agent("\r");
    CHECK((t.Cal->Reasons() & CalResize) != 0);
    CHECK(t.Cal->QueryDue(t.R().Now, t.T()));
    CHECK((t.Cal->Reasons() & CalWidth) == 0);
}

TEST_CASE("A path broken by a move left sends no width query") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + RU + "\x1b[D> ");
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
}

TEST_CASE("Every unknown sequence of the parser breaks the width path") {
    struct Case {
        const char* Name;
        std::string Bytes;
        bool Breaks;
    };
    const Case cases[] = {
        {"unknown ESC final (DECBI)",
         "\x1b"
         "6",
         true},
        {"ESC with an intermediate", "\x1b%G", true},
        {"CSI with a final nobody knows", "\x1b[99z", true},
        {"CSI ignored after an intermediate", "\x1b[ 1m", true},
        {"private-marker CSI that is no mode", "\x1b[?5n", true},
        {"DECSTBM with an intermediate", "\x1b[1;2 r", true},
        {"CSI p without the bang", "\x1b[1p", true},
        {"CSI q without the space", "\x1b[1q", true},
        {"DECID, which leaves the cursor where it is", "\x1bZ", false},
    };
    for (const Case& c : cases) {
        CAPTURE(std::string(c.Name));
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + RU + c.Bytes);
        CHECK(t.R().G->PathBroken() == c.Breaks);
    }
    // REP right after a resync: the mirror has no glyph to repeat.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\x1b[b" + RU);
    CHECK(t.R().G->PathBroken());
}

namespace {

constexpr std::uint8_t CLUSTER_BIT = grid::WidthClassBit(EWidthClass::Cluster);
// U+1F468 ZWJ U+1F4BB: 2 + 2 cells on the model terminal, as in the mirror.
const std::string TECH = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb";

}  // namespace

TEST_CASE("A column difference is no glyph's when something else asked too") {
    // An unknown sequence is a reason of its own: the cursor may have moved
    // for it, so the row is foreign and nothing is erased, as for any
    // mismatch that is not a width's.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\rab\x1b[99z");
    REQUIRE(t.Frozen());
    t.Scripted(0, 5);
    const auto& g = *t.R().G;
    CHECK(t.Cal->ColumnFixed() == 1);
    CHECK(g.CursorCol() == 5);
    CHECK(g.KnownFrom(0) == g.Cols());
}

TEST_CASE("A cluster mark is not asked about and stays") {
    SUBCASE("alone on the path") {
        CalClient t(40, 6);
        Start(t);
        t.Agent("\r" + TECH + "> ");
        CHECK(t.Queries() == 1);
        CHECK((t.R().G->WidthDoubtClasses(0) & CLUSTER_BIT) != 0);
    }
    SUBCASE("with a glyph of open width") {
        CalClient t(40, 6);
        Start(t);
        t.Agent("\r" + ZH + TECH + "> ");
        CHECK(t.Queries() == 1);
        CHECK((t.R().G->WidthDoubtClasses(0) & CLUSTER_BIT) != 0);
    }
    SUBCASE("with a class that has a verdict") {
        CalClient t(40, 6);
        t.SetTerminal(Terminal(2, 1, false));
        Start(t);
        t.Agent("\r" + RU + "> ");
        REQUIRE(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
        const std::size_t asked = t.Queries();
        t.Agent("\r\n" + ZH + TECH + "> ");
        CHECK(t.Queries() == asked);
        CHECK((t.R().G->WidthDoubtClasses(1) & CLUSTER_BIT) != 0);
    }
    SUBCASE("on an earlier path of the row") {
        CalClient t(40, 6);
        Start(t);
        t.Agent("\r\n" + TECH + "\r");
        t.Agent("\x1b[10C" + TECH + "> ");
        CHECK(t.Queries() == 1);
        CHECK((t.R().G->WidthDoubtClasses(1) & CLUSTER_BIT) != 0);
    }
}

TEST_CASE(
    "A matched answer to a query asked for another reason keeps the cluster "
    "mark") {
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + TECH + "\x1b[99z");
    REQUIRE(t.Frozen());
    t.Scripted(0, 4);
    CHECK(t.Cal->Matched() == 1);
    CHECK((t.R().G->WidthDoubtClasses(0) & CLUSTER_BIT) != 0);
}
