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
        // Use ends_with to verify the exec is the last statement.
        const std::string_view script = INSTALLER_SCRIPT;
        const std::string_view suffix = "--bootstrap";
        CHECK(script.size() >= suffix.size());
        CHECK(script.substr(script.size() - suffix.size()) == suffix);
    }

    TEST_CASE(
        "ShellQuote round-trip: quoted string begins and ends with "
        "apostrophe") {
        // The full quoted form must be a valid single-quoted shell literal.
        const std::string_view script = INSTALLER_SCRIPT;
        const std::size_t qsize = ShellQuotedSize(script);
        REQUIRE(qsize >= 2);
        std::string quoted(qsize, '\0');
        const auto n = ShellQuote(script, std::span{quoted});
        REQUIRE(n.has_value());
        REQUIRE(*n == qsize);
        CHECK(quoted.front() == '\'');
        CHECK(quoted.back() == '\'');
        // The inner content must not contain a raw single-quote: ShellQuote
        // escapes them as '"'"', so the length must be >= script.size() + 2.
        CHECK(quoted.size() >= script.size() + 2);
    }

    TEST_CASE("contains ZET-FAILED write reason") {
        // head failure path emits a distinct reason.
        CHECK(INSTALLER_SCRIPT.find("ZET-FAILED write") !=
              std::string_view::npos);
    }

    TEST_CASE("contains ZET-FAILED size reason") {
        CHECK(INSTALLER_SCRIPT.find("ZET-FAILED size") !=
              std::string_view::npos);
    }

    TEST_CASE("contains ZET-FAILED rename reason") {
        CHECK(INSTALLER_SCRIPT.find("ZET-FAILED rename") !=
              std::string_view::npos);
    }

}  // TEST_SUITE

}  // namespace
