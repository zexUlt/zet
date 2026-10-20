#include "zet/wire/probe.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace zet;
using namespace zet::wire;

ByteSpan AsBytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// Feed all of `input` to a fresh scanner and return the first Answer (if any).
std::optional<ProbeAnswer> FeedAll(std::string_view input) {
    ProbeScanner scanner;
    const auto scan = scanner.Feed(AsBytes(input));
    return scan.Answer;
}

TEST_SUITE("ProbeScanner") {
    TEST_CASE("ZET-PROBE x86_64 exec parsed") {
        // ProbeInfo::Arch is a string_view into the scanner's line buffer;
        // the scanner must outlive the answer.
        ProbeScanner scanner;
        const auto scan = scanner.Feed(AsBytes("ZET-PROBE 1 x86_64 exec\n"));
        REQUIRE(scan.Answer.has_value());
        const auto* info = std::get_if<ProbeInfo>(&*scan.Answer);
        REQUIRE(info != nullptr);
        CHECK(info->Arch == "x86_64");
        CHECK(!info->NoExec);
    }

    TEST_CASE("ZET-PROBE aarch64 noexec parsed") {
        ProbeScanner scanner;
        const auto scan = scanner.Feed(AsBytes("ZET-PROBE 1 aarch64 noexec\n"));
        REQUIRE(scan.Answer.has_value());
        const auto* info = std::get_if<ProbeInfo>(&*scan.Answer);
        REQUIRE(info != nullptr);
        CHECK(info->Arch == "aarch64");
        CHECK(info->NoExec);
    }

    TEST_CASE("ZET-HAVE parsed") {
        CHECK(std::holds_alternative<ProbeHave>(*FeedAll("ZET-HAVE\n")));
    }

    TEST_CASE("ZET-SEND parsed") {
        CHECK(std::holds_alternative<ProbeSend>(*FeedAll("ZET-SEND\n")));
    }

    TEST_CASE("ZET-STORED parsed") {
        CHECK(std::holds_alternative<ProbeStored>(*FeedAll("ZET-STORED\n")));
    }

    TEST_CASE("ZET-FAILED with reason") {
        ProbeScanner scanner;
        const auto scan = scanner.Feed(AsBytes("ZET-FAILED size\n"));
        REQUIRE(scan.Answer.has_value());
        const auto* f = std::get_if<ProbeFailed>(&*scan.Answer);
        REQUIRE(f != nullptr);
        CHECK(f->Reason == "size");
    }

    TEST_CASE("ZET-FAILED rename reason") {
        ProbeScanner scanner;
        const auto scan = scanner.Feed(AsBytes("ZET-FAILED rename\n"));
        REQUIRE(scan.Answer.has_value());
        const auto* f = std::get_if<ProbeFailed>(&*scan.Answer);
        REQUIRE(f != nullptr);
        CHECK(f->Reason == "rename");
    }

    TEST_CASE("CRLF stripped") {
        CHECK(std::holds_alternative<ProbeHave>(*FeedAll("ZET-HAVE\r\n")));
    }

    TEST_CASE("noise before probe is skipped") {
        const auto answer = FeedAll(
            "Welcome to my server\nLoading profile\nZET-PROBE 1 x86_64 exec\n");
        REQUIRE(answer.has_value());
        CHECK(std::get_if<ProbeInfo>(&*answer) != nullptr);
    }

    TEST_CASE("empty lines and whitespace-only lines are noise") {
        const auto answer = FeedAll("\n  \nZET-HAVE\n");
        REQUIRE(answer.has_value());
        CHECK(std::holds_alternative<ProbeHave>(*answer));
    }

    TEST_CASE("unknown ZET- line increments SkippedMarkers") {
        ProbeScanner scanner;
        static_cast<void>(scanner.Feed(AsBytes("ZET-UNKNOWN\n")));
        CHECK(scanner.SkippedMarkers() == 1);
    }

    TEST_CASE("line-too-long ZET- line increments SkippedMarkers") {
        ProbeScanner scanner;
        // Build a ZET- line longer than MAX_PROBE_LINE.
        std::string line = "ZET-";
        line.append(MAX_PROBE_LINE, 'X');
        line += '\n';
        static_cast<void>(scanner.Feed(AsBytes(line)));
        CHECK(scanner.SkippedMarkers() == 1);
    }

    TEST_CASE("line too long is not parsed as answer") {
        std::string line = "ZET-PROBE 1 x86_64 exec";
        line.append(MAX_PROBE_LINE, ' ');
        line += '\n';
        CHECK(!FeedAll(line).has_value());
    }

    TEST_CASE("Consumed stops at newline of answer") {
        ProbeScanner scanner;
        const std::string_view input = "ZET-HAVE\nZET-SEND\n";
        const auto scan = scanner.Feed(AsBytes(input));
        // Consumed should be exactly the length of "ZET-HAVE\n"
        CHECK(scan.Consumed == std::string_view{"ZET-HAVE\n"}.size());
        REQUIRE(scan.Answer.has_value());
        CHECK(std::holds_alternative<ProbeHave>(*scan.Answer));
    }

    TEST_CASE("remainder after answer fed to scanner for next answer") {
        ProbeScanner scanner;
        const std::string_view input = "ZET-HAVE\nZET-SEND\n";
        const auto scan1 = scanner.Feed(AsBytes(input));
        REQUIRE(scan1.Answer.has_value());
        CHECK(std::holds_alternative<ProbeHave>(*scan1.Answer));

        // Feed the remainder.
        const auto remainder = input.substr(scan1.Consumed);
        const auto scan2 = scanner.Feed(AsBytes(remainder));
        REQUIRE(scan2.Answer.has_value());
        CHECK(std::holds_alternative<ProbeSend>(*scan2.Answer));
    }

    TEST_CASE("byte-by-byte: ZET-PROBE parsed") {
        // ProbeInfo::Arch is a string_view into the scanner's line buffer;
        // FeedByteByByte would return a dangling view, so use a local scanner.
        ProbeScanner scanner;
        const std::string_view input = "ZET-PROBE 1 aarch64 exec\n";
        std::optional<ProbeScan> last_scan;
        for (std::size_t i = 0; i < input.size(); ++i) {
            auto scan = scanner.Feed(AsBytes(input.substr(i, 1)));
            if (scan.Answer) {
                last_scan = scan;
                break;
            }
        }
        REQUIRE(last_scan.has_value());
        REQUIRE(last_scan->Answer.has_value());
        const auto* info = std::get_if<ProbeInfo>(&*last_scan->Answer);
        REQUIRE(info != nullptr);
        CHECK(info->Arch == "aarch64");
    }

    TEST_CASE("byte-by-byte: ZET-FAILED parsed") {
        ProbeScanner scanner;
        const std::string_view input = "ZET-FAILED size\n";
        std::optional<ProbeScan> last_scan;
        for (std::size_t i = 0; i < input.size(); ++i) {
            auto scan = scanner.Feed(AsBytes(input.substr(i, 1)));
            if (scan.Answer) {
                last_scan = scan;
                break;
            }
        }
        REQUIRE(last_scan.has_value());
        REQUIRE(last_scan->Answer.has_value());
        const auto* f = std::get_if<ProbeFailed>(&*last_scan->Answer);
        REQUIRE(f != nullptr);
        CHECK(f->Reason == "size");
    }

    TEST_CASE("ZET-PROBE wrong version returns no answer") {
        CHECK(!FeedAll("ZET-PROBE 2 x86_64 exec\n").has_value());
    }

    TEST_CASE("ZET-PROBE missing arch returns no answer") {
        CHECK(!FeedAll("ZET-PROBE 1\n").has_value());
    }

    TEST_CASE("ZET-PROBE bad exec field returns no answer") {
        CHECK(!FeedAll("ZET-PROBE 1 x86_64 maybe\n").has_value());
    }

    TEST_CASE("non-ZET noise does not increment SkippedMarkers") {
        ProbeScanner scanner;
        static_cast<void>(scanner.Feed(AsBytes("hello world\nsome banner\n")));
        CHECK(scanner.SkippedMarkers() == 0);
    }

    TEST_CASE("partial line across two feeds") {
        ProbeScanner scanner;
        const auto scan1 = scanner.Feed(AsBytes("ZET-HA"));
        CHECK(!scan1.Answer.has_value());
        const auto scan2 = scanner.Feed(AsBytes("VE\n"));
        REQUIRE(scan2.Answer.has_value());
        CHECK(std::holds_alternative<ProbeHave>(*scan2.Answer));
    }

    TEST_CASE("empty chunk returns no answer, Consumed = 0") {
        ProbeScanner scanner;
        const auto scan = scanner.Feed(AsBytes(""));
        CHECK(scan.Consumed == 0);
        CHECK(!scan.Answer.has_value());
    }

}  // TEST_SUITE

}  // namespace
