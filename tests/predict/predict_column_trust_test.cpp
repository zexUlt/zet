// The mirror's cursor column is only trusted once something set it
// absolutely (design.md §9, "Позиционирование — CHA, не CUP"; §16
// predict_column_trust_test).
#include <doctest/doctest.h>

#include <string>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

// Output that sets the column, then a key: is it predicted?
bool PredictsAfter(const std::string& out) {
    Rig r(20, 4, EDisplayMode::Always);
    r.Output(out);
    r.Type("a");
    return r.Count() == 1;
}

}  // namespace

TEST_CASE("nothing is predicted before the column is known") {
    Rig r(20, 4, EDisplayMode::Always);
    CHECK_FALSE(r.E.ColumnTrusted());
    const auto epoch = r.E.PredictionEpoch();
    r.Type("a");
    r.Output("$ ");  // text alone says nothing about the terminal's column
    r.Type("b");
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.ColumnTrusted());
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("CR sets the column") { CHECK(PredictsAfter("$ \r$ ")); }

// An absolute row alone does not make the mirror's rows the terminal's (the
// shell's lines may be above the first one, design.md §9): it is trusted
// after the screen is cleared, as the snapshot does it.
TEST_CASE("CUP sets the column") {
    CHECK(PredictsAfter("\x1b[2J\x1b[3;4H"));
    CHECK_FALSE(PredictsAfter("\x1b[3;4H"));
}

TEST_CASE("HVP sets the column") {
    CHECK(PredictsAfter("\x1b[2J\x1b[3;4f"));
    CHECK_FALSE(PredictsAfter("\x1b[3;4f"));
}

TEST_CASE("CHA sets the column") { CHECK(PredictsAfter("\x1b[4G")); }

TEST_CASE("CNL and CPL set the column") {
    CHECK(PredictsAfter("\x1b[2J\x1b[H\x1b[1E"));
    CHECK(PredictsAfter("\x1b[2J\x1b[H\x1b[1F"));
    CHECK_FALSE(PredictsAfter("\x1b[1E"));  // the terminal may be at its bottom
    CHECK_FALSE(PredictsAfter("\x1b[1F"));
}

TEST_CASE("RIS sets the column") {
    CHECK(
        PredictsAfter("\x1b"
                      "c"));
}

TEST_CASE("a snapshot sets the column") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Gen = 1;
    r.Out = 0;
    r.E.OnSnapshot(1, r.Now, r.View());
    CHECK(r.E.ColumnTrusted());
    r.Type("a");
    CHECK(r.Count() == 1);
}

TEST_CASE("line feed alone does not set the column") {
    CHECK_FALSE(PredictsAfter("\n$ "));
}

TEST_CASE("a resize takes the column away until it is set again") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    REQUIRE(r.E.ColumnTrusted());
    REQUIRE(r.G->Resize(25, 4).has_value());
    r.E.OnResize(r.Now, r.View());
    CHECK_FALSE(r.E.ColumnTrusted());
    r.Type("a");
    CHECK(r.Count() == 0);

    r.Prompt();
    CHECK(r.E.ColumnTrusted());
}

TEST_CASE("DCS takes the column away until it is set again") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output("\x1bPq#0\x1b\\");
    CHECK_FALSE(r.E.ColumnTrusted());
    r.Type("a");
    CHECK(r.Count() == 0);
    r.Prompt();
    CHECK(r.E.ColumnTrusted());
}

TEST_CASE("losing and regaining the column in one stretch of output loses it") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output("\x1bPq#0\x1b\\\r");
    CHECK_FALSE(r.E.ColumnTrusted());
}

TEST_CASE("HPA is not modelled by the grid, so it counts as unknown") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output("\x1b[5`");
    CHECK_FALSE(r.E.ColumnTrusted());
}
