#include "zet/io/fd.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <utility>

namespace {

using namespace zet;
using namespace zet::io;

/// Whether `fd` still names an open descriptor.
bool IsOpenDescriptor(int fd) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::fcntl(fd, F_GETFD) != -1;
}

}  // namespace

TEST_CASE("a UniqueFd closes what it owns, once") {
    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    const int raw = pipe->Read.Get();
    CHECK(IsOpenDescriptor(raw));

    UniqueFd moved{std::move(pipe->Read)};
    CHECK_FALSE(pipe->Read.IsOpen());
    CHECK(moved.Get() == raw);

    UniqueFd assigned;
    assigned = std::move(moved);
    CHECK_FALSE(moved);
    CHECK(assigned);

    assigned.Reset();
    CHECK_FALSE(IsOpenDescriptor(raw));
    CHECK(assigned.Get() == -1);
}

TEST_CASE("Release hands the descriptor over without closing it") {
    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    const int raw = pipe->Write.Release();
    CHECK_FALSE(pipe->Write.IsOpen());
    CHECK(IsOpenDescriptor(raw));
    UniqueFd again{raw};
}

TEST_CASE("pipes and socket pairs come non-blocking and close-on-exec") {
    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    for (const int fd : {pipe->Read.Get(), pipe->Write.Get(), pair->First.Get(),
                         pair->Second.Get()}) {
        CAPTURE(fd);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        CHECK((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    }
}

TEST_CASE("an empty non-blocking read says it would block") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    std::array<std::byte, 16> buffer{};
    const auto n = ReadSome(pair->First.Get(), MutableByteSpan{buffer});
    REQUIRE_FALSE(n.has_value());
    CHECK(n.error().WouldBlock());
    CHECK(n.error().Call == "read");
}

TEST_CASE("bytes written to one end are read from the other") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    const std::array<std::byte, 3> sent{std::byte{1}, std::byte{2},
                                        std::byte{3}};
    REQUIRE(WriteSome(pair->First.Get(), ByteSpan{sent}) == sent.size());

    std::array<std::byte, 16> got{};
    const auto n = ReadSome(pair->Second.Get(), MutableByteSpan{got});
    REQUIRE(n == sent.size());
    CHECK(std::equal(sent.begin(), sent.end(), got.begin()));

    pair->First.Reset();
    CHECK(ReadSome(pair->Second.Get(), MutableByteSpan{got}) == 0u);
}

TEST_CASE("a full socket says it would block instead of blocking") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    std::array<std::byte, 4096> chunk{};
    for (;;) {
        const auto n = WriteSome(pair->First.Get(), ByteSpan{chunk});
        if (!n) {
            CHECK(n.error().WouldBlock());
            CHECK(n.error().Call == "write");
            break;
        }
    }
}

TEST_CASE("a blocking descriptor can be made non-blocking") {
    std::array<int, 2> fds{};
    REQUIRE(::pipe(fds.data()) == 0);
    UniqueFd read{fds[0]};
    UniqueFd write{fds[1]};
    REQUIRE(SetNonBlocking(read.Get()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    CHECK((::fcntl(read.Get(), F_GETFL) & O_NONBLOCK) != 0);

    const auto closed = SetNonBlocking(-1);
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().Code == EBADF);
    CHECK(closed.error().Call == "fcntl");
}

TEST_CASE("an Errno describes itself") {
    const Errno error{.Code = EBADF, .Call = "close"};
    CHECK_FALSE(error.Description().empty());
    CHECK_FALSE(error.WouldBlock());
    CHECK(Errno{.Code = EAGAIN, .Call = "read"}.WouldBlock());
    CHECK(error == Errno{.Code = EBADF, .Call = "close"});
    CHECK_FALSE(error == Errno{.Code = EBADF, .Call = "read"});
}
