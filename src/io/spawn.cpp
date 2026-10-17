#include "zet/io/spawn.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>

namespace zet::io {

namespace {

/// What the child exits with when it could not get as far as execve. The
/// parent never reports it: the errno through the pipe says more.
constexpr int SPAWN_FAILED_EXIT = 127;

[[nodiscard]] bool IsNullTerminated(
    std::span<const char* const> list) noexcept {
    return !list.empty() && list.back() == nullptr;
}

/// Everything the child does between fork and execve. Only async-signal-safe
/// calls, no allocation, no return: the parent's heap and locks are a copy of
/// a moment that may have been mid-update.
[[noreturn]] void RunChild(const SpawnSpec& spec, int slave, int errorPipe,
                           const sigset_t& emptyMask, int lastSignal) noexcept {
    const auto fail = [errorPipe]() {
        const int code = errno;
        static_cast<void>(::write(errorPipe, &code, sizeof(code)));
        ::_exit(SPAWN_FAILED_EXIT);
    };

    // A session of its own, so that the pty can become its controlling
    // terminal and Ctrl-C reaches its process group and not ours.
    if (::setsid() < 0) {
        fail();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::ioctl(slave, TIOCSCTTY, 0) != 0) {
        fail();
    }
    for (const int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(slave, target) < 0) {
            fail();
        }
        // dup2 onto itself changes nothing, close-on-exec included: a slave
        // that came out as 0, 1 or 2 would be closed by execve.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (::fcntl(target, F_SETFD, 0) != 0) {
            fail();
        }
    }
    if (slave > STDERR_FILENO) {
        static_cast<void>(::close(slave));
    }

    // Handlers do not survive execve, but ignored signals and the mask do.
    struct sigaction defaults{};
    defaults.sa_handler = SIG_DFL;
    for (int number = 1; number <= lastSignal; ++number) {
        // SIGKILL, SIGSTOP and the numbers the C library reserves refuse;
        // there is nothing to reset on those.
        static_cast<void>(::sigaction(number, &defaults, nullptr));
    }
    if (::sigprocmask(SIG_SETMASK, &emptyMask, nullptr) != 0) {
        fail();
    }

    if (spec.WorkingDirectory != nullptr &&
        ::chdir(spec.WorkingDirectory) != 0) {
        fail();
    }
    // execve takes char* const[] for historical reasons; it writes to none of
    // them.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ::execve(spec.Path, const_cast<char* const*>(spec.Argv.data()),
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
             const_cast<char* const*>(spec.Envp.data()));
    fail();
    ::_exit(SPAWN_FAILED_EXIT);
}

}  // namespace

IoResult<Child> Spawn(const SpawnSpec& spec) noexcept {
    if (spec.Path == nullptr || !IsNullTerminated(spec.Argv) ||
        !IsNullTerminated(spec.Envp)) {
        return std::unexpected(Errno{.Code = EINVAL, .Call = "Spawn"});
    }

    auto pty = OpenPty();
    if (!pty) {
        return std::unexpected(pty.error());
    }
    if (auto resized = Resize(pty->Master.Get(), spec.Size); !resized) {
        return std::unexpected(resized.error());
    }

    // Blocking, unlike MakePipe's: the parent waits on it for the one answer
    // the child gives, which is either an errno or the close that execve does.
    std::array<int, 2> errorFds{-1, -1};
    if (::pipe2(errorFds.data(), O_CLOEXEC) != 0) {
        return std::unexpected(LastErrno("pipe2"));
    }
    UniqueFd errorRead{errorFds[0]};
    UniqueFd errorWrite{errorFds[1]};

    // Computed before fork: SIGRTMAX is a function call on some C libraries,
    // and not one the child may make.
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    const int lastSignal = SIGRTMAX;

    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(LastErrno("fork"));
    }
    if (pid == 0) {
        RunChild(spec, pty->Slave.Get(), errorWrite.Get(), emptyMask,
                 lastSignal);
    }

    errorWrite.Reset();
    pty->Slave.Reset();

    int childErrno = 0;
    ssize_t got = 0;
    do {
        got = ::read(errorRead.Get(), &childErrno, sizeof(childErrno));
    } while (got < 0 && errno == EINTR);

    if (got == 0) {
        return Child{.Pid = pid, .Master = std::move(pty->Master)};
    }
    // The child is exiting or has: wait for it here, so that a failed spawn
    // leaves no zombie behind.
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (got < 0) {
        return std::unexpected(LastErrno("read"));
    }
    return std::unexpected(Errno{.Code = childErrno, .Call = "execve"});
}

IoResult<std::optional<ExitStatus>> TryReap(pid_t pid) noexcept {
    int status = 0;
    pid_t reaped = 0;
    do {
        reaped = ::waitpid(pid, &status, WNOHANG);
    } while (reaped < 0 && errno == EINTR);
    if (reaped < 0) {
        return std::unexpected(LastErrno("waitpid"));
    }
    if (reaped == 0) {
        return std::nullopt;
    }
    if (WIFSIGNALED(status)) {
        return ExitStatus{.Kind = ExitStatus::EKind::Signaled,
                          .Value = WTERMSIG(status)};
    }
    return ExitStatus{.Kind = ExitStatus::EKind::Exited,
                      .Value = WEXITSTATUS(status)};
}

}  // namespace zet::io
