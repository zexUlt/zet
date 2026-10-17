#include "zet/io/terminal.hpp"

#include <doctest/doctest.h>
#include <termios.h>

#include <utility>

#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"

namespace {

using namespace zet::io;

termios ModeOf(int fd) {
    termios mode{};
    REQUIRE(::tcgetattr(fd, &mode) == 0);
    return mode;
}

}  // namespace

TEST_CASE("a pty is a terminal and a pipe is not") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    CHECK(IsTerminal(pty->Slave.Get()));
    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    CHECK_FALSE(IsTerminal(pipe->Read.Get()));
}

TEST_CASE(
    "raw mode turns off what a remote terminal does itself, then comes back") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    const int slave = pty->Slave.Get();
    const termios before = ModeOf(slave);
    REQUIRE((before.c_lflag & ICANON) != 0);
    REQUIRE((before.c_lflag & ECHO) != 0);

    {
        auto raw = RawMode::Enter(slave);
        REQUIRE(raw.has_value());
        const termios during = ModeOf(slave);
        CHECK((during.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) == 0);
        CHECK((during.c_iflag & (IXON | IXOFF | ICRNL)) == 0);
        CHECK((during.c_oflag & OPOST) == 0);
        CHECK(during.c_cc[VMIN] == 1);
        CHECK(during.c_cc[VTIME] == 0);

        // A moved-from mode restores nothing; the one it moved into does.
        RawMode moved{std::move(*raw)};
        REQUIRE(raw->Restore());
        CHECK((ModeOf(slave).c_lflag & ICANON) == 0);
    }

    const termios after = ModeOf(slave);
    CHECK(after.c_lflag == before.c_lflag);
    CHECK(after.c_iflag == before.c_iflag);
    CHECK(after.c_oflag == before.c_oflag);
}

TEST_CASE("Restore runs once and raw mode needs a terminal") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    auto raw = RawMode::Enter(pty->Slave.Get());
    REQUIRE(raw.has_value());
    REQUIRE(raw->Restore());
    REQUIRE(raw->Restore());

    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    const auto refused = RawMode::Enter(pipe->Read.Get());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Call == "tcgetattr");
}
