#include "zet/io/self_path.hpp"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("SelfPath returns an absolute path ending in the test binary name") {
    const auto result = zet::io::SelfPath();
    REQUIRE(result.has_value());
    const std::string& path = *result;
    // An absolute path starts with a slash.
    REQUIRE(!path.empty());
    CHECK(path[0] == '/');
    // The path must refer to a real file: it must contain at least one slash.
    CHECK(path.find('/') != std::string::npos);
    // Running under ctest the binary is named zet_tests.
    CHECK(path.find("zet_tests") != std::string::npos);
}

TEST_CASE("SelfPath is stable across two calls") {
    const auto first = zet::io::SelfPath();
    const auto second = zet::io::SelfPath();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}
