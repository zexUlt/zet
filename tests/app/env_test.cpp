#include "zet/app/env.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using zet::app::ChoiceFromEnv;
using zet::app::SizeFromEnv;

constexpr const char* NAME = "ZET_ENV_TEST_VALUE";
constexpr std::array<std::string_view, 2> PATHS{"A", "B"};

/// Sets NAME for one test and unsets it again, so the cases stay independent.
class ScopedEnv {
public:
    explicit ScopedEnv(const char* value) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): the test is single-threaded.
        ::setenv(NAME, value, 1);
    }
    ScopedEnv() { ::unsetenv(NAME); }
    ~ScopedEnv() { ::unsetenv(NAME); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;
};

}  // namespace

TEST_CASE("ChoiceFromEnv takes exactly an allowed value") {
    {
        const ScopedEnv env{"A"};
        CHECK(ChoiceFromEnv(NAME, PATHS) == "A");
    }
    {
        const ScopedEnv env{"B"};
        CHECK(ChoiceFromEnv(NAME, PATHS) == "B");
    }
}

TEST_CASE("ChoiceFromEnv ignores unset, near misses and nonsense") {
    {
        const ScopedEnv env;
        CHECK_FALSE(ChoiceFromEnv(NAME, PATHS).has_value());
    }
    for (const char* bad : {"a", "b", "AB", "A ", " B", "", "C", "1", "none"}) {
        CAPTURE(bad);
        const ScopedEnv env{bad};
        CHECK_FALSE(ChoiceFromEnv(NAME, PATHS).has_value());
    }
}

TEST_CASE("SizeFromEnv takes a positive whole number") {
    {
        const ScopedEnv env;
        CHECK_FALSE(SizeFromEnv(NAME).has_value());
    }
    {
        const ScopedEnv env{"65536"};
        CHECK(SizeFromEnv(NAME) == 65536);
    }
}

TEST_CASE("SizeFromEnv ignores zero, signs, suffixes and overflow") {
    for (const char* bad : {"0", "", "-1", "+5", "64k", "1.5", " 7", "7 ",
                            "99999999999999999999999"}) {
        CAPTURE(bad);
        const ScopedEnv env{bad};
        CHECK_FALSE(SizeFromEnv(NAME).has_value());
    }
}
