// A saved-cursor slot written before the rows were synced holds a row of the
// terminal that is not the mirror's row of that number (design.md §9, "Строки
// до первой синхронизации"): restoring it from a synced state takes the rows
// away again and counts as an unknown sequence.
#include <doctest/doctest.h>

#include <memory>
#include <string_view>

#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

using namespace std::string_view_literals;

constexpr std::string_view SYNC = "\x1b[2J\x1b[H"sv;
constexpr std::string_view DECSC =
    "\x1b"
    "7"sv;
constexpr std::string_view DECRC =
    "\x1b"
    "8"sv;
constexpr std::string_view RIS =
    "\x1b"
    "c"sv;
constexpr std::string_view DECSTR = "\x1b[!p"sv;
constexpr std::string_view TO_ALT = "\x1b[?47h"sv;  // switch only, no slot
constexpr std::string_view TO_MAIN = "\x1b[?47l"sv;

struct Rig {
    std::unique_ptr<Grid> G;
    VtParser P;

    Rig() {
        auto r = Grid::Create(10, 4, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
    /// Feeds `s` and says by how much the unknown count grew.
    std::uint64_t Unknown(std::string_view s) {
        const auto before = P.UnknownSeqCount();
        Feed(s);
        return P.UnknownSeqCount() - before;
    }
};

// A save and a restore of one kind, on the slot of the current screen.
struct Pair {
    const char* Name;
    std::string_view Save;
    std::string_view Restore;
};

constexpr Pair PAIRS[] = {
    {"DECSC/DECRC", DECSC, DECRC},
    {"SCP/RCP", "\x1b[s"sv, "\x1b[u"sv},
    {"?1048", "\x1b[?1048h"sv, "\x1b[?1048l"sv},
};

}  // namespace

TEST_CASE("saved slot: a slot written with the rows synced keeps them") {
    for (const Pair& p : PAIRS) {
        CAPTURE(p.Name);
        Rig r;
        r.Feed(SYNC);
        r.Feed("\x1b[3;4H"sv);
        r.Feed(p.Save);
        r.Feed("\x1b[H"sv);
        CHECK(r.Unknown(p.Restore) == 0);
        CHECK(r.G->RowsTrusted());
        CHECK(r.G->CursorRow() == 2);
        CHECK(r.G->CursorCol() == 3);
    }
}

TEST_CASE("saved slot: a slot written before the sync takes the rows away") {
    for (const Pair& p : PAIRS) {
        CAPTURE(p.Name);
        Rig r;
        r.Feed(p.Save);
        r.Feed(SYNC);
        r.Feed("\x1b[3;4H"sv);
        REQUIRE(r.G->RowsTrusted());
        CHECK(r.Unknown(p.Restore) == 1);
        CHECK_FALSE(r.G->RowsTrusted());
        // The cursor still goes where the slot says: a stale slot is a doubt
        // about the row, not a refusal to restore.
        CHECK(r.G->CursorRow() == 0);
    }
}

TEST_CASE("saved slot: a restore with nothing saved is from a stale slot") {
    Rig r;
    r.Feed(SYNC);
    CHECK(r.Unknown(DECRC) == 1);
    CHECK_FALSE(r.G->RowsTrusted());
}

TEST_CASE(
    "saved slot: before the sync a restore loses the rows but does not count") {
    // zsh's right prompt saves and restores the cursor before any answer
    // came; counting it would reset the engine on every prompt of a terminal
    // that never answers.
    Rig r;
    r.Feed(DECSC);
    CHECK(r.G->RowsTrusted());
    CHECK(r.Unknown(DECRC) == 0);
    CHECK_FALSE(r.G->RowsTrusted());
}

TEST_CASE("saved slot: the next sync gives the rows back, and a new slot") {
    Rig r;
    r.Feed(DECSC);
    r.Feed(SYNC);
    r.Feed(DECRC);
    REQUIRE_FALSE(r.G->RowsTrusted());
    r.Feed(SYNC);
    CHECK(r.G->RowsTrusted());
    r.Feed(DECSC);
    CHECK(r.Unknown(DECRC) == 0);
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("saved slot: ?1049l restores the main slot as ?1048l does") {
    {
        // The fuzzer's case: ?1049h before the sync, a second ?1049l after it.
        Rig r;
        r.Feed("\x1b[?1049h\x1b[?1049l"sv);
        r.Feed(SYNC);
        REQUIRE(r.G->RowsTrusted());
        CHECK(r.Unknown("\x1b[?1049l"sv) == 1);
        CHECK_FALSE(r.G->RowsTrusted());
    }
    {
        Rig r;
        r.Feed(SYNC);
        r.Feed("\x1b[?1049h"sv);
        CHECK(r.Unknown("\x1b[?1049l"sv) == 0);
        CHECK(r.G->RowsTrusted());
    }
    {
        // A ?1049h on the main screen after the sync rewrites the slot.
        Rig r;
        r.Feed("\x1b[?1049h\x1b[?1049l"sv);
        r.Feed(SYNC);
        r.Feed("\x1b[?1049h"sv);
        CHECK(r.Unknown("\x1b[?1049l"sv) == 0);
        CHECK(r.G->RowsTrusted());
    }
}

TEST_CASE("saved slot: the alt screen has a slot of its own") {
    {
        // A stale alt slot beside a fresh main one.
        Rig r;
        r.Feed(TO_ALT);
        r.Feed(DECSC);
        r.Feed(TO_MAIN);
        r.Feed(SYNC);
        r.Feed(DECSC);
        CHECK(r.Unknown(DECRC) == 0);
        r.Feed(TO_ALT);
        CHECK(r.Unknown(DECRC) == 1);
        CHECK_FALSE(r.G->RowsTrusted());
    }
    {
        // A fresh alt slot beside a stale main one.
        Rig r;
        r.Feed(DECSC);
        r.Feed(SYNC);
        r.Feed(TO_ALT);
        r.Feed(DECSC);
        CHECK(r.Unknown(DECRC) == 0);
        CHECK(r.G->RowsTrusted());
        r.Feed(TO_MAIN);
        CHECK(r.Unknown(DECRC) == 1);
        CHECK_FALSE(r.G->RowsTrusted());
    }
}

TEST_CASE("saved slot: a resync makes both slots stale") {
    Rig r;
    r.Feed(SYNC);
    r.Feed(DECSC);
    r.Feed(TO_ALT);
    r.Feed(DECSC);
    r.Feed(TO_MAIN);
    REQUIRE(r.G->Resync(2, 3, true));
    REQUIRE(r.G->RowsTrusted());
    CHECK(r.Unknown(DECRC) == 1);
    CHECK_FALSE(r.G->RowsTrusted());
    // The alt slot: the resync is refused on the alt screen, so go there
    // after it.
    REQUIRE(r.G->Resync(2, 3, true));
    r.Feed(TO_ALT);
    CHECK(r.Unknown(DECRC) == 1);
    CHECK_FALSE(r.G->RowsTrusted());
}

TEST_CASE("saved slot: a slot written after a resync is fresh") {
    Rig r;
    REQUIRE(r.G->Resync(2, 3, true));
    r.Feed(DECSC);
    CHECK(r.Unknown(DECRC) == 0);
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("saved slot: RIS resets the slots on both sides, fresh") {
    Rig r;
    r.Feed(DECSC);
    r.Feed(TO_ALT);
    r.Feed(DECSC);
    r.Feed(TO_MAIN);  // both stale
    r.Feed(RIS);
    REQUIRE(r.G->RowsTrusted());
    CHECK(r.Unknown(DECRC) == 0);
    CHECK(r.G->RowsTrusted());
    r.Feed(TO_ALT);
    CHECK(r.Unknown(DECRC) == 0);
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("saved slot: DECSTR leaves the slots as they were") {
    {
        Rig r;  // stale stays stale
        r.Feed(DECSC);
        r.Feed(SYNC);
        r.Feed(DECSTR);
        CHECK(r.Unknown(DECRC) == 1);
        CHECK_FALSE(r.G->RowsTrusted());
    }
    {
        Rig r;  // fresh stays fresh
        r.Feed(SYNC);
        r.Feed(DECSC);
        r.Feed(DECSTR);
        CHECK(r.Unknown(DECRC) == 0);
        CHECK(r.G->RowsTrusted());
    }
}
