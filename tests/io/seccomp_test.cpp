#include "zet/io/seccomp.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>

#include "zet/io/fd.hpp"

namespace {

using namespace zet;
using namespace zet::io;

/// Exit codes of the child, one per thing that can go wrong in it.
enum EChildExit : int {
    ChildOk = 0,
    InstallFailed = 10,
    OpenAllowed,
    OpenWrongErrno,
    MkdirAllowed,
    UnlinkAllowed,
    ChmodAllowed,
    PipeFailed,
    ReadWriteFailed,
    SendFailed,
    EpollFailed,
    ExecReturned,
    IoUringAllowed,
};

/// Forks, installs the filter in the child and runs `body` there. The child
/// is the only process the filter ever touches.
template <typename TBody>
int RunFiltered(TBody&& body) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // A process the filter kills dumps core like any other; this one is
        // meant to die, and leaves no file behind for it.
        const rlimit noCore{.rlim_cur = 0, .rlim_max = 0};
        static_cast<void>(::setrlimit(RLIMIT_CORE, &noCore));
        if (!InstallSeccomp(ESeccompProfile::NoFileOperations)) {
            ::_exit(InstallFailed);
        }
        ::_exit(body());
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    return status;
}

/// Room for struct io_uring_params, which the kernel reads and writes; its
/// layout does not matter when the call is refused before it is looked at.
constexpr std::size_t IO_URING_PARAMS_ROOM = 256;

}  // namespace

TEST_CASE("after seccomp the file system is refused and the loop still runs") {
    const int status = RunFiltered([]() -> int {
        // Through openat as libc does it, and with the errno documented.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            return OpenAllowed;
        }
        if (errno != EPERM) {
            return OpenWrongErrno;
        }
        if (::mkdir("/tmp/zet-seccomp-should-not-exist", 0700) == 0 ||
            errno != EPERM) {
            return MkdirAllowed;
        }
        if (::unlink("/tmp/zet-seccomp-should-not-exist") == 0 ||
            errno != EPERM) {
            return UnlinkAllowed;
        }
        if (::chmod("/tmp", 0777) == 0 || errno != EPERM) {
            return ChmodAllowed;
        }
#ifdef __NR_io_uring_setup
        // A ring would carry file operations past every number above.
        std::array<std::byte, IO_URING_PARAMS_ROOM> params{};
        if (::syscall(__NR_io_uring_setup, 1, params.data()) >= 0 ||
            errno != EPERM) {
            return IoUringAllowed;
        }
#endif

        // What the mux's loop does keeps working.
        std::array<int, 2> pipe{};
        if (::pipe(pipe.data()) != 0) {
            return PipeFailed;
        }
        char byte = 'z';
        if (::write(pipe[1], &byte, 1) != 1 || ::read(pipe[0], &byte, 1) != 1) {
            return ReadWriteFailed;
        }
        std::array<int, 2> pair{};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0 ||
            ::send(pair[0], &byte, 1, 0) != 1) {
            return SendFailed;
        }
        const int epoll = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event event{.events = EPOLLIN, .data = {.fd = pair[1]}};
        if (epoll < 0 ||
            ::epoll_ctl(epoll, EPOLL_CTL_ADD, pair[1], &event) != 0 ||
            ::epoll_wait(epoll, &event, 1, 0) != 1) {
            return EpollFailed;
        }
        return ChildOk;
    });
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == ChildOk);
}

TEST_CASE("after seccomp an exec kills the process") {
    const int status = RunFiltered([]() -> int {
        const std::array<const char*, 2> argv{"/bin/true", nullptr};
        const std::array<const char*, 1> envp{nullptr};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        ::execve(argv[0], const_cast<char* const*>(argv.data()),
                 // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                 const_cast<char* const*>(envp.data()));
        return ExecReturned;
    });
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSYS);
}
