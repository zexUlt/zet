#include "zet/io/unix.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "zet/io/fd.hpp"

namespace {

using namespace zet;
using namespace zet::io;

/// A fresh directory under /tmp for one test, removed with what is in it.
class ScratchDirectory {
public:
    ScratchDirectory() {
        std::string pattern = "/tmp/zet-unix-XXXXXX";
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        Path_ = pattern;
    }
    ~ScratchDirectory() {
        static_cast<void>(::unlink((Path_ + "/socket").c_str()));
        static_cast<void>(::unlink((Path_ + "/file").c_str()));
        static_cast<void>(::unlink((Path_ + "/link").c_str()));
        static_cast<void>(::rmdir((Path_ + "/inner").c_str()));
        static_cast<void>(::rmdir(Path_.c_str()));
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    [[nodiscard]] std::string In(const char* name) const {
        return Path_ + "/" + name;
    }
    [[nodiscard]] const std::string& Path() const { return Path_; }

private:
    std::string Path_;
};

ByteSpan AsBytes(const char* text, std::size_t length) {
    return ByteSpan{reinterpret_cast<const std::byte*>(text), length};
}

constexpr mode_t SOCKET_MODE = 0666;
constexpr mode_t PRIVATE_MODE = 0600;
constexpr mode_t DIRECTORY_MODE = 0755;

}  // namespace

TEST_CASE("a unix listener is born with its mode, whatever the umask") {
    const ScratchDirectory scratch;
    const std::string path = scratch.In("socket");
    // A umask that would leave the socket unreachable to others, had the
    // mode been fixed after bind instead of around it.
    const mode_t previous = ::umask(077);
    auto listener = ListenUnix(path.c_str(), SOCKET_MODE);
    ::umask(previous);
    REQUIRE(listener.has_value());
    struct stat status{};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    CHECK(S_ISSOCK(status.st_mode));
    CHECK((status.st_mode & 0777) == SOCKET_MODE);
    // And the process's umask is what it was.
    const mode_t now = ::umask(previous);
    CHECK(now == previous);
}

TEST_CASE("a unix path that exists or does not fit is refused") {
    const ScratchDirectory scratch;
    const std::string path = scratch.In("socket");
    auto first = ListenUnix(path.c_str(), PRIVATE_MODE);
    REQUIRE(first.has_value());
    const auto second = ListenUnix(path.c_str(), PRIVATE_MODE);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().Code == EADDRINUSE);

    const std::string tooLong(sizeof(sockaddr_un{}.sun_path), 'x');
    const auto longListen = ListenUnix(tooLong.c_str(), PRIVATE_MODE);
    REQUIRE_FALSE(longListen.has_value());
    CHECK(longListen.error().Code == ENAMETOOLONG);
    const auto longConnect = ConnectUnix(tooLong.c_str());
    REQUIRE_FALSE(longConnect.has_value());
    CHECK(longConnect.error().Code == ENAMETOOLONG);
}

TEST_CASE("connect, accept and the peer's credentials") {
    const ScratchDirectory scratch;
    const std::string path = scratch.In("socket");
    auto listener = ListenUnix(path.c_str(), PRIVATE_MODE);
    REQUIRE(listener.has_value());

    const auto none = AcceptUnix(listener->Get());
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().WouldBlock());

    auto client = ConnectUnix(path.c_str());
    REQUIRE(client.has_value());
    auto server = AcceptUnix(listener->Get());
    REQUIRE(server.has_value());

    for (const int fd : {client->Get(), server->Get()}) {
        const auto credentials = PeerCredentialsOf(fd);
        REQUIRE(credentials.has_value());
        CHECK(credentials->Uid == ::getuid());
        CHECK(credentials->Gid == ::getgid());
        CHECK(credentials->Pid == ::getpid());
    }

    const auto refused = ConnectUnix(scratch.In("nobody").c_str());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().Code == ENOENT);
    CHECK(refused.error().Call == "connect");
}

TEST_CASE("a descriptor crosses with its bytes and arrives ready") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    auto pipe = MakeBlockingPipe();
    REQUIRE(pipe.has_value());

    constexpr std::string_view hello = "hello frame";
    const auto sent =
        SendWithFd(pair->First.Get(), AsBytes(hello.data(), hello.size()),
                   pipe->Read.Get());
    REQUIRE(sent == hello.size());

    std::array<std::byte, 64> buffer{};
    auto got = ReceiveWithFd(pair->Second.Get(), MutableByteSpan{buffer});
    REQUIRE(got.has_value());
    CHECK(got->Bytes == hello.size());
    REQUIRE(got->Fd.has_value());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    CHECK((::fcntl(got->Fd->Get(), F_GETFD) & FD_CLOEXEC) != 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    CHECK((::fcntl(got->Fd->Get(), F_GETFL) & O_NONBLOCK) != 0);

    // The descriptor that arrived is the pipe: what goes in one end comes
    // out of it.
    REQUIRE(WriteSome(pipe->Write.Get(), AsBytes("x", 1)) == 1U);
    std::array<std::byte, 1> one{};
    CHECK(ReadSome(got->Fd->Get(), MutableByteSpan{one}) == 1U);
}

TEST_CASE("bytes without a descriptor, an empty send and the end of file") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    REQUIRE(WriteSome(pair->First.Get(), AsBytes("plain", 5)) == 5U);
    std::array<std::byte, 16> buffer{};
    auto got = ReceiveWithFd(pair->Second.Get(), MutableByteSpan{buffer});
    REQUIRE(got.has_value());
    CHECK(got->Bytes == 5U);
    CHECK_FALSE(got->Fd.has_value());

    const auto nothing =
        ReceiveWithFd(pair->Second.Get(), MutableByteSpan{buffer});
    REQUIRE_FALSE(nothing.has_value());
    CHECK(nothing.error().WouldBlock());

    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    const auto empty =
        SendWithFd(pair->First.Get(), ByteSpan{}, pipe->Read.Get());
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().Code == EINVAL);

    pair->First.Reset();
    auto ended = ReceiveWithFd(pair->Second.Get(), MutableByteSpan{buffer});
    REQUIRE(ended.has_value());
    CHECK(ended->Bytes == 0U);
    CHECK_FALSE(ended->Fd.has_value());
}

TEST_CASE("more than one descriptor in a message is refused and closed") {
    auto pair = MakeSocketPair();
    REQUIRE(pair.has_value());
    auto first = MakePipe();
    auto second = MakePipe();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // Two descriptors in one SCM_RIGHTS, the way a hostile peer would send.
    std::array<int, 2> fds{first->Read.Get(), second->Read.Get()};
    char byte = 'x';
    iovec chunk{.iov_base = &byte, .iov_len = 1};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(fds))> control{};
    msghdr message{};
    message.msg_iov = &chunk;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fds));
    std::memcpy(CMSG_DATA(header), fds.data(), sizeof(fds));
    REQUIRE(::sendmsg(pair->First.Get(), &message, 0) == 1);

    // The write ends tell whether the received copies were closed: with every
    // read end gone, a write fails with EPIPE.
    first->Read.Reset();
    second->Read.Reset();
    // EPIPE, not the SIGPIPE that would end the test.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    struct sigaction previous{};
    REQUIRE(::sigaction(SIGPIPE, &ignore, &previous) == 0);
    std::array<std::byte, 8> buffer{};
    const auto got = ReceiveWithFd(pair->Second.Get(), MutableByteSpan{buffer});
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().Code == EBADMSG);
    for (const int writer : {first->Write.Get(), second->Write.Get()}) {
        const auto written = ::write(writer, "y", 1);
        CHECK(written == -1);
        CHECK(errno == EPIPE);
    }
    REQUIRE(::sigaction(SIGPIPE, &previous, nullptr) == 0);
}

TEST_CASE("CheckDirectory wants a directory, its owner and its exact mode") {
    const ScratchDirectory scratch;
    const std::string inner = scratch.In("inner");
    REQUIRE(::mkdir(inner.c_str(), DIRECTORY_MODE) == 0);
    REQUIRE(::chmod(inner.c_str(), DIRECTORY_MODE) == 0);
    CHECK(
        CheckDirectory(inner.c_str(), ::getuid(), DIRECTORY_MODE).has_value());

    const auto mode = CheckDirectory(inner.c_str(), ::getuid(), 0700);
    REQUIRE_FALSE(mode.has_value());
    CHECK(mode.error().Code == EACCES);

    const auto owner =
        CheckDirectory(inner.c_str(), ::getuid() + 1, DIRECTORY_MODE);
    REQUIRE_FALSE(owner.has_value());
    CHECK(owner.error().Code == EPERM);

    // A symlink to a good directory is not one: lstat sees the link.
    const std::string link = scratch.In("link");
    REQUIRE(::symlink(inner.c_str(), link.c_str()) == 0);
    const auto linked =
        CheckDirectory(link.c_str(), ::getuid(), DIRECTORY_MODE);
    REQUIRE_FALSE(linked.has_value());
    CHECK(linked.error().Code == ENOTDIR);

    const auto missing = CheckDirectory(scratch.In("nothing").c_str(),
                                        ::getuid(), DIRECTORY_MODE);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().Code == ENOENT);
    CHECK(missing.error().Call == "lstat");
}
