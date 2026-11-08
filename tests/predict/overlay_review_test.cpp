// Review of PR #142: cases where the overlay leaves the real terminal different
// from what the agent's bytes alone would have left. A second grid plays the
// terminal (fed overlay + agent bytes), a third the reference terminal (fed
// the agent's bytes only). After a restore the two must agree.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "predict_rig.hpp"
#include "zet/grid/limits.hpp"
#include "zet/predict/overlay.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

struct Term {
    std::unique_ptr<zet::grid::Grid> G;
    zet::grid::VtParser P;
    Term(int cols, int rows) : G(MakeGrid(cols, rows)) {}
    void Feed(zet::ByteSpan b) { P.Feed(b.data(), b.size(), *G); }
    void Feed(std::string_view s) { Feed(Bytes(s)); }
};

bool Blank(const zet::grid::Cell& c) {
    return c.Codepoint == 0 || c.Codepoint == U' ';
}

/// Cell text and cursor of two grids, compared.
bool SameText(const zet::grid::Grid& a, const zet::grid::Grid& b) {
    if (a.Rows() != b.Rows() || a.Cols() != b.Cols()) return false;
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol()) {
        return false;
    }
    for (std::uint16_t r = 0; r < a.Rows(); ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const auto& x = a.CellAt(r, c);
            const auto& y = b.CellAt(r, c);
            if (Blank(x) != Blank(y)) return false;
            if (!Blank(x) && (x.Codepoint != y.Codepoint || x.Width != y.Width))
                return false;
        }
    }
    return true;
}

/// The client's order: undo, agent bytes, mirror, redraw. `Real` gets the
/// overlay and the agent's bytes, `Ref` only the agent's bytes.
struct Client {
    Rig R;
    Term Real;
    Term Ref;
    std::unique_ptr<Overlay> O = Overlay::Create();
    std::size_t Draws = 0;
    /// Bytes the overlay has put on the real terminal.
    std::size_t Written = 0;

    Client(int cols, int rows, EDisplayMode mode)
        : R(cols, rows, mode), Real(cols, rows), Ref(cols, rows) {}

    /// Bytes on the local terminal before zet started: not in the mirror.
    void Before(std::string_view s) {
        Real.Feed(s);
        Ref.Feed(s);
    }
    void Undo() {
        const auto b = O->BuildUndo(R.View());
        Written += b.size();
        Real.Feed(b);
        O->Commit(R.E);
    }
    /// The terminal and the mirror change size. The overlay is taken off only
    /// if the restore is still right afterwards, as Client::Resize does it;
    /// otherwise it is forgotten without a byte. The terminal here does not
    /// reflow: a test that wants it says what it would have done.
    void Resize(std::uint16_t cols, std::uint16_t rows) {
        if (O->UndoSurvivesResize(R.View(), cols)) {
            Undo();
        } else {
            O->Forget();
            R.E.ClearDrawn();
        }
        REQUIRE(Real.G->Resize(cols, rows).has_value());
        REQUIRE(Ref.G->Resize(cols, rows).has_value());
        REQUIRE(R.G->Resize(cols, rows).has_value());
        R.E.OnResize(R.Now, R.View());
    }
    void Agent(std::string_view bytes) {
        Undo();
        Real.Feed(bytes);
        Ref.Feed(bytes);
        R.Output(bytes);
        Refresh();
    }
    void Refresh() {
        const auto b = O->BuildUpdate(R.E, R.View());
        if (!b.empty()) ++Draws;
        Written += b.size();
        Real.Feed(b);
        O->Commit(R.E);
    }
    void Key(std::string_view k) {
        R.Type(k);
        Refresh();
    }
    /// Prompt and two echoed keys: a confirmed epoch, cursor at column 4.
    void Start() {
        Agent("\r$ ");
        Key("a");
        Agent("a");
        R.Ack();
        Key("B");
        Agent("B");
        R.Ack();
        Refresh();
    }
};

}  // namespace

TEST_CASE(
    "review: a prediction under a wide glyph that arrived later is taken off") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");  // predicted at column 4
    REQUIRE(c.R.Count() == 1);
    REQUIRE(c.O->Drawn());
    // The program redraws the line so that a wide glyph covers columns 3-4
    // (a prompt segment that grew, say). Unexplained output only raises the
    // epoch: the confirmed prediction stays visible at column 4.
    c.Agent("\x1b[4G\xe4\xb8\xad");  // CHA 4, U+4E2D
    REQUIRE(c.R.G->CellAt(0, 4).Width == 0);
    // The overlay draws 'c' on the right half of the wide glyph; the next
    // restore skips cells whose width is not 1 and leaves 'c' there.
    c.Agent("");
    CHECK(SameText(*c.Real.G, *c.Ref.G));
}

TEST_CASE(
    "review: nothing is drawn after the parser gave up on a long OSC string") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    // An OSC longer than BYPASS_LIMIT_BYTES (iTerm2 inline image, OSC 52 of a
    // big selection). The mirror's parser forces Ground at the limit; the
    // real terminal is still inside the string.
    c.Undo();
    std::string osc = "\x1b]1337;File=inline=1:";
    c.R.P.Feed(Bytes(osc).data(), osc.size(), *c.R.G);
    std::string chunk(4096, 'A');
    std::size_t fed = 0;
    while (c.R.P.State() != zet::grid::EParserState::Ground &&
           fed < 2 * zet::grid::BYPASS_LIMIT_BYTES) {
        c.R.P.Feed(Bytes(std::string_view{chunk.data(), 1}).data(), 1, *c.R.G);
        ++fed;
    }
    REQUIRE(c.R.P.State() == zet::grid::EParserState::Ground);
    c.R.Out += osc.size() + fed;
    c.R.E.OnOutput(c.R.Out, c.R.Now, c.R.View());
    // Whatever goes out now lands inside the terminal's OSC string: ESC [
    // cancels it there, and the rest of the payload is printed as text.
    CHECK(c.O->BuildUpdate(c.R.E, c.R.View()).empty());
}

// Round B (row sync before absolute positioning): not fixed by this round.
TEST_CASE(
    "review: a cursor-up above the mirror's first row puts the overlay on a "
    "row of the terminal the mirror never saw" *
    doctest::skip()) {
    Client c(30, 6, EDisplayMode::Always);
    // zet starts on row 3 of a terminal that already shows the local shell.
    c.Before("local$ ls\r\nfile1 file2 file3\r\nlocal$ zet host\r\n");
    c.Start();  // prompt on terminal row 3, mirror row 0
    // A program moves the cursor up one line and back to column 4 (a
    // two-line prompt redrawn, a progress line). The terminal goes to row 2,
    // the mirror stays on row 0: no reset, the column stays trusted.
    c.Agent("\x1b[A\x1b[5G");
    c.Key("x");
    REQUIRE(c.Draws > 0);
    c.Agent("");
    // The restore wrote the mirror's row 0 over the terminal's row 2.
    CHECK(SameText(*c.Real.G, *c.Ref.G));
}

TEST_CASE(
    "review: a narrowing resize leaves a drawn column outside the new width "
    "alone") {
    Client c(30, 5, EDisplayMode::Always);
    c.Agent("\r$ 0123456789");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");  // predicted at column 14
    REQUIRE(c.O->Drawn());
    // The terminal is narrowed to 10 columns; SIGWINCH reaches the client
    // after the terminal has already resized. CHA 15 would be clamped to
    // column 10 and ECH would erase '7': nothing may be written.
    const std::size_t before = c.Written;
    c.Resize(10, 5);
    CHECK(c.Written == before);
    CHECK_FALSE(c.O->Drawn());
    CHECK(SameText(*c.Real.G, *c.Ref.G));
}

TEST_CASE("review: a narrowing resize that keeps every column is undone") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");  // column 4, cursor 5
    REQUIRE(c.O->Drawn());
    const std::size_t before = c.Written;
    c.Resize(10, 5);
    CHECK(c.Written > before);
    CHECK_FALSE(c.O->Drawn());
    CHECK(SameText(*c.Real.G, *c.Ref.G));
}

TEST_CASE(
    "review: a widening resize on a row that continues a wrapped one is not "
    "undone (the terminal reflows it)") {
    Client c(10, 5, EDisplayMode::Always);
    // A full row, then the prompt on the next: the terminal joins the two
    // when it widens, and the cursor's row and column are not the mirror's.
    c.Agent("\r0123456789$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    REQUIRE(c.R.G->CursorRow() == 1);
    const std::size_t before = c.Written;
    c.Resize(20, 5);
    CHECK(c.Written == before);
    CHECK_FALSE(c.O->Drawn());
}

TEST_CASE(
    "review: a widening resize on a row that is wrapped itself is not "
    "undone") {
    Client c(10, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    // The program prints in the last column of the row (a right-hand prompt)
    // and goes back; the confirmed prediction stays up.
    c.Agent("\x1b[10G*\x1b[6G");
    REQUIRE(c.R.G->CursorRow() == 0);
    REQUIRE(c.O->Drawn());
    const std::size_t before = c.Written;
    c.Resize(20, 5);
    CHECK(c.Written == before);
    CHECK_FALSE(c.O->Drawn());
}

TEST_CASE("review: a widening resize on a plain row is undone") {
    Client c(10, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    const std::size_t before = c.Written;
    c.Resize(20, 5);
    CHECK(c.Written > before);
    CHECK(SameText(*c.Real.G, *c.Ref.G));
}

TEST_CASE(
    "review: echoed predictions with no EchoAck to come are redrawn "
    "underlined after 5 s (path-A reconnect, baseline only)") {
    Client c(30, 5, EDisplayMode::Adaptive);
    c.Start();
    c.Key("x");
    c.Key("y");
    // The link drops; on path A the replay brings the echo. The new
    // connection's first EchoAck is a baseline only, ahead of the replay, and
    // the agent sends no other until the user types again.
    c.R.Baseline();
    c.Agent("xy");
    const std::size_t before = c.Draws;
    c.R.Advance(std::chrono::milliseconds{5100});
    c.R.E.OnTick(c.R.Now);
    c.Refresh();
    // Nothing should be written over a correct echo.
    CHECK(c.Draws == before);
}
