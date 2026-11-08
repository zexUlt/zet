// Whole-screen change accounting and the two-distinct-echoes rule (design.md
// §9, M7): what a password prompt can do that the cursor row alone hides.
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

// --- Hash is blind to anything outside the cursor row / cursor position ----

TEST_CASE("LEAK: prompt drawn on another row, cursor saved/restored (DECSC)") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    r.Output(
        "\x1b"
        "7\x1b[3;1HPassword: \x1b"
        "8");
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("LEAK: prompt drawn on the row below, cursor back via CUU+CHA") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");  // cursor at col 4 (0-based), row 0
    r.Output("\x1b[B\rPassword: \x1b[A\x1b[5G");
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("LEAK(low): wide glyphs collapse to one hash value") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt("\r$ ");
    r.Output("\xE4\xBD\xA0");  // wide char, raises the epoch
    r.Confirm("a");
    r.Confirm("b");
    REQUIRE(r.E.ConfirmedEpoch() >= r.E.PredictionEpoch());
    // a different wide char over the same cells, same cursor position
    r.Output(
        "\r$ \xE5\xA5\xBD"
        "ab");
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

// --- Mask glyph rule completeness -------------------------------------------

namespace {

/// A hidden line whose program answers every key with `mask`; the user types
/// `keys`. Returns whether any prediction was ever visible.
bool MaskedLineLeaks(std::string_view mask,
                     const std::vector<std::string>& keys,
                     bool earlierEpochConfirmed) {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    if (earlierEpochConfirmed) r.Confirm("b");
    r.Type("\r");
    r.Output("\r\n");
    r.Output("\rPassword: ");
    bool leaked = false;
    for (const std::string& k : keys) {
        r.Type(k);
        leaked = leaked || r.AnyVisible();
        r.Output(mask);
        r.Ack();
    }
    r.Type("q");
    return leaked || r.AnyVisible();
}

}  // namespace

TEST_CASE("LEAK: password containing the app's mask glyph reveals the rest") {
    for (const std::string_view mask :
         {std::string_view("#"), std::string_view("."), std::string_view("x"),
          std::string_view("-"), std::string_view("\xC2\xB7"),
          std::string_view("*"), std::string_view("\xE2\x80\xA2")}) {
        INFO(std::string(mask));
        for (const bool confirmedBefore : {false, true}) {
            // "p" + mask glyph, then more keys
            CHECK_FALSE(MaskedLineLeaks(mask, {"p", std::string(mask), "r"},
                                        confirmedBefore));
            // starts with the mask glyph
            CHECK_FALSE(MaskedLineLeaks(mask, {std::string(mask), "p", "r"},
                                        confirmedBefore));
            // the same glyph for every key, however many
            CHECK_FALSE(MaskedLineLeaks(mask,
                                        {std::string(mask), std::string(mask),
                                         std::string(mask), std::string(mask)},
                                        confirmedBefore));
        }
    }
}

TEST_CASE("a wrong echo between two right ones starts the count again") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");  // one right echo
    r.Type("b");
    r.Output("Z");  // a wrong one
    r.Ack();
    REQUIRE(r.Count() == 0);
    r.Type("c");
    r.Output("c");  // a different glyph from "a", but not in a row with it
    r.Ack();
    CHECK(r.E.ConfirmedEpoch() == 0);
    r.Type("d");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("the same glyph twice in a row does not confirm") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("a");
    CHECK(r.E.ConfirmedEpoch() == 0);
    r.Confirm("b");
    CHECK(r.E.ConfirmedEpoch() == 1);
}

// --- Same-chunk / ordering ---------------------------------------------------

TEST_CASE(
    "typeahead echo and same-row prompt in one chunk (unconfirmed epoch)") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    r.Type("\r");  // epoch raised, raw-mode program: no newline echo
    r.Type("x");   // typeahead, epoch unconfirmed, outstanding
    r.Output("x[sudo] password: ");
    r.Ack();
    r.Type("h");
    CHECK_FALSE(r.AnyVisible());
    r.Type("unter2");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("ack pending before the chunk holding echo+prompt") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    r.Type("\r");
    r.Type("x");
    const std::string_view chunk = "x[sudo] password: ";
    r.E.OnEchoAck(EchoAck{0, r.Out + chunk.size(), r.In}, r.Now, r.View());
    r.Output(chunk);
    r.Type("h");
    CHECK_FALSE(r.AnyVisible());
    r.Type("unter2");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("echo, confirming ack, then prompt in same chunk as later echo") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    r.Type("\r");
    r.Type("x");
    r.Output("x");
    r.Ack();  // confirms the new epoch
    r.Type("y");
    r.E.OnEchoAck(EchoAck{0, r.Out + 18, r.In}, r.Now, r.View());
    r.Output("y[sudo] password: ");
    r.Type("h");
    CHECK_FALSE(r.AnyVisible());
}

// --- Fluency -----------------------------------------------------------------

TEST_CASE("fluency: zsh redraws the whole line per key") {
    Rig r(60, 4, EDisplayMode::Always);
    r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    std::string line = "ab";
    for (char c : std::string_view("cdefgh")) {
        r.Type(std::string_view(&c, 1));
        CHECK(r.AnyVisible());
        line += c;
        r.Output("\r$ " + line);
        r.Ack();
    }
}

TEST_CASE("fluency: syntax-highlight SGR around echo") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    for (char c : std::string_view("cdefgh")) {
        r.Type(std::string_view(&c, 1));
        CHECK(r.AnyVisible());
        r.Output(std::string("\x1b[32m") + c + "\x1b[39m");
        r.Ack();
    }
}

TEST_CASE("fluency: echo followed by erase-to-eol") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    for (char c : std::string_view("cdefgh")) {
        r.Type(std::string_view(&c, 1));
        CHECK(r.AnyVisible());
        r.Output(std::string(1, c) + "\x1b[K");
        r.Ack();
    }
}

// Text right of the cursor (zsh RPROMPT, autosuggestions): not predicted, by
// design (TailEmpty). Second-step scope, design.md M7; these two pin the
// current behaviour, they are not a promise.
TEST_CASE("zsh right prompt present: not predicted (second step)") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output("\x1b[30G[rp]\r\x1b[3C");  // RPROMPT at col 30, cursor back
    r.Confirm("a");
    r.Type("b");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("zsh-autosuggestions grey tail: not predicted (second step)") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Type("b");
    r.Output("b\x1b[90mcdef\x1b[39m\x1b[4D");
    r.Ack();
    r.Type("c");
    CHECK_FALSE(r.AnyVisible());
}

// --- Round 3: echoes that are not on the screen for the user ---------------

namespace {

/// A prompt whose program echoes the keys with `sgr` active, then two
/// distinct keys and a third one. True if the third is drawn.
bool EchoWithSgrShows(const char* sgr) {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output(sgr);
    r.Confirm("p");
    r.Confirm("q");
    r.Type("r");
    return r.AnyVisible();
}

}  // namespace

TEST_CASE("LEAK: a concealed (SGR 8) echo does not confirm an epoch") {
    CHECK_FALSE(EchoWithSgrShows("\x1b[8m"));
}

TEST_CASE("LEAK: an echo in its own palette colour does not confirm") {
    CHECK_FALSE(EchoWithSgrShows("\x1b[30;40m"));
    CHECK_FALSE(EchoWithSgrShows("\x1b[38;5;7;48;5;7m"));
}

TEST_CASE("LEAK: an echo in its own truecolor does not confirm") {
    CHECK_FALSE(EchoWithSgrShows("\x1b[38;2;9;8;7;48;2;9;8;7m"));
}

TEST_CASE("an ordinary coloured echo still confirms an epoch") {
    CHECK(EchoWithSgrShows("\x1b[31m"));
    CHECK(EchoWithSgrShows("\x1b[31;44m"));
    CHECK(EchoWithSgrShows("\x1b[38;2;9;8;7;48;2;9;8;8m"));
    CHECK(EchoWithSgrShows("\x1b[38;5;7;48;5;8m"));
}

// An echo before a silent prompt must not pair with a mask glyph after it.
TEST_CASE("LEAK: a streak glyph from before a silent prompt does not pair") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Output("\rPassword: ");
    r.Type("*");
    r.Output("*");  // the mask equals the first typed key
    r.Ack();
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("LEAK: the same in adaptive mode at high SRTT") {
    Rig r(60, 4, EDisplayMode::Adaptive);
    r.E.OnSrtt(std::chrono::milliseconds(300), r.Now);
    r.Prompt();
    r.Confirm("a");
    r.Output("\rPassword: ");
    r.Type("#");
    r.Output("#");
    r.Ack();
    r.Type("s");
    CHECK_FALSE(r.AnyVisible());
}

// The budget is exact: one changed cell that no prediction explains raises
// the epoch.
TEST_CASE("a single unexplained cell change raises the epoch") {
    Rig r(60, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    const auto e = r.E.PredictionEpoch();
    r.Output("\x1b[1;1H#\x1b[1;5H");
    CHECK(r.E.PredictionEpoch() > e);
}
