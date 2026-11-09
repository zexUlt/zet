// A second grid plays the terminal: it is fed what the client would write
// (overlay and agent bytes, in queue order), the mirror only the agent's. After
// every restore the two must agree cell by cell (design.md §9, "Расхождение
// зеркала с терминалом").
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "predict_rig.hpp"
#include "zet/predict/overlay.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

struct Terminal {
    std::unique_ptr<zet::grid::Grid> G = MakeGrid(30, 5);
    zet::grid::VtParser P;
    void Feed(zet::ByteSpan b) { P.Feed(b.data(), b.size(), *G); }
    void Feed(std::string_view s) { Feed(Bytes(s)); }
};

bool SameScreen(const zet::grid::Grid& a, const zet::grid::Grid& b) {
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol()) {
        return false;
    }
    for (std::uint16_t r = 0; r < a.Rows(); ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const auto& x = a.CellAt(r, c);
            const auto& y = b.CellAt(r, c);
            const bool xb = x.Codepoint == 0 || x.Codepoint == U' ';
            const bool yb = y.Codepoint == 0 || y.Codepoint == U' ';
            if (xb != yb || (!xb && x.Codepoint != y.Codepoint) ||
                x.Attrs != y.Attrs || x.FgKind != y.FgKind ||
                x.BgKind != y.BgKind) {
                return false;
            }
        }
    }
    return true;
}

struct Session {
    Rig R;
    Terminal T;
    std::unique_ptr<Overlay> O = Overlay::Create();
    std::size_t Drawn = 0;

    explicit Session(EDisplayMode mode) : R(30, 5, mode) {}

    /// The client's order around a piece of agent bytes.
    void Agent(std::string_view bytes) {
        const auto undo = O->BuildUndo(R.View());
        T.Feed(undo);
        O->Commit(R.E);
        // What the terminal shows now is what the mirror shows.
        CHECK(SameScreen(*T.G, *R.G));
        T.Feed(bytes);
        R.Output(bytes);
        Refresh();
    }
    void Refresh() {
        const auto bytes = O->BuildUpdate(R.E, R.View());
        if (!bytes.empty()) ++Drawn;
        T.Feed(bytes);
        O->Commit(R.E);
    }
    void Key(std::string_view k) {
        R.Type(k);
        Refresh();
    }
};

}  // namespace

TEST_CASE("after every restore the terminal grid equals the mirror") {
    Session s(EDisplayMode::Always);
    s.Agent("\r$ ");
    // Two keys echoed confirm the epoch.
    s.Key("a");
    s.Agent("a");
    s.R.Ack();
    s.Key("B");
    s.Agent("B");
    s.R.Ack();
    s.Refresh();

    // Typing ahead: the predictions are on the terminal, the echo not yet.
    s.Key("c");
    s.Key("d");
    s.Key("\x7f");
    s.Key("e");
    CHECK(s.Drawn > 0);
    s.Agent("c");
    s.Agent("d\b \b");
    s.Agent("e");
    s.R.Ack();

    // The program answers with a line and a coloured prompt.
    s.Key("\r");
    s.Agent("\r\n\x1b[1;32mok\x1b[0m\r\n");
    s.Agent("$ ");
    s.Key("x");
    s.Agent("x");
    s.Key("y");
    s.Agent("\x1b[31m");  // an SGR between the keys
    s.Agent("y");
    s.Key("z");
    s.Agent("\x1b[");  // a sequence cut in the middle: nothing drawn here
    s.Agent("2Kz");
    s.Agent("");  // and the restore at the end
}

TEST_CASE("a mismatching echo is repaired by the next restore") {
    Session s(EDisplayMode::Always);
    s.Agent("\r$ ");
    s.Key("a");
    s.Agent("a");
    s.R.Ack();
    s.Key("B");
    s.Agent("B");
    s.R.Ack();
    s.Key("q");
    REQUIRE(s.Drawn > 0);
    // The program prints something else where the prediction was.
    s.Agent("Z");
    s.Agent("");  // the restore checks the terminal against the mirror
}
