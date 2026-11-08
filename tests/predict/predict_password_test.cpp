// With echo off nothing is ever visible, and no code says "password"
// (design.md §9, "Пароли").
#include <doctest/doctest.h>

#include <string_view>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

// Types each key of `s` on its own, lets the program echo nothing, and acks.
void TypeSilently(Rig& r, std::string_view s) {
    for (char c : s) {
        r.Type(std::string_view(&c, 1));
        CHECK_FALSE(r.AnyVisible());
    }
}

}  // namespace

TEST_CASE("typing at a silent prompt is never visible") {
    for (const auto mode : {EDisplayMode::Always, EDisplayMode::Adaptive}) {
        Rig r(40, 4, mode);
        r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
        r.Prompt();

        // `echo ok` confirms the first epoch.
        r.Type("echo ok");
        r.Output("echo ok");
        r.Ack();
        REQUIRE(r.E.ConfirmedEpoch() >= 1);
        r.Type("\r");
        r.Output("\r\nok\r\n");
        r.Prompt("\rPassword: ");

        // The program turned echo off: keys go in, nothing comes back.
        TypeSilently(r, "hunter2");
        CHECK_FALSE(r.AnyVisible());
        r.Ack();  // the check finds the cells empty
        CHECK(r.Count() == 0);
        CHECK_FALSE(r.AnyVisible());

        // Typing on: the next keys land in a later epoch and die the same way.
        const auto confirmed = r.E.ConfirmedEpoch();
        TypeSilently(r, "more");
        r.Ack();
        CHECK(r.E.ConfirmedEpoch() == confirmed);
        CHECK(r.Count() == 0);

        // Enter, and the next prompt echoes again: the first key after it is
        // invisible, the second too, the third shows (design.md M7).
        r.Type("\r");
        r.Output("\r\n$ ");
        r.Type("l");
        CHECK_FALSE(r.AnyVisible());
        r.Output("l");
        r.Ack();
        r.Type("s");
        CHECK_FALSE(r.AnyVisible());
        r.Output("s");
        r.Ack();
        r.Type(" ");
        CHECK(r.AnyVisible());
    }
}

TEST_CASE("keys typed ahead of a silent prompt do not show either") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("\r");
    // Typed before the program's reply arrived, in a fresh epoch.
    r.Type("pw");
    CHECK_FALSE(r.AnyVisible());
    r.Output("\r\nPassword: ");
    CHECK_FALSE(r.AnyVisible());
    r.Ack();
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("a silent prompt costs no glitch") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("\r");
    r.Output("\r\nPassword: ");
    r.Prompt("\rPassword: ");
    r.Type("secret");
    r.Ack();
    CHECK(r.E.GlitchTrigger() == 0);
    CHECK_FALSE(r.E.ShowPredictions());
}
