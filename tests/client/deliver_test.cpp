#include "deliver.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/shell_quote.hpp"

namespace {

using namespace zet;
using namespace zet::client;

TEST_SUITE("INSTALLER_SCRIPT") {
    TEST_CASE("no exclamation mark") {
        // csh expands ! in history substitution even inside single quotes.
        const std::string_view script = INSTALLER_SCRIPT;
        CHECK(script.find('!') == std::string_view::npos);
    }

    TEST_CASE("no literal newline") {
        const std::string_view script = INSTALLER_SCRIPT;
        CHECK(script.find('\n') == std::string_view::npos);
    }

    TEST_CASE("no NUL byte") {
        const std::string_view script = INSTALLER_SCRIPT;
        CHECK(script.find('\0') == std::string_view::npos);
    }

    TEST_CASE("survives ShellQuote round-trip") {
        const std::string_view script = INSTALLER_SCRIPT;
        std::string quoted(ShellQuotedSize(script), '\0');
        const auto n = ShellQuote(script, std::span{quoted});
        REQUIRE(n.has_value());
        CHECK(*n == quoted.size());
        // The quoted form must be non-empty (at least two surrounding
        // apostrophes).
        CHECK(quoted.size() >= 2);
        CHECK(quoted.front() == '\'');
        CHECK(quoted.back() == '\'');
    }

    TEST_CASE("contains ZET-PROBE marker") {
        CHECK(INSTALLER_SCRIPT.find("ZET-PROBE") != std::string_view::npos);
    }

    TEST_CASE("contains ZET-HAVE marker") {
        CHECK(INSTALLER_SCRIPT.find("ZET-HAVE") != std::string_view::npos);
    }

    TEST_CASE("contains ZET-SEND marker") {
        CHECK(INSTALLER_SCRIPT.find("ZET-SEND") != std::string_view::npos);
    }

    TEST_CASE("contains ZET-STORED marker") {
        CHECK(INSTALLER_SCRIPT.find("ZET-STORED") != std::string_view::npos);
    }

    TEST_CASE("contains ZET-FAILED marker") {
        CHECK(INSTALLER_SCRIPT.find("ZET-FAILED") != std::string_view::npos);
    }

    TEST_CASE("ends with --bootstrap") {
        // The last thing the installer does is exec zet-agent --bootstrap.
        CHECK(INSTALLER_SCRIPT.find("--bootstrap") != std::string_view::npos);
    }

}  // TEST_SUITE

}  // namespace
