// What the overlay and the engine refuse while the mirror holds something it
// cannot follow (design.md §9, round C, step C4b): a mode the grid does not
// model stops predictions and drawing; an SGR attribute the pen cannot hold
// stops drawing, because the overlay sets the pen back from the mirror and
// would lose it. Taking down what was drawn before is not affected.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>

#include "predict_rig.hpp"
#include "zet/grid/grid.hpp"
#include "zet/predict/overlay.hpp"

using namespace zet::predict;
using namespace zet::predict::test;
using zet::grid::EUnmodelledMode;

namespace {

std::string Str(zet::ByteSpan b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

struct Rig2 : Rig {
    std::unique_ptr<Overlay> O = Overlay::Create();

    Rig2() : Rig(40, 4, EDisplayMode::Always) {
        Prompt();
        ConfirmEpoch();
    }
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
    /// Bytes the mirror takes without the engine hearing of them: the state
    /// of the grid changes under a prediction that already exists.
    void Quiet(std::string_view s) { P.Feed(Bytes(s).data(), s.size(), *G); }
};

}  // namespace

TEST_CASE("nothing is drawn while an unmodelled SGR attribute is set") {
    for (const char* set :
         {"\x1b[53m", "\x1b[6m", "\x1b[58:2::1:2:3m", "\x1b[58;2;1;2;3m",
          "\x1b[51m", "\x1b[26m", "\x1b[73m"}) {
        CAPTURE(set);
        Rig2 r;
        r.Quiet(set);
        REQUIRE(r.G->HasUnmodelledSgr());
        r.Type("b");
        REQUIRE(r.Count() == 1);  // the engine still follows the key
        CHECK(r.Update().empty());
        CHECK_FALSE(r.O->Drawn());
        r.Quiet("\x1b[0m");
        CHECK_FALSE(r.Update().empty());
        CHECK(r.O->Drawn());
    }
}

TEST_CASE("the attribute's own reset lets the overlay draw again") {
    Rig2 r;
    r.Quiet("\x1b[53m");
    r.Type("b");
    CHECK(r.Update().empty());
    r.Quiet("\x1b[25m");  // another attribute's reset
    CHECK(r.Update().empty());
    r.Quiet("\x1b[55m");
    CHECK_FALSE(r.Update().empty());
}

TEST_CASE("what was drawn before the mark is taken down as usual") {
    Rig2 r;
    r.Type("b");
    REQUIRE_FALSE(r.Update().empty());
    REQUIRE(r.O->Drawn());
    // The agent's bytes set the attribute after the take-down in the client's
    // order; here it is set while the overlay stands, as if between the two.
    r.Quiet("\x1b[53m");
    REQUIRE(r.G->HasUnmodelledSgr());
    const std::string undo = r.Undo();
    CHECK_FALSE(undo.empty());
    CHECK(undo.find("\x1b[X") != std::string::npos);
    CHECK_FALSE(r.O->Drawn());
}

TEST_CASE(
    "nothing is drawn or predicted while a mode the grid does not model") {
    for (const EUnmodelledMode mode :
         {EUnmodelledMode::LeftRightMargins, EUnmodelledMode::ReverseWrap,
          EUnmodelledMode::ColumnMode, EUnmodelledMode::Utf8Off}) {
        CAPTURE(static_cast<int>(mode));
        // A prediction made before the mode is not drawn while it is set.
        Rig2 r;
        r.Type("b");
        REQUIRE(r.Count() == 1);
        r.G->SetUnmodelledMode(mode, true);
        CHECK(r.Update().empty());
        r.G->SetUnmodelledMode(mode, false);
        CHECK_FALSE(r.Update().empty());

        // A key typed while it is set is not predicted.
        Rig2 q;
        q.G->SetUnmodelledMode(mode, true);
        q.Type("b");
        CHECK(q.Count() == 0);
        CHECK(q.Update().empty());
    }
}
