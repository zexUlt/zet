#include "zet/io/pty.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>

#include <array>
#include <cstddef>

#include "pty_test_support.hpp"

namespace {

using namespace zet;
using namespace zet::io;

}  // namespace

TEST_CASE("a fresh pty has a non-blocking master and close-on-exec ends") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    CHECK((::fcntl(pty->Master.Get(), F_GETFL) & O_NONBLOCK) != 0);
    for (const int fd : {pty->Master.Get(), pty->Slave.Get()}) {
        CAPTURE(fd);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    }
}

TEST_CASE("an empty master says it would block") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    std::array<std::byte, 16> buffer{};
    const auto n = ReadPty(pty->Master.Get(), MutableByteSpan{buffer});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().WouldBlock());
}

TEST_CASE("a size set on the master is read back from the slave") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    const WindowSize size{.Rows = 33, .Cols = 101, .XPixels = 7, .YPixels = 9};
    REQUIRE(Resize(pty->Master.Get(), size));
    CHECK(GetWindowSize(pty->Slave.Get()) == size);
}

TEST_CASE("the master reports end of stream once the slave is gone") {
    auto pty = OpenPty();
    REQUIRE(pty.has_value());
    const std::array<std::byte, 4> sent{std::byte{'t'}, std::byte{'a'},
                                        std::byte{'i'}, std::byte{'l'}};
    REQUIRE(WriteSome(pty->Slave.Get(), ByteSpan{sent}) == sent.size());
    pty->Slave.Reset();

    // What was written before the close is still there to read, and after it
    // Linux says EIO, which reads as the end of the stream. The bytes cross
    // from slave to master asynchronously, hence the wait rather than one read.
    const auto out = test::ReadUntil(pty->Master.Get(), "");
    CHECK(out.Ended);
    CHECK(out.Output == "tail");
}

TEST_CASE("pty calls on a closed descriptor fail with the call named") {
    const auto resized = Resize(-1, WindowSize{});
    REQUIRE_FALSE(resized.has_value());
    CHECK(resized.error().Call == "ioctl(TIOCSWINSZ)");
    const auto size = GetWindowSize(-1);
    REQUIRE_FALSE(size.has_value());
    CHECK(size.error().Call == "ioctl(TIOCGWINSZ)");
    std::array<std::byte, 1> buffer{};
    const auto n = ReadPty(-1, MutableByteSpan{buffer});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().Call == "read");
}
