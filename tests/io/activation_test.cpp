#include "zet/io/activation.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "zet/io/fd.hpp"

namespace {

using namespace zet;
using namespace zet::io;

constexpr int CHILD_OK = 0;
constexpr int CHILD_FAILED = 1;

/// Runs `body` in a forked child, so that what it does to descriptors 3 and
/// on and to the environment stays there. The child's exit status is the
/// verdict.
template <typename TBody>
void InChild(TBody&& body) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::_exit(body() ? CHILD_OK : CHILD_FAILED);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == CHILD_OK);
}

bool Gone(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    return std::getenv(name) == nullptr;
}

/// Puts a pipe's read end at exactly `slot`, as systemd lines them up: no
/// close-on-exec, owned by nobody until TakeActivatedSockets.
bool PlaceAt(int slot) {
    auto pipe = MakeBlockingPipe();
    if (!pipe) {
        return false;
    }
    // A forked test child has the low descriptors free, so the pipe may well
    // have come out at the slot itself; dup2 onto it would do nothing, and
    // the pipe's own destructor would then close it.
    if (pipe->Read.Get() == slot) {
        static_cast<void>(pipe->Read.Release());
    } else if (::dup2(pipe->Read.Get(), slot) != slot) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::fcntl(slot, F_SETFD, 0) == 0;
}

}  // namespace

TEST_CASE("sockets passed to this process are taken, ready, and forgotten") {
    InChild([] {
        if (!PlaceAt(FIRST_ACTIVATED_FD) || !PlaceAt(FIRST_ACTIVATED_FD + 1)) {
            return false;
        }
        const std::string pid = std::to_string(::getpid());
        // NOLINTBEGIN(concurrency-mt-unsafe)
        ::setenv("LISTEN_PID", pid.c_str(), 1);
        ::setenv("LISTEN_FDS", "2", 1);
        ::setenv("LISTEN_FDNAMES", "zet:zet", 1);
        // NOLINTEND(concurrency-mt-unsafe)
        auto sockets = TakeActivatedSockets();
        if (!sockets || sockets->Count != 2) {
            return false;
        }
        for (const UniqueFd& fd : sockets->View()) {
            // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
            if ((::fcntl(fd.Get(), F_GETFD) & FD_CLOEXEC) == 0 ||
                (::fcntl(fd.Get(), F_GETFL) & O_NONBLOCK) == 0) {
                return false;
            }
            // NOLINTEND(cppcoreguidelines-pro-type-vararg)
        }
        return sockets->View()[0].Get() == FIRST_ACTIVATED_FD &&
               Gone("LISTEN_PID") && Gone("LISTEN_FDS") &&
               Gone("LISTEN_FDNAMES");
    });
}

TEST_CASE("an offer meant for another process is not taken") {
    InChild([] {
        if (!PlaceAt(FIRST_ACTIVATED_FD)) {
            return false;
        }
        const std::string parent = std::to_string(::getppid());
        // NOLINTBEGIN(concurrency-mt-unsafe)
        ::setenv("LISTEN_PID", parent.c_str(), 1);
        ::setenv("LISTEN_FDS", "1", 1);
        // NOLINTEND(concurrency-mt-unsafe)
        auto sockets = TakeActivatedSockets();
        // Not taken: descriptor 3 still has no close-on-exec, nobody owns it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const bool untouched =
            (::fcntl(FIRST_ACTIVATED_FD, F_GETFD) & FD_CLOEXEC) == 0;
        return sockets && sockets->Count == 0 && untouched &&
               Gone("LISTEN_PID") && Gone("LISTEN_FDS");
    });
}

TEST_CASE("no offer is no sockets, and a bad count is an error") {
    InChild([] {
        // NOLINTBEGIN(concurrency-mt-unsafe)
        ::unsetenv("LISTEN_PID");
        ::unsetenv("LISTEN_FDS");
        // NOLINTEND(concurrency-mt-unsafe)
        auto none = TakeActivatedSockets();
        if (!none || none->Count != 0) {
            return false;
        }
        const std::string pid = std::to_string(::getpid());
        for (const char* count : {"two", "", "3x", "9"}) {
            // NOLINTBEGIN(concurrency-mt-unsafe)
            ::setenv("LISTEN_PID", pid.c_str(), 1);
            ::setenv("LISTEN_FDS", count, 1);
            // NOLINTEND(concurrency-mt-unsafe)
            auto bad = TakeActivatedSockets();
            if (bad || bad.error().Code != EINVAL || !Gone("LISTEN_FDS")) {
                return false;
            }
        }
        return true;
    });
}
