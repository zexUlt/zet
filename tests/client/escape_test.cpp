#include "escape.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zet;
using zet::client::EscapeScanner;

struct Scanned {
    std::string Sent;
    bool Quit{false};
};

/// Feeds `chunks` one after another, as reads off the terminal.
Scanned Scan(EscapeScanner& scanner,
             std::initializer_list<std::string_view> chunks) {
    Scanned scanned;
    for (const std::string_view chunk : chunks) {
        std::vector<std::byte> out(2 * chunk.size() + 1);
        const auto result = scanner.Feed(
            ByteSpan{reinterpret_cast<const std::byte*>(chunk.data()),
                     chunk.size()},
            MutableByteSpan{out});
        scanned.Sent.append(reinterpret_cast<const char*>(out.data()),
                            result.Written);
        if (result.Quit) {
            scanned.Quit = true;
            break;
        }
    }
    return scanned;
}

Scanned Scan(std::initializer_list<std::string_view> chunks) {
    EscapeScanner scanner;
    return Scan(scanner, chunks);
}

}  // namespace

TEST_CASE("~. at the start of the session quits") {
    const auto scanned = Scan({"~."});
    CHECK(scanned.Quit);
    CHECK(scanned.Sent.empty());
}

TEST_CASE("~. after a carriage return or a newline quits") {
    for (const std::string_view line : {"ls\r~.", "ls\n~."}) {
        CAPTURE(line);
        const auto scanned = Scan({line});
        CHECK(scanned.Quit);
        CHECK(scanned.Sent == line.substr(0, 3));
    }
}

TEST_CASE("bytes after ~. in the same chunk are not sent") {
    const auto scanned = Scan({"\r~.rm -rf\r"});
    CHECK(scanned.Quit);
    CHECK(scanned.Sent == "\r");
}

TEST_CASE("~ in the middle of a line is just a ~") {
    const auto scanned = Scan({"cd ~.", "/x\r"});
    CHECK_FALSE(scanned.Quit);
    CHECK(scanned.Sent == "cd ~./x\r");
}

TEST_CASE("~~ sends one ~, and the next ~ is mid-line") {
    const auto scanned = Scan({"~~~."});
    CHECK_FALSE(scanned.Quit);
    CHECK(scanned.Sent == "~~.");
}

TEST_CASE("~ and any other byte sends both") {
    const auto scanned = Scan({"~x"});
    CHECK_FALSE(scanned.Quit);
    CHECK(scanned.Sent == "~x");
}

TEST_CASE("a ~ followed by a newline starts a new line") {
    const auto scanned = Scan({"~\r~."});
    CHECK(scanned.Quit);
    CHECK(scanned.Sent == "~\r");
}

TEST_CASE("the escape survives being split across reads") {
    EscapeScanner scanner;
    CHECK(Scan(scanner, {"echo\r"}).Sent == "echo\r");
    const auto held = Scan(scanner, {"~"});
    CHECK(held.Sent.empty());
    CHECK_FALSE(held.Quit);
    CHECK(Scan(scanner, {"."}).Quit);
}

TEST_CASE("a held ~ goes out with the byte that resolves it") {
    EscapeScanner scanner;
    CHECK(Scan(scanner, {"~"}).Sent.empty());
    CHECK(Scan(scanner, {"q"}).Sent == "~q");
}
