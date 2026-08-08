#include "zet/core/version.hpp"

#include <doctest/doctest.h>

TEST_CASE("the build substitutes a version into the code") {
    // static_assert as well as CHECK: a tree that fails to number itself
    // should not link, never mind run.
    static_assert(zet::VERSION != 0);
    CHECK(zet::VERSION != 0);
}
