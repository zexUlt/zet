#include "zet/core/shell_quote.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace {

std::string Quote(std::string_view text) {
    std::string out(zet::ShellQuotedSize(text), '\0');
    const auto n = zet::ShellQuote(text, std::span{out});
    REQUIRE(n.has_value());
    CHECK(*n == out.size());
    return out;
}

}  // namespace

TEST_CASE("a plain path is wrapped in single quotes") {
    CHECK(Quote("/usr/local/bin/zet-agent") == "'/usr/local/bin/zet-agent'");
}

TEST_CASE("the empty string is an empty word, not no word") {
    CHECK(Quote("") == "''");
}

TEST_CASE("a quote closes the word, goes in escaped, and reopens it") {
    CHECK(Quote("it's") == "'it'\\''s'");
    CHECK(Quote("'") == "''\\'''");
    CHECK(Quote("''") == "''\\'''\\'''");
}

TEST_CASE("everything a shell would expand stays literal") {
    for (const std::string_view text :
         {"$HOME", "`id`", "$(id)", "a b", "a;b", "a\nb", "\\", "*", "!x",
          "~/bin", "\"q\"", "\t"}) {
        CAPTURE(text);
        const std::string quoted = Quote(text);
        CHECK(quoted == "'" + std::string{text} + "'");
    }
}

TEST_CASE("bytes outside ASCII pass through untouched") {
    CHECK(Quote("/home/пользователь/bin") == "'/home/пользователь/bin'");
    CHECK(Quote("\xff\x80") == "'\xff\x80'");
}

TEST_CASE("a NUL cannot be quoted, and a short buffer is refused") {
    std::string out(16, '\0');
    CHECK(
        zet::ShellQuote(std::string_view{"a\0b", 3}, std::span{out}).error() ==
        zet::EProtoError::MalformedField);
    std::string small(zet::ShellQuotedSize("it's") - 1, '\0');
    CHECK(zet::ShellQuote("it's", std::span{small}).error() ==
          zet::EProtoError::BufferTooSmall);
}
