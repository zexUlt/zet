/// Sixel passthrough (design.md §9, "Sixel"; M6 criterion
/// `test_m6_sixel_passthrough`).
///
/// A sixel image is a DCS sequence.  It reaches the terminal byte for byte in
/// one connection, the grid does not draw it and ends in Ground, and a
/// snapshot taken afterwards holds the text cells around the image position
/// and nothing else.
///
/// Mutants these tests were checked against (applied by hand):
///   VtParser: ESC inside Bypass is ignored (the ST never ends the DCS)
///                                                  -> both tests fail
///   Agent MovePtyOutput: `PtyFrom_ += *taken + 1` (a byte is skipped on
///   every write to the session)                    -> the agent test fails

#include "agent_rig.hpp"

#include <string>
#include <string_view>

namespace {

/// A hand-written but valid sixel image: raster attributes, two colour
/// definitions, run-length repeats, graphic carriage return (`$`) and graphic
/// newline (`-`), closed by ST.
const std::string SIXEL =
    "\x1bP0;0;0q\"1;1;12;12#0;2;100;0;0#1;2;0;100;0#0!12~$#1!6?!6~-#0!"
    "12N\x1b\\";

std::string Join(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (const auto part : parts) {
        out += part;
    }
    return out;
}

std::vector<std::byte> AsBytes(const std::string& text) { return Bytes(text); }

}  // namespace

// The grid ignores the picture: whatever way the pty chops the stream, the
// snapshot is the one of the same text without the image.
//
// Kills: a parser that prints DCS payload, or does not return to Ground after
// ST, in some fragmentation (the snapshot differs from the reference).

TEST_CASE_FIXTURE(SodiumFixture,
                  "sixel: any split of the image leaves the grid as without "
                  "the image") {
    const std::string head = "row\r\nA";
    const std::string tail = "B\x1b[1;1HOVER";
    const auto ref = ReferenceSnapshot({AsBytes(head), AsBytes(tail)}, 24, 80);

    for (std::size_t cut = 0; cut <= SIXEL.size(); ++cut) {
        INFO("cut at " << cut);
        auto pump = MakePump();
        const auto feed = [&](const std::string& s) {
            const auto b = AsBytes(s);
            pump->FeedBytes(b.data(), b.size());
        };
        feed(head);
        feed(SIXEL.substr(0, cut));
        feed(SIXEL.substr(cut));
        feed(tail);
        REQUIRE(pump->ParserState() == EParserState::Ground);
        CHECK_FALSE(pump->GetGrid().PossiblyDiverged());

        const auto ready = BuildNow(*pump, 24, 80);
        CHECK(ready.Flags == 0);
        CHECK(std::vector<std::byte>{ready.Bytes.begin(), ready.Bytes.end()} ==
              ref);
        pump->ReleaseSnapshot();
    }
}

// The agent's output path, pty to client, with the image cut into 7-byte
// writes: the client gets exactly the shell's bytes, the agent's parser is
// back in Ground, and the snapshot of the agent's grid renders the text and
// nothing of the image.
//
// Kills: the agent forwarding anything but the pty's bytes (the delivered
// bytes differ), and a parser left inside or garbling the DCS (the snapshot
// differs from the reference, the parser is not in Ground).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a sixel image passes through byte for byte and the "
                  "grid shows only the text") {
    AgentRig rig{81};
    auto c = rig.Connect();
    c->QueueResize(24, 80);
    REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 2; }));
    rig.AwaitResumeTaken(*c);  // prints "." and sees it delivered

    const std::string before = "before";
    const std::string after = "after";
    const std::string over = "\x1b[1;1HOVER";
    const std::string stream = Join({before, SIXEL, after, over});

    for (std::size_t at = 0; at < stream.size(); at += 7) {
        const auto piece = stream.substr(at, 7);
        rig.PtyPrints(AsBytes(piece));
        SleepMs(2);
    }
    const auto expected = AsBytes("." + stream);
    REQUIRE(c->RunUntil(
        8000, [&] { return c->Delivered.size() >= expected.size(); }));
    c->Run(100);
    CHECK(c->Delivered == expected);

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.ParserInGround);
    CHECK_FALSE(seen.PossiblyDiverged);
    CHECK(seen.GridBytes == expected.size());

    // Same text, no image.
    const auto ref =
        ReferenceSnapshot({AsBytes("." + before + after + over)}, 24, 80);
    CHECK(seen.GridSnapshot == ref);

    // What a terminal shows after that snapshot.
    const auto shown = ParseInto(seen.GridSnapshot, 80, 24);
    CHECK(RowText(*shown, 0) == "OVERoreafter");
    for (std::uint16_t row = 1; row < shown->Rows(); ++row) {
        CHECK(RowText(*shown, row).empty());
    }
    CHECK(shown->CursorRow() == 0);
    CHECK(shown->CursorCol() == 4);
}
