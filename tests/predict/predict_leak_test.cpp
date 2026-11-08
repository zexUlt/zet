// Password leaks found in review of PR 137: output on the cursor row that is
// not the echo of a prediction raises the epoch, and mask glyphs never earn
// credit (design.md §9 "Пароли"; §16 predict_leak_test).
#include <doctest/doctest.h>

#include <string_view>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

constexpr std::string_view BULLET = "\xE2\x80\xA2";
constexpr std::string_view BLACK_CIRCLE = "\xE2\x97\x8F";

}  // namespace

TEST_CASE(
    "typeahead echoed after Enter confirms the epoch, then a prompt prints on "
    "the same row") {
    for (const auto mode : {EDisplayMode::Always, EDisplayMode::Adaptive}) {
        Rig r(40, 4, mode);
        r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
        r.Prompt();
        r.ConfirmEpoch();
        r.Type("\r");      // e.g. `make && sudo make install`
        r.Output("\r\n");  // the command starts and runs long
        r.Type("xy");      // the user pokes keys; the tty echoes them
        r.Output("xy");
        r.Ack();
        REQUIRE(r.E.ConfirmedEpoch() >= r.E.PredictionEpoch());
        r.Output(
            "[sudo] password: ");  // sudo prompts on the same row, echo off
        r.Type("h");
        CHECK_FALSE(r.AnyVisible());
        r.Type("unter2");
        CHECK_FALSE(r.AnyVisible());
    }
}

TEST_CASE("a prompt overwriting the row from the left also raises the epoch") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    const auto before = r.E.PredictionEpoch();
    r.Output("\rPassword: ");
    CHECK(r.E.PredictionEpoch() > before);
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("a cursor jump or an erase in the cursor row raises the epoch") {
    for (const char* seq : {"\x1b[3G", "\x1b[2K", "\x1b[3D", "\r"}) {
        Rig r(40, 4, EDisplayMode::Always);
        r.Prompt();
        r.Confirm("a");
        r.Confirm("b");
        const auto before = r.E.PredictionEpoch();
        r.Output(seq);
        INFO(std::string_view(seq));
        CHECK(r.E.PredictionEpoch() > before);
    }
}

TEST_CASE("output that changes nothing in the cursor row keeps the epoch") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    const auto before = r.E.PredictionEpoch();
    r.Output("\x1b[31m");  // a colour: no cell, no cursor
    r.Output("");
    CHECK(r.E.PredictionEpoch() == before);
}

TEST_CASE(
    "normal typing with a fast echo keeps the epoch and shows every key") {
    for (const auto mode : {EDisplayMode::Always, EDisplayMode::Adaptive}) {
        Rig r(40, 4, mode);
        r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
        r.Prompt();
        r.ConfirmEpoch();
        const auto epoch = r.E.PredictionEpoch();
        for (char c : std::string_view("bcdefgh")) {
            r.Type(std::string_view(&c, 1));
            REQUIRE(r.AnyVisible());
            r.Output(std::string_view(&c, 1));
            CHECK(r.E.PredictionEpoch() == epoch);
            r.Ack();
        }
        CHECK(r.Count() == 0);
        r.Type("i");
        CHECK(r.AnyVisible());
    }
}

TEST_CASE("an echo that arrives in pieces keeps the epoch") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    const auto epoch = r.E.PredictionEpoch();
    r.Type("b");
    r.Type("c");
    r.Type("d");
    r.Output("b");
    r.Output("cd");
    r.Ack();
    CHECK(r.E.PredictionEpoch() == epoch);
    CHECK(r.Count() == 0);
}

TEST_CASE("the echo of a backspace keeps the epoch") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    const auto epoch = r.E.PredictionEpoch();
    r.Type("\x7f");
    r.Output("\b \b");
    r.Ack();
    CHECK(r.E.PredictionEpoch() == epoch);
    r.Type("c");
    CHECK(r.AnyVisible());
}

TEST_CASE(
    "pwfeedback: a typed asterisk echoed as an asterisk credits no epoch") {
    for (const auto mode : {EDisplayMode::Always, EDisplayMode::Adaptive}) {
        Rig r(40, 4, mode);
        r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
        r.Prompt();
        r.ConfirmEpoch();
        r.Type("\r");
        r.Output("\r\n");
        r.Prompt("\rPassword: ");
        // the program echoes '*' for every key
        for (char c : std::string_view("pa*")) {
            r.Type(std::string_view(&c, 1));
            r.Output("*");
            r.Ack();
        }
        r.Type("s");
        CHECK_FALSE(r.AnyVisible());
    }
}

TEST_CASE("mask glyphs of all three kinds earn no credit") {
    for (const std::string_view glyph :
         {std::string_view("*"), BULLET, BLACK_CIRCLE}) {
        Rig r(40, 4, EDisplayMode::Always);
        r.Prompt();
        r.Type("a");  // epoch 1: invisible
        r.Output("\r\n");
        r.Prompt("\rPassword: ");
        const auto confirmed = r.E.ConfirmedEpoch();
        r.Type(glyph);
        r.Output(glyph);
        r.Ack();
        CHECK(r.E.ConfirmedEpoch() == confirmed);
        r.Type("s");
        CHECK_FALSE(r.AnyVisible());
    }
}

TEST_CASE("an ordinary glyph still earns credit") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    CHECK(r.E.ConfirmedEpoch() > 0);
}

TEST_CASE("wrong visible prediction is gone at the first covering ack") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");  // epoch 1 confirmed: visible
    REQUIRE(r.AnyVisible());
    r.Type("c");
    r.Output("b");  // mirror echoes b only
    r.E.OnEchoAck(EchoAck{0, r.Out, r.In - 1}, r.Now, r.View());  // covers b
    CHECK(r.Count() == 1);  // c (not covered) still there
    r.Ack();                // covers c, which never echoed
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("ack before output, silent prompt") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("x");
    REQUIRE(r.AnyVisible());
    r.E.OnEchoAck(EchoAck{0, r.Out + 3, r.In}, r.Now, r.View());
    CHECK(r.Count() == 1);
    r.Output("\r\n$");
    CHECK(r.Count() == 0);
}

TEST_CASE("snapshot mid typing hides later keys") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    REQUIRE(r.AnyVisible());
    r.G = MakeGrid(40, 4);
    r.Gen = 1;
    r.Out = 0;
    r.P.Feed(Bytes("\r$ ab").data(), 5, *r.G);
    r.E.OnSnapshot(1, r.Now, r.View());
    CHECK(r.Count() == 0);
    r.Type("c");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("resize mid typing hides later keys") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    (void)r.G->Resize(30, 4);
    r.E.OnResize(r.Now, r.View());
    r.Type("c");
    CHECK_FALSE(r.AnyVisible());
}
