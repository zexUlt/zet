/// The ground-wait force-reset (design.md §9, path B; M6 criteria "Ground
/// timeout" and `test_m6_mid_sequence_cut`), on the agent's GridPump.
///
/// A snapshot is never built inside a DCS: the pump waits for Ground, and past
/// GROUND_WAIT_TIMEOUT it resets the parser, flags the snapshot
/// PossiblyDiverged and builds anyway.  What the shell prints afterwards is
/// parsed from Ground.
///
/// Mutants these tests were checked against (applied by hand):
///   DoBuild: no SetPossiblyDiverged(false) after the build
///                              -> "ground timeout", "mid-sequence cut: DCS"
///   DoBuild: no Parser_.Reset() on the forced path
///                              -> all three
///   VtParser: ESC \ prints the backslash
///                              -> "ground timeout", "mid-sequence cut: DCS"

#include "agent_rig.hpp"

#include <string>

namespace {

/// Past the deadline GridPump::OnNeedsSnapshot arms from START.
const Timestamp TIMED_OUT =
    START + GROUND_WAIT_TIMEOUT + std::chrono::milliseconds{1};

const std::string SIXEL = "\x1bP0;0;0q\"1;1;8;8#0;2;100;0;0#0!8~$#0!8N\x1b\\";

void Feed(GridPump& pump, const std::string& text) {
    const auto b = Bytes(text);
    pump.FeedBytes(b.data(), b.size());
}

/// Arms the wait on a pump whose parser is not in Ground, checks that nothing
/// is built before the deadline, and returns the snapshot forced out at it.
GridPump::SnapReady ForcedSnapshot(GridPump& pump) {
    REQUIRE(pump.ParserState() != EParserState::Ground);
    CHECK_FALSE(pump.OnNeedsSnapshot(24, 80, START).has_value());
    CHECK(pump.HasPendingSnap());
    {
        auto early = pump.TryBuild(START + std::chrono::seconds{1});
        REQUIRE(early.has_value());
        CHECK_FALSE(early->has_value());
    }
    auto forced = pump.TryBuild(TIMED_OUT);
    REQUIRE(forced.has_value());
    REQUIRE(forced->has_value());
    return **forced;
}

}  // namespace

// Kills: DoBuild leaves the parser inside the DCS (not Ground after the
// build); never flags the snapshot (Flags == 0); never clears the grid's flag
// (the next snapshot is flagged too); or loses the next DCS.

TEST_CASE_FIXTURE(SodiumFixture,
                  "ground timeout on an unterminated DCS, then a complete DCS "
                  "passes and text parses normally") {
    auto pump = MakePump();
    Feed(*pump, "AB");
    Feed(*pump, "\x1bP0;0;0q\"1;1;8;8#0;2;100;0;0#0!8~");  // no ST
    REQUIRE(pump->ParserState() == EParserState::Bypass);

    const auto forced = ForcedSnapshot(*pump);
    CHECK((forced.Flags & 1U) == 1U);
    CHECK(pump->ParserState() == EParserState::Ground);
    CHECK_FALSE(pump->GetGrid().PossiblyDiverged());  // the build took it
    {
        const auto shown = ParseInto(forced.Bytes, 80, 24);
        CHECK(RowText(*shown, 0) == "AB");  // the half image drew nothing
        CHECK(shown->CursorCol() == 2);
    }
    pump->ReleaseSnapshot();

    // A complete DCS now, then text.
    Feed(*pump, SIXEL);
    CHECK(pump->ParserState() == EParserState::Ground);
    Feed(*pump, "CD");
    CHECK(pump->ParserState() == EParserState::Ground);
    CHECK(RowText(pump->GetGrid(), 0) == "ABCD");
    CHECK(pump->GetGrid().CursorCol() == 4);

    // The forced reset does not stick: the next snapshot is not flagged.
    const auto next = BuildNow(*pump, 24, 80);
    CHECK(next.Flags == 0);
    const auto shown = ParseInto(next.Bytes, 80, 24);
    CHECK(RowText(*shown, 0) == "ABCD");
    pump->ReleaseSnapshot();
}

// The stream stops inside a CSI.  The design only says the parser is reset to
// Ground and the snapshot is flagged; the rest of the sequence then parses as
// text, as for a DCS.
//
// Kills: a forced build that leaves the parser inside the CSI (not Ground,
// and the tail "1H" would act as a cursor move), or that is not flagged.

TEST_CASE_FIXTURE(SodiumFixture, "mid-sequence cut: CSI") {
    auto pump = MakePump();
    Feed(*pump, "AB\x1b[3;1");  // CSI cut after two parameters
    REQUIRE(pump->ParserState() == EParserState::CsiParam);

    const auto forced = ForcedSnapshot(*pump);
    CHECK((forced.Flags & 1U) == 1U);
    CHECK(pump->ParserState() == EParserState::Ground);
    {
        const auto shown = ParseInto(forced.Bytes, 80, 24);
        CHECK(RowText(*shown, 0) == "AB");
        CHECK(shown->CursorRow() == 0);
        CHECK(shown->CursorCol() == 2);
    }
    pump->ReleaseSnapshot();

    // The tail of the cut CSI is text now; the next CSI has fresh parameters.
    Feed(*pump, "1HZ\x1b[2;1Hq");
    CHECK(pump->ParserState() == EParserState::Ground);
    CHECK(RowText(pump->GetGrid(), 0) == "AB1HZ");
    CHECK(RowText(pump->GetGrid(), 1) == "q");
}

// The stream stops inside a DCS.  Design §9: the rest of the DCS reaches the
// terminal as ordinary text, the string terminator is swallowed.
//
// Kills: a parser that prints the ST's backslash, swallows the rest of the
// DCS after the reset, or does not leave Ground-wait flagged.

TEST_CASE_FIXTURE(SodiumFixture, "mid-sequence cut: DCS") {
    auto pump = MakePump();
    Feed(*pump, "AB\x1bP0;0;0q\"1;1;8;8#0;2;100;0;0#0!8");  // cut in the data
    REQUIRE(pump->ParserState() == EParserState::Bypass);

    const auto forced = ForcedSnapshot(*pump);
    CHECK((forced.Flags & 1U) == 1U);
    CHECK(pump->ParserState() == EParserState::Ground);
    {
        const auto shown = ParseInto(forced.Bytes, 80, 24);
        CHECK(RowText(*shown, 0) == "AB");
        CHECK(shown->CursorCol() == 2);
    }
    pump->ReleaseSnapshot();

    // The remaining DCS bytes, its ST, then ordinary text.
    Feed(*pump, "~~$#0!8N\x1b\\CD");
    CHECK(pump->ParserState() == EParserState::Ground);
    CHECK(RowText(pump->GetGrid(), 0) == "AB~~$#0!8NCD");
    for (std::uint16_t row = 1; row < pump->GetGrid().Rows(); ++row) {
        CHECK(RowText(pump->GetGrid(), row).empty());
    }

    // The next snapshot is the grid as it now is, unflagged.
    const auto next = BuildNow(*pump, 24, 80);
    CHECK(next.Flags == 0);
    const auto shown = ParseInto(next.Bytes, 80, 24);
    CHECK(RowText(*shown, 0) == "AB~~$#0!8NCD");
    pump->ReleaseSnapshot();
}
