#include "zet/io/spawn.hpp"

#include <doctest/doctest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <string_view>

#include "pty_test_support.hpp"

namespace {

using namespace zet;
using namespace zet::io;
using zet::io::test::ReadUntil;

constexpr WindowSize DEFAULT_SIZE{.Rows = 24, .Cols = 80};

constexpr std::array<const char*, 3> ENVIRONMENT{"PATH=/bin:/usr/bin",
                                                 "TERM=dumb", nullptr};

/// Runs `script` under /bin/sh on a fresh pty.
IoResult<Child> SpawnShell(const char* script, WindowSize size = DEFAULT_SIZE) {
    const std::array<const char*, 4> argv{"/bin/sh", "-c", script, nullptr};
    return Spawn(SpawnSpec{
        .Path = "/bin/sh", .Argv = argv, .Envp = ENVIRONMENT, .Size = size});
}

/// Writes all of `text` to the master; the tests write a few bytes at a time,
/// far below what a pty buffers.
void Type(const Child& child, std::string_view text) {
    const ByteSpan bytes{reinterpret_cast<const std::byte*>(text.data()),
                         text.size()};
    REQUIRE(WriteSome(child.Master.Get(), bytes) == text.size());
}

/// Reads to the end of the stream and reaps the child. A child whose output
/// has ended has closed its terminal, so the wait that follows is short.
ExitStatus Finish(Child& child) {
    const auto rest = ReadUntil(child.Master.Get(), "");
    REQUIRE_FALSE(rest.TimedOut);
    int status = 0;
    REQUIRE(::waitpid(child.Pid, &status, 0) == child.Pid);
    if (WIFSIGNALED(status)) {
        return ExitStatus{.Kind = ExitStatus::EKind::Signaled,
                          .Value = WTERMSIG(status)};
    }
    return ExitStatus{.Kind = ExitStatus::EKind::Exited,
                      .Value = WEXITSTATUS(status)};
}

/// Blocks a signal and ignores it for the life of a test, then restores both.
class HostileSignal {
public:
    explicit HostileSignal(int number) : Number_(number) {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, Number_);
        REQUIRE(::sigprocmask(SIG_BLOCK, &blocked, &OldMask_) == 0);
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        REQUIRE(::sigaction(Number_, &ignore, &OldAction_) == 0);
    }
    ~HostileSignal() {
        ::sigaction(Number_, &OldAction_, nullptr);
        ::sigprocmask(SIG_SETMASK, &OldMask_, nullptr);
    }
    HostileSignal(const HostileSignal&) = delete;
    HostileSignal& operator=(const HostileSignal&) = delete;
    HostileSignal(HostileSignal&&) = delete;
    HostileSignal& operator=(HostileSignal&&) = delete;

private:
    int Number_;
    sigset_t OldMask_{};
    struct sigaction OldAction_{};
};

}  // namespace

TEST_CASE("a child reads what the master writes and answers through it") {
    auto child = SpawnShell("read line; echo \"got:$line\"");
    REQUIRE(child.has_value());
    Type(*child, "hello\n");
    const auto out = ReadUntil(child->Master.Get(), "got:hello");
    CHECK(out.Found);
    CHECK(Finish(*child) == ExitStatus{.Kind = ExitStatus::EKind::Exited});
}

TEST_CASE("the child starts at the size it was given") {
    auto child = SpawnShell("stty size", WindowSize{.Rows = 31, .Cols = 97});
    REQUIRE(child.has_value());
    CHECK(ReadUntil(child->Master.Get(), "31 97").Found);
    CHECK(Finish(*child).Value == 0);
}

TEST_CASE("a resize reaches the child as SIGWINCH and a new size") {
    // The loop keeps the shell between commands, which is where it runs
    // traps; a builtin read would hold the signal until input arrived.
    auto child = SpawnShell(
        "trap 'stty size; exit 0' WINCH; echo ready; while :; do :; done");
    REQUIRE(child.has_value());
    REQUIRE(ReadUntil(child->Master.Get(), "ready").Found);
    REQUIRE(Resize(child->Master.Get(), WindowSize{.Rows = 40, .Cols = 120}));
    CHECK(ReadUntil(child->Master.Get(), "40 120").Found);
    CHECK(Finish(*child).Value == 0);
}

TEST_CASE("the pty is the child's controlling terminal") {
    // /dev/tty opens only for a process that has one.
    auto child = SpawnShell("exec 3</dev/tty && echo has-ctty");
    REQUIRE(child.has_value());
    CHECK(ReadUntil(child->Master.Get(), "has-ctty").Found);
    CHECK(Finish(*child).Value == 0);
}

TEST_CASE("Ctrl-C ends the child even if the parent blocks SIGINT") {
    // The agent blocks what it reads through signalfd; a shell that inherited
    // that would never see Ctrl-C.
    const HostileSignal hostile{SIGINT};
    auto child = SpawnShell("echo ready; exec cat");
    REQUIRE(child.has_value());
    REQUIRE(ReadUntil(child->Master.Get(), "ready").Found);
    Type(*child, "\x03");
    CHECK(Finish(*child) ==
          ExitStatus{.Kind = ExitStatus::EKind::Signaled, .Value = SIGINT});
}

TEST_CASE("a child's last output is read after it has exited") {
    auto child = SpawnShell("echo last-words");
    REQUIRE(child.has_value());
    int status = 0;
    REQUIRE(::waitpid(child->Pid, &status, 0) == child->Pid);
    const auto out = ReadUntil(child->Master.Get(), "");
    CHECK(out.Ended);
    CHECK(out.Output.find("last-words") != std::string::npos);
}

TEST_CASE("TryReap waits for nothing and reports how the child ended") {
    auto child = SpawnShell("echo ready; exec cat");
    REQUIRE(child.has_value());
    REQUIRE(ReadUntil(child->Master.Get(), "ready").Found);
    const auto running = TryReap(child->Pid);
    REQUIRE(running.has_value());
    CHECK_FALSE(running->has_value());

    REQUIRE(::kill(child->Pid, SIGTERM) == 0);
    const auto rest = ReadUntil(child->Master.Get(), "");
    REQUIRE(rest.Ended);
    // The terminal closing and the exit being reapable are two events; the
    // first does not guarantee the second, so ask until it is.
    std::optional<ExitStatus> status;
    while (!status) {
        auto reaped = TryReap(child->Pid);
        REQUIRE(reaped.has_value());
        status = *reaped;
    }
    CHECK(*status ==
          ExitStatus{.Kind = ExitStatus::EKind::Signaled, .Value = SIGTERM});

    const auto again = TryReap(child->Pid);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().Code == ECHILD);
}

TEST_CASE("an exit code comes back as it was given") {
    auto child = SpawnShell("exit 7");
    REQUIRE(child.has_value());
    CHECK(Finish(*child) ==
          ExitStatus{.Kind = ExitStatus::EKind::Exited, .Value = 7});
}

TEST_CASE("a failed execve is an error from Spawn, not a child to reap") {
    const std::array<const char*, 2> argv{"/nonexistent/zet-shell", nullptr};
    const auto child = Spawn(SpawnSpec{.Path = "/nonexistent/zet-shell",
                                       .Argv = argv,
                                       .Envp = ENVIRONMENT,
                                       .Size = DEFAULT_SIZE});
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error() == Errno{.Code = ENOENT, .Call = "execve"});
    // Already reaped: no child of ours is left, zombie or otherwise.
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}

TEST_CASE("a working directory that does not exist fails the spawn") {
    const std::array<const char*, 2> argv{"/bin/sh", nullptr};
    const auto child = Spawn(SpawnSpec{.Path = "/bin/sh",
                                       .Argv = argv,
                                       .Envp = ENVIRONMENT,
                                       .Size = DEFAULT_SIZE,
                                       .WorkingDirectory = "/nonexistent"});
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().Code == ENOENT);
}

TEST_CASE("the working directory is the one asked for") {
    const std::array<const char*, 4> argv{"/bin/sh", "-c", "pwd", nullptr};
    auto child = Spawn(SpawnSpec{.Path = "/bin/sh",
                                 .Argv = argv,
                                 .Envp = ENVIRONMENT,
                                 .Size = DEFAULT_SIZE,
                                 .WorkingDirectory = "/"});
    REQUIRE(child.has_value());
    CHECK(ReadUntil(child->Master.Get(), "/\r\n").Found);
    CHECK(Finish(*child).Value == 0);
}

TEST_CASE("a spec without its terminating nullptr is refused") {
    const std::array<const char*, 1> argv{"/bin/sh"};
    const auto child = Spawn(SpawnSpec{.Path = "/bin/sh",
                                       .Argv = argv,
                                       .Envp = ENVIRONMENT,
                                       .Size = DEFAULT_SIZE});
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error() == Errno{.Code = EINVAL, .Call = "Spawn"});
}
