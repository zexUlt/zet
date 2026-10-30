#include "zet/io/self_path.hpp"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("SelfPath returns an absolute path to the test binary") {
    const auto result = zet::io::SelfPath();
    REQUIRE(result.has_value());
    // .value() is used below so the static analyser can see it is guarded;
    // the REQUIRE above already ensures we do not reach these lines on failure.
    const std::string path = result.value();
    // An absolute path starts with a slash.
    REQUIRE(!path.empty());
    CHECK(path[0] == '/');
    // Running under ctest the binary is named zet_tests.
    CHECK(path.find("zet_tests") != std::string::npos);
}

TEST_CASE("SelfPath is stable across two calls") {
    const auto first = zet::io::SelfPath();
    const auto second = zet::io::SelfPath();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value() == second.value());
}
