// What the engine makes of the byte stream the user types: keys split across
// calls, escape sequences, UTF-8.
#include <doctest/doctest.h>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("printable ASCII is predicted at the cursor and the cursor follows") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("hi");
    REQUIRE(r.Count() == 2);
    CHECK(r.E.Predictions()[0].Col == 2);
    CHECK(r.E.Predictions()[0].Codepoint == U'h');
    CHECK(r.E.Predictions()[1].Col == 3);
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 4);
    CHECK(r.E.Cursor()->Row == 0);
    CHECK(r.E.Cursor()->InputEnd == r.In);
}

TEST_CASE("a multibyte character split between calls is one prediction") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("\xc3");
    CHECK(r.Count() == 0);
    r.Type("\xa9");
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Codepoint == U'é');
    CHECK(r.E.Predictions()[0].InputEnd == 2);
}

TEST_CASE("an escape sequence split between calls is not typed text") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("\x1b");
    r.Type("[");
    r.Type("1;5");
    r.Type("D");
    CHECK(r.Count() == 0);
    r.Type("a");
    CHECK(r.Count() == 1);
}

TEST_CASE("ESC in the middle of a sequence starts another") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("\x1b[1\x1b[D");
    CHECK(r.Count() == 0);
    r.Type("a");
    CHECK(r.Count() == 1);
}

TEST_CASE("a control byte ends a malformed sequence and counts as a key") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    const auto epoch = r.E.PredictionEpoch();
    r.Type("\x1b[1\x03");
    CHECK(r.E.PredictionEpoch() > epoch + 1);
    r.Type("a");
    CHECK(r.Count() == 1);
}

TEST_CASE("an endless CSI parameter list is bounded and not paste") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    std::string s = "\x1b[";
    s.append(1000, '2');
    s += "~";
    r.Type(s);
    r.Type("a");
    CHECK(r.Count() == 1);  // not stuck in paste
}

TEST_CASE("a CSI 200 with a second parameter is not the paste marker") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("\x1b[200;1~");
    r.Type("a");
    CHECK(r.Count() == 1);
}

TEST_CASE("typing after a prediction continues from the predicted cursor") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Output("a");  // echo arrives before the ack: the mirror cursor is at 3
    r.Type("b");    // still one cell after "a", not two
    REQUIRE(r.Count() == 2);
    CHECK(r.E.Predictions()[1].Col == 3);
}
