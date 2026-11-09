// The rig for the overlay against a terminal that is not the mirror. A second
// grid plays the terminal ("Real": the bytes the client queues, overlay and
// agent's, in queue order), a third the terminal as it would be without the
// overlay ("Ref": the agent's bytes only). Both start from the same content,
// which the mirror does not have: zet starts on a terminal that already shows
// the local shell, with the cursor on some row N, and the mirror starts blank
// at row 0 as the client creates it. After every restore Real must equal Ref
// (design.md §9, "Расхождение зеркала с терминалом"); a desync between the
// mirror and the terminal cannot pass unseen here, as it can when the terminal
// is a copy of the mirror.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "predict_rig.hpp"
#include "zet/predict/overlay.hpp"

namespace zet::predict::test {

struct Term {
    std::unique_ptr<zet::grid::Grid> G;
    zet::grid::VtParser P;
    Term(int cols, int rows) : G(MakeGrid(cols, rows)) {}
    void Feed(zet::ByteSpan b) { P.Feed(b.data(), b.size(), *G); }
    void Feed(std::string_view s) { Feed(Bytes(s)); }
};

inline bool Blank(const zet::grid::Cell& c) {
    return c.Codepoint == 0 || c.Codepoint == U' ';
}

/// Whether two screens are the same: cells (a cell that is blank in either way
/// counts once, ECH and a space being the same to the eye), the cursor, the
/// pen, and the character REP would repeat.
inline bool SameScreen(const zet::grid::Grid& a, const zet::grid::Grid& b) {
    if (a.Rows() != b.Rows() || a.Cols() != b.Cols()) return false;
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol()) {
        return false;
    }
    if (a.PenAttrs() != b.PenAttrs() || a.PenFgKind() != b.PenFgKind() ||
        a.PenBgKind() != b.PenBgKind() ||
        a.LastPrinted().Codepoint != b.LastPrinted().Codepoint) {
        return false;
    }
    for (std::uint16_t r = 0; r < a.Rows(); ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const auto& x = a.CellAt(r, c);
            const auto& y = b.CellAt(r, c);
            if (Blank(x) != Blank(y)) return false;
            if (x.BgKind != y.BgKind) return false;
            if (!Blank(x) &&
                (x.Codepoint != y.Codepoint || x.Width != y.Width ||
                 x.Attrs != y.Attrs || x.FgKind != y.FgKind))
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
    /// The bytes of the last drawing.
    std::string LastDrawn;
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
        // The restore: the terminal is what it would be without the overlay.
        CHECK(SameScreen(*Real.G, *Ref.G));
        Real.Feed(bytes);
        Ref.Feed(bytes);
        R.Output(bytes);
        Refresh();
    }
    void Refresh() {
        const auto b = O->BuildUpdate(R.E, R.View());
        if (!b.empty()) {
            ++Draws;
            LastDrawn.assign(reinterpret_cast<const char*>(b.data()), b.size());
        }
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

}  // namespace zet::predict::test
