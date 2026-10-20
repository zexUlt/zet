#include "zet/core/version.hpp"

#include <doctest/doctest.h>

TEST_CASE("the build substitutes a version into the code") {
    // static_assert as well as CHECK: a tree that fails to number itself
    // should not link, never mind run.
    static_assert(zet::VERSION != 0);
    CHECK(zet::VERSION != 0);
}

TEST_CASE("RELEASE_VERSION is non-empty and has a known format") {
    // Accepted formats:
    //   X.Y.Z                  — exact tag build
    //   X.Y.Z-N-gHASH-dev      — post-tag build
    //   0.0.0-dev              — fallback (no .git or no tag)
    const auto v = std::string{zet::RELEASE_VERSION};
    CHECK_FALSE(v.empty());

    // Either a plain X.Y.Z ...
    const bool exact = !v.empty() && [&] {
        std::size_t i = 0;
        auto digits = [&] {
            if (i >= v.size() || v[i] < '0' || v[i] > '9') {
                return false;
            }
            while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
                ++i;
            }
            return true;
        };
        return digits() && i < v.size() && v[i++] == '.' && digits() &&
               i < v.size() && v[i++] == '.' && digits() && i == v.size();
    }();

    // ... or X.Y.Z-N-gHASH-dev ...
    const bool postTag = !v.empty() && [&] {
        const auto dash1 = v.rfind("-dev");
        if (dash1 == std::string::npos || dash1 + 4 != v.size()) {
            return false;
        }
        // Remove -dev suffix and check X.Y.Z-N-gHEX
        const auto core = v.substr(0, dash1);
        // Must contain at least two dashes: X.Y.Z-N-gHASH
        const auto d2 = core.rfind('-');
        if (d2 == std::string::npos) {
            return false;
        }
        const auto hashPart = core.substr(d2 + 1);
        if (hashPart.size() < 2 || hashPart[0] != 'g') {
            return false;
        }
        for (char c : hashPart.substr(1)) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }();

    // ... or the 0.0.0-dev fallback.
    const bool fallback = (v == "0.0.0-dev");

    CHECK((exact || postTag || fallback));
}
