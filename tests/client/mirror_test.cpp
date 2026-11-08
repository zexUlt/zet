/// The client's mirror grid (design.md §9, "Предиктивное эхо"): fed what the
/// terminal is sent, it must end up in the state the agent's grid reaches from
/// the same bytes, compared the way the M6 fidelity tests compare grids: by
/// the snapshot encoding, which names every cell, pen, mode and cursor.

#include "mirror.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

namespace {

using namespace zet;

ByteSpan Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// What the agent keeps: a grid with scrollback.
struct AgentGrid {
    explicit AgentGrid(std::uint16_t cols, std::uint16_t rows,
                       std::uint32_t history = 1000) {
        auto created = grid::Grid::Create(cols, rows, history);
        REQUIRE(created.has_value());
        Grid.reset(*created);
    }
    void Feed(std::string_view text) {
        Parser.Feed(Bytes(text).data(), text.size(), *Grid);
    }
    std::unique_ptr<grid::Grid> Grid;
    grid::VtParser Parser;
};

/// The whole snapshot encoding of `g`.
std::vector<std::byte> Encode(const grid::Grid& g) {
    auto snap = grid::Snapshot::Create(g);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    const auto bytes = owned->Bytes();
    return {bytes.begin(), bytes.end()};
}

/// The screen section of the encoding: what a mirror without history can
/// share with an agent grid that has some.
std::vector<std::byte> EncodeScreen(const grid::Grid& g) {
    auto snap = grid::Snapshot::Create(g);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    const auto bytes = owned->Bytes().subspan(owned->ScrollbackLen());
    return {bytes.begin(), bytes.end()};
}

std::unique_ptr<client::Mirror> NewMirror(std::uint16_t cols,
                                          std::uint16_t rows) {
    auto mirror = client::Mirror::Create(cols, rows);
    REQUIRE(mirror != nullptr);
    return mirror;
}

/// Output that exercises scrolling, wrapping, colour, cursor addressing, a
/// scroll region, the alternate screen, wide and combining characters, and
/// tabs. Long enough to scroll a 10-row screen several times.
std::string Stream() {
    std::string s;
    for (int i = 0; i < 25; ++i) {
        s += "line " + std::to_string(i) + " of the \x1b[3" +
             std::to_string(i % 8) + "mstream\x1b[0m\r\n";
    }
    s += "a very long line that is certain to wrap around the right edge of "
         "the forty column screen\r\n";
    s += "\x1b[3;5Hmoved\x1b[1;4;38;2;10;20;30;48;5;99mpen\x1b[m";
    s += "\x1b[2;8r\x1b[8;1H\n\n\x1b[r";
    s += "tab\there\t\tthere \xe6\x97\xa5\xe6\x9c\xac e\xcc\x81x\r\n";
    s += "\x1b[?1049h\x1b[2J\x1b[5;5Halt screen\x1b[?1049l";
    s += "\x1b[?2004h\x1b[?25l\x1b]0;a title\x07\x1b[10;3Hdone";
    return s;
}

}  // namespace

TEST_CASE("the mirror equals the agent's grid after a stream") {
    AgentGrid agent{40, 10, 0};
    auto mirror = NewMirror(40, 10);
    const std::string stream = Stream();
    agent.Feed(stream);
    mirror->Feed(Bytes(stream));
    CHECK(Encode(mirror->Screen()) == Encode(*agent.Grid));
}

TEST_CASE(
    "the mirror, which has no history, matches the screen of a grid that has") {
    AgentGrid agent{40, 10};
    auto mirror = NewMirror(40, 10);
    const std::string stream = Stream();
    agent.Feed(stream);
    mirror->Feed(Bytes(stream));
    CHECK(mirror->Screen().ScrollbackSize() == 0);
    CHECK(agent.Grid->ScrollbackSize() > 0);
    CHECK(EncodeScreen(mirror->Screen()) == EncodeScreen(*agent.Grid));
}

TEST_CASE("how the stream is cut into reads does not change the mirror") {
    AgentGrid agent{40, 10, 0};
    auto mirror = NewMirror(40, 10);
    const std::string stream = Stream();
    agent.Feed(stream);
    // One byte at a time cuts every escape sequence and UTF-8 sequence.
    for (std::size_t i = 0; i < stream.size(); ++i) {
        mirror->Feed(Bytes(stream).subspan(i, 1));
    }
    CHECK(Encode(mirror->Screen()) == Encode(*agent.Grid));
}

TEST_CASE("an empty feed changes nothing") {
    auto mirror = NewMirror(40, 10);
    mirror->Feed(Bytes("abc"));
    const auto before = Encode(mirror->Screen());
    mirror->Feed({});
    CHECK(Encode(mirror->Screen()) == before);
}

TEST_CASE("the mirror holds no history, whatever the stream scrolls") {
    auto mirror = NewMirror(40, 10);
    for (int i = 0; i < 100; ++i) {
        mirror->Feed(Bytes("scrolling away\r\n"));
    }
    CHECK(mirror->Screen().ScrollbackLimit() == 0);
    CHECK(mirror->Screen().ScrollbackSize() == 0);
}

TEST_CASE("an oversize terminal is clamped like the agent's grid") {
    auto mirror = NewMirror(60000, 60000);
    AgentGrid agent{60000, 60000, 0};
    CHECK(mirror->Screen().Cols() == agent.Grid->Cols());
    CHECK(mirror->Screen().Rows() == agent.Grid->Rows());
    CHECK(static_cast<std::size_t>(mirror->Screen().Cols()) *
              mirror->Screen().Rows() <=
          grid::MAX_SCREEN_CELLS);
}

TEST_CASE("after a snapshot the mirror equals the agent's grid") {
    AgentGrid agent{40, 10};
    const std::string stream = Stream();
    agent.Feed(stream);
    auto snap = grid::Snapshot::Create(*agent.Grid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);

    // A mirror that has seen something else entirely, and sits mid-sequence.
    auto mirror = NewMirror(40, 10);
    mirror->Feed(
        Bytes("junk\x1b[31m\x1b[5;5Hjunk\x1b[?1049h\x1b"
              "7\x1b["));
    REQUIRE(mirror->ResetFromSnapshot(owned->Bytes()));

    CHECK(mirror->Parser().State() == grid::EParserState::Ground);
    CHECK(EncodeScreen(mirror->Screen()) == EncodeScreen(*agent.Grid));
    CHECK(mirror->Screen().ScrollbackSize() == 0);
}

TEST_CASE("a snapshot leaves nothing of what the mirror held before") {
    AgentGrid agent{40, 10};
    agent.Feed("only this\r\n");
    auto snap = grid::Snapshot::Create(*agent.Grid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);

    // The reference: the snapshot fed to a grid that has never seen anything.
    AgentGrid fresh{40, 10, 0};
    fresh.Parser.Feed(owned->Bytes().data(), owned->Bytes().size(),
                      *fresh.Grid);

    // Content on the alternate screen, saved cursors on both screens, custom
    // tab stops, a cursor style, a mouse mode, a charset: the snapshot's
    // soft reset and repaint do not necessarily unset all of it.
    auto mirror = NewMirror(40, 10);
    mirror->Feed(
        Bytes("\x1b[3;4r\x1b[?6h\x1b[2;2H\x1b"
              "7\x1b[4;4Hmain"
              "\x1b[?1049h\x1b[5;5Halt content\x1b[3;3H\x1b"
              "7"
              "\x1b[?1049l\x1b[3g\x1b[1;6H\x1bH\x1b[5 q\x1b[?1000h"
              "\x1b[?7l\x1b[4h\x1b(0\x1b[?1004h"));
    REQUIRE(mirror->ResetFromSnapshot(owned->Bytes()));
    CHECK(Encode(mirror->Screen()) == Encode(*fresh.Grid));
}

TEST_CASE("a snapshot keeps the size the mirror had") {
    AgentGrid agent{30, 7};
    agent.Feed("abc");
    auto snap = grid::Snapshot::Create(*agent.Grid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    auto mirror = NewMirror(30, 7);
    REQUIRE(mirror->ResetFromSnapshot(owned->Bytes()));
    CHECK(mirror->Screen().Cols() == 30);
    CHECK(mirror->Screen().Rows() == 7);
    CHECK(mirror->Screen().ScrollbackLimit() == 0);
}

TEST_CASE("bytes after a snapshot land in the new grid") {
    AgentGrid agent{40, 10};
    agent.Feed("before\r\n");
    auto snap = grid::Snapshot::Create(*agent.Grid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    auto mirror = NewMirror(40, 10);
    REQUIRE(mirror->ResetFromSnapshot(owned->Bytes()));
    agent.Feed("after\x1b[1;31m!\r\nmore");
    mirror->Feed(Bytes("after\x1b[1;31m!\r\nmore"));
    CHECK(EncodeScreen(mirror->Screen()) == EncodeScreen(*agent.Grid));
}

TEST_CASE("the mirror follows a resize as the agent's grid does") {
    for (const auto [cols, rows] :
         {std::pair<std::uint16_t, std::uint16_t>{20, 10},
          {60, 10},
          {40, 4},
          {40, 20},
          {20, 4},
          {80, 30}}) {
        CAPTURE(cols);
        CAPTURE(rows);
        AgentGrid agent{40, 10, 0};
        auto mirror = NewMirror(40, 10);
        const std::string before = Stream();
        agent.Feed(before);
        mirror->Feed(Bytes(before));

        REQUIRE(agent.Grid->Resize(cols, rows).has_value());
        REQUIRE(mirror->Resize(cols, rows));
        CHECK(mirror->Screen().Cols() == cols);
        CHECK(mirror->Screen().Rows() == rows);
        CHECK(Encode(mirror->Screen()) == Encode(*agent.Grid));

        const std::string after =
            "\x1b[2;3Hafter the resize, long enough to "
            "wrap on the narrow screens\r\nnext\r\n";
        agent.Feed(after);
        mirror->Feed(Bytes(after));
        CHECK(Encode(mirror->Screen()) == Encode(*agent.Grid));
    }
}

TEST_CASE("a snapshot after a resize keeps the new size") {
    AgentGrid agent{40, 10};
    auto mirror = NewMirror(40, 10);
    REQUIRE(mirror->Resize(25, 6));
    REQUIRE(agent.Grid->Resize(25, 6).has_value());
    agent.Feed("abc");
    auto snap = grid::Snapshot::Create(*agent.Grid);
    REQUIRE(snap.has_value());
    const std::unique_ptr<grid::Snapshot> owned(*snap);
    REQUIRE(mirror->ResetFromSnapshot(owned->Bytes()));
    CHECK(mirror->Screen().Cols() == 25);
    CHECK(mirror->Screen().Rows() == 6);
    CHECK(EncodeScreen(mirror->Screen()) == EncodeScreen(*agent.Grid));
}

TEST_CASE(
    "a snapshot starts from a parser in Ground, whatever the mirror was in") {
    // Real snapshots open with CAN, which would also abort this; the mirror
    // does not rely on that.
    auto mirror = NewMirror(40, 10);
    mirror->Feed(Bytes("\x1bP1;2|swallowed by a device control string"));
    REQUIRE(mirror->Parser().State() != grid::EParserState::Ground);
    REQUIRE(mirror->ResetFromSnapshot(Bytes("visible")));
    CHECK(mirror->Parser().State() == grid::EParserState::Ground);

    AgentGrid expected{40, 10, 0};
    expected.Feed("visible");
    CHECK(Encode(mirror->Screen()) == Encode(*expected.Grid));
}
