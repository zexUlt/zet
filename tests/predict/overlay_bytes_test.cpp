// The overlay renderer's exact bytes (design.md §9, ADR-037, ADR-039): what
// goes to the terminal to show a prediction, to take it off again, and to
// change it; and where it refuses to draw.
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <string_view>

#include "predict_rig.hpp"
#include "zet/predict/overlay.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

std::string Str(zet::ByteSpan b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

struct OverlayRig : Rig {
    std::unique_ptr<Overlay> O = Overlay::Create();

    OverlayRig(int cols, int rows, EDisplayMode mode) : Rig(cols, rows, mode) {}

    /// The bytes of an update, committed as the client does once queued.
    std::string Update() {
        const std::string s = Str(O->BuildUpdate(E, View()));
        O->Commit(E);
        return s;
    }
    std::string Undo() {
        const std::string s = Str(O->BuildUndo(View()));
        O->Commit(E);
        return s;
    }

    /// A prompt, two keys echoed to confirm an epoch; the cursor is at
    /// column 4 (0-based) of an otherwise empty row.
    void Start() {
        Prompt();
        ConfirmEpoch();
    }
};

constexpr std::string_view BEGIN = "\x1b[?2026h\x1b[?25l";
constexpr std::string_view END = "\x1b[?25h\x1b[?2026l";

}  // namespace

TEST_CASE(
    "a visible prediction is drawn with CHA and the cursor put after it") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    CHECK(r.Update() ==
          std::string(BEGIN) + "\x1b[5Gb\x1b[6G" + std::string(END));
}

TEST_CASE("the overlay says what was drawn to the engine") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    REQUIRE(r.Count() == 1);
    CHECK_FALSE(r.E.Predictions()[0].Drawn);
    static_cast<void>(r.O->BuildUpdate(r.E, r.View()));
    CHECK_FALSE(r.E.Predictions()[0].Drawn);  // built, not queued
    CHECK_FALSE(r.O->Drawn());
    r.O->Commit(r.E);
    CHECK(r.E.Predictions()[0].Drawn);
    CHECK(r.O->Drawn());
}

TEST_CASE("what was not queued is not taken off") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    static_cast<void>(r.O->BuildUpdate(r.E, r.View()));
    r.O->Forget();  // the client could not queue it
    CHECK(r.Undo().empty());
}

TEST_CASE("taking the overlay off writes the mirror's cell back") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    static_cast<void>(r.Update());
    // Nothing came back yet: the cell is empty in the mirror, so ECH.
    CHECK(r.Undo() ==
          std::string(BEGIN) + "\x1b[5G\x1b[X\x1b[5G" + std::string(END));
    CHECK_FALSE(r.O->Drawn());
    CHECK_FALSE(r.E.Predictions()[0].Drawn);
}

TEST_CASE("taking the overlay off after the echo writes the echoed glyph") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    static_cast<void>(r.Update());
    // The caller undoes first, then lets the bytes through, then feeds the
    // mirror: the restore is built from the mirror as it was.
    const std::string undo = r.Undo();
    CHECK(undo.find("\x1b[X") != std::string::npos);
    r.Output("b");
    // Now drawn nothing, and the glyph is in the mirror: nothing to draw.
    CHECK(r.Update().empty());
}

TEST_CASE(
    "the glyph under an overlay is restored from the mirror, not blanked") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    // Backspace over the last echoed glyph: the predicted cell is empty.
    r.Type("\x7f");
    REQUIRE(r.Count() >= 1);
    const std::string draw = r.Update();
    CHECK(draw.find("\x1b[4G\x1b[X") != std::string::npos);
    // The mirror still holds the glyph there; the restore is the glyph.
    CHECK(r.Undo() ==
          std::string(BEGIN) + "\x1b[4GA\x1b[5G" + std::string(END));
}

TEST_CASE("an unchanged plan draws nothing") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    CHECK_FALSE(r.Update().empty());
    CHECK(r.Update().empty());
}

TEST_CASE("a new prediction replaces the drawn ones in one block") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    static_cast<void>(r.Update());
    r.Type("c");
    const std::string s = r.Update();
    // One synchronised block, both cells drawn, cursor after the last.
    CHECK(s.find("?2026h") == s.rfind("?2026h"));
    CHECK(s.find("b") != std::string::npos);
    CHECK(s.find("c") != std::string::npos);
    CHECK(s.find("\x1b[7G") != std::string::npos);
}

TEST_CASE("positioning is CHA, and DECSC, DECRC and CUP are never used") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    std::string all;
    for (const char* key : {"b", "c", "\x7f", "d"}) {
        r.Type(key);
        all += r.Update();
        all += r.Undo();
    }
    CHECK_FALSE(all.empty());
    CHECK(all.find("\x1b"
                   "7") == std::string::npos);
    CHECK(all.find("\x1b"
                   "8") == std::string::npos);
    CHECK(all.find("\x1b[s") == std::string::npos);
    CHECK(all.find("\x1b[u") == std::string::npos);
    for (std::size_t i = 0; i + 1 < all.size(); ++i) {
        if (all[i] != '\x1b' || all[i + 1] != '[') continue;
        std::size_t j = i + 2;
        while (j < all.size() && (all[j] == '?' || all[j] == ';' ||
                                  (all[j] >= '0' && all[j] <= '9'))) {
            ++j;
        }
        REQUIRE(j < all.size());
        CHECK(all[j] != 'H');
        CHECK(all[j] != 'f');
        CHECK(all[j] != 'd');
    }
}

TEST_CASE(
    "predictions over the threshold are underlined, and the pen comes back") {
    OverlayRig r(40, 4, EDisplayMode::Adaptive);
    r.E.OnSrtt(std::chrono::milliseconds(400), r.Now);
    r.Start();
    r.Type("b");
    REQUIRE(r.E.UnderlinePredictions());
    CHECK(r.Update() == std::string(BEGIN) + "\x1b[5G\x1b[0;4mb\x1b[0m\x1b[6G" +
                            std::string(END));
}

TEST_CASE("a prediction is not drawn when SRTT is low") {
    OverlayRig r(40, 4, EDisplayMode::Adaptive);
    r.E.OnSrtt(std::chrono::milliseconds(20), r.Now);
    r.Start();
    r.Type("b");
    CHECK(r.Update().empty());
}

TEST_CASE("the overlay is drawn only when the parser is in Ground") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    SUBCASE("in the middle of a sequence") {
        r.Output("\x1b[3");
        CHECK(r.Update().empty());
        r.Output("1m");
        CHECK_FALSE(r.Update().empty());
    }
    SUBCASE("in the middle of a UTF-8 character") {
        r.Output("\xe2\x82");
        CHECK(r.Update().empty());
    }
    SUBCASE("in a string") {
        r.Output("\x1b]0;ti");
        CHECK(r.Update().empty());
    }
    SUBCASE("at a boundary") { CHECK_FALSE(r.Update().empty()); }
}

TEST_CASE("the overlay is not drawn on the alternate screen") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    r.Output("\x1b[?1049h\x1b[H");
    CHECK(r.Update().empty());
}

TEST_CASE("the overlay is not drawn with a deferred wrap or insert mode") {
    SUBCASE("wrap") {
        OverlayRig r(8, 4, EDisplayMode::Always);
        r.Start();
        r.Type("b");
        r.Output("1234");  // the last column is written: wrap pending
        CHECK(r.Update().empty());
    }
    SUBCASE("insert mode") {
        OverlayRig r(40, 4, EDisplayMode::Always);
        r.Start();
        r.Type("b");
        r.Output("\x1b[4h");
        CHECK(r.Update().empty());
    }
}

TEST_CASE("never draws nothing") {
    OverlayRig r(40, 4, EDisplayMode::Never);
    r.Start();
    r.Type("b");
    CHECK(r.Update().empty());
}

TEST_CASE(
    "the cursor stays hidden if the mirror hides it, and a program's "
    "synchronised update is not closed") {
    OverlayRig r(40, 4, EDisplayMode::Always);
    r.Start();
    r.Type("b");
    SUBCASE("hidden cursor") {
        r.Output("\x1b[?25l");
        const std::string s = r.Update();
        CHECK_FALSE(s.empty());
        CHECK(s.find("\x1b[?25h") == std::string::npos);
    }
    SUBCASE("program's own sync") {
        r.Output("\x1b[?2026h");
        const std::string s = r.Update();
        CHECK_FALSE(s.empty());
        CHECK(s.find("2026") == std::string::npos);
    }
}
