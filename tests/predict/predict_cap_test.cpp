// The prediction list is bounded (design.md §9, "Пределы"; §16
// predict_cap_test).
#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("one key past the cap is not predicted and raises the epoch") {
    Rig r(static_cast<int>(PREDICTIONS_MAX) + 40, 3, EDisplayMode::Always);
    r.Prompt();
    for (std::size_t i = 0; i < PREDICTIONS_MAX; ++i) r.Type("a");
    REQUIRE(r.Count() == PREDICTIONS_MAX);
    const auto epoch = r.E.PredictionEpoch();

    r.Type("a");
    CHECK(r.Count() == PREDICTIONS_MAX);
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("the list never grows past the cap however many keys are typed") {
    Rig r(static_cast<int>(PREDICTIONS_MAX) * 3, 3, EDisplayMode::Always);
    r.Prompt();
    for (std::size_t i = 0; i < PREDICTIONS_MAX * 2; ++i) {
        r.Type("a");
        CHECK(r.Count() <= PREDICTIONS_MAX);
    }
}

TEST_CASE("acks free room and prediction resumes") {
    Rig r(static_cast<int>(PREDICTIONS_MAX) + 40, 3, EDisplayMode::Always);
    r.Prompt();
    std::string typed;
    for (std::size_t i = 0; i < PREDICTIONS_MAX + 1; ++i) typed += 'a';
    for (char c : typed) r.Type(std::string_view(&c, 1));
    REQUIRE(r.Count() == PREDICTIONS_MAX);

    r.Output(std::string(PREDICTIONS_MAX, 'a'));
    r.Ack();
    CHECK(r.Count() == 0);
    r.Type("b");
    CHECK(r.Count() == 1);
}

TEST_CASE("a backspace over a predicted cell needs no room") {
    Rig r(static_cast<int>(PREDICTIONS_MAX) + 40, 3, EDisplayMode::Always);
    r.Prompt();
    for (std::size_t i = 0; i < PREDICTIONS_MAX; ++i) r.Type("a");
    REQUIRE(r.Count() == PREDICTIONS_MAX);
    r.Type("\x7f");
    CHECK(r.Count() == PREDICTIONS_MAX);
    REQUIRE(r.E.Cursor().has_value());
    CHECK(static_cast<std::size_t>(r.E.Cursor()->Col) ==
          2 + PREDICTIONS_MAX - 1);
}
