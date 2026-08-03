#include "zet/core/log.hpp"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

namespace {

using zet::Timestamp;
using zet::log::ELevel;
using zet::log::Field;
using zet::log::Record;

struct Captured {
    ELevel Level;
    std::string Message;
    std::vector<std::string> Keys;
    Timestamp At;
};

struct Capture {
    std::vector<Captured> Records;
};

void CaptureSink(const Record& record, void* context) noexcept {
    auto* capture = static_cast<Capture*>(context);
    Captured c{.Level = record.Level,
               .Message = std::string{record.Message},
               .Keys = {},
               .At = record.At};
    for (const auto& field : record.Fields) {
        c.Keys.emplace_back(field.Key());
    }
    capture->Records.push_back(std::move(c));
}

/// Installs a sink for one test and takes it away afterwards, so a leftover
/// sink cannot make a later test pass for the wrong reason.
class SinkGuard {
public:
    explicit SinkGuard(Capture& capture) {
        zet::log::SetSink(&CaptureSink, &capture);
        zet::log::SetMinLevel(ELevel::Trace);
    }
    SinkGuard(const SinkGuard&) = delete;
    SinkGuard& operator=(const SinkGuard&) = delete;
    ~SinkGuard() {
        zet::log::SetSink(nullptr, nullptr);
        zet::log::SetMinLevel(ELevel::Info);
    }
};

}  // namespace

TEST_CASE("there is no fatal level") {
    // Not a runtime check but a statement of intent that fails to compile if
    // someone adds one: a logging level that ends the process is how
    // EternalTerminal ended up with 164 ways to kill the daemon.
    static_assert(static_cast<int>(ELevel::Error) == 4,
                  "Error must remain the highest level");
}

TEST_CASE("records reach the installed sink") {
    Capture capture;
    const SinkGuard guard{capture};

    const std::array fields{Field::Text("sid", "abc"),
                            Field::Unsigned("bytes", 42)};
    zet::log::Emit(ELevel::Warn, "reconnecting", fields,
                   Timestamp::FromNanos(7));

    REQUIRE(capture.Records.size() == 1);
    CHECK(capture.Records[0].Level == ELevel::Warn);
    CHECK(capture.Records[0].Message == "reconnecting");
    CHECK(capture.Records[0].Keys == std::vector<std::string>{"sid", "bytes"});
    CHECK(capture.Records[0].At == Timestamp::FromNanos(7));
}

TEST_CASE("nothing is emitted until a sink is installed") {
    // A library that writes to stderr because nobody told it otherwise will
    // one day corrupt somebody's stdout.
    zet::log::SetSink(nullptr, nullptr);
    zet::log::SetMinLevel(ELevel::Trace);

    CHECK_FALSE(zet::log::IsEnabled(ELevel::Error));
    zet::log::Emit(ELevel::Error, "goes nowhere");  // must not crash

    zet::log::SetMinLevel(ELevel::Info);
}

TEST_CASE("records below the minimum level are dropped") {
    Capture capture;
    const SinkGuard guard{capture};
    zet::log::SetMinLevel(ELevel::Warn);

    zet::log::Emit(ELevel::Debug, "chatter");
    zet::log::Emit(ELevel::Info, "routine");
    zet::log::Emit(ELevel::Warn, "notable");
    zet::log::Emit(ELevel::Error, "bad");

    REQUIRE(capture.Records.size() == 2);
    CHECK(capture.Records[0].Message == "notable");
    CHECK(capture.Records[1].Message == "bad");
}

TEST_CASE("IsEnabled agrees with what Emit actually does") {
    Capture capture;
    const SinkGuard guard{capture};
    zet::log::SetMinLevel(ELevel::Info);

    CHECK_FALSE(zet::log::IsEnabled(ELevel::Trace));
    CHECK(zet::log::IsEnabled(ELevel::Info));
    CHECK(zet::log::MinLevel() == ELevel::Info);

    zet::log::Emit(ELevel::Trace, "dropped");
    CHECK(capture.Records.empty());
}

TEST_CASE("fields carry their type through to the sink") {
    Capture capture;
    const SinkGuard guard{capture};

    const auto text = Field::Text("path", "/run/zet");
    const auto count = Field::Unsigned("count", 1u << 20);
    const auto offset = Field::Signed("skew", -5);

    CHECK(text.Kind() == Field::EKind::Text);
    CHECK(text.AsText() == "/run/zet");
    CHECK(count.Kind() == Field::EKind::Unsigned);
    CHECK(count.AsUnsigned() == 1u << 20);
    CHECK(offset.Kind() == Field::EKind::Signed);
    CHECK(offset.AsSigned() == -5);
}
