#include "zet/io/daemon.hpp"

#include <doctest/doctest.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>

#include "zet/io/fd.hpp"

namespace {

using namespace zet;
using namespace zet::io;

/// How long the test waits for the detached child's report.
constexpr std::chrono::milliseconds REPORT_TIMEOUT{10'000};

/// What the detached child found out about itself.
struct Report {
    bool LeadsSession{false};
    std::array<bool, 3> OnDevNull{};
    bool Valid{false};
};

bool IsDevNull(int fd, const struct stat& devNull) {
    struct stat st{};
    return ::fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
           st.st_rdev == devNull.st_rdev;
}

}  // namespace

TEST_CASE("Detach leaves a child in its own session with stdio on /dev/null") {
    // The detached child is the test's grandchild; as subreaper the test gets
    // it back to reap instead of leaving it to init.
    REQUIRE(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    auto pipe = MakePipe();
    REQUIRE(pipe.has_value());
    struct stat devNull{};
    REQUIRE(::stat("/dev/null", &devNull) == 0);

    bool closeStdin = false;
    SUBCASE("with stdio open") {}
    SUBCASE("with stdin closed, so /dev/null opens as descriptor 0") {
        closeStdin = true;
    }

    const pid_t launcher = ::fork();
    REQUIRE(launcher >= 0);
    if (launcher == 0) {
        // Stands for the process ssh started.
        if (closeStdin) {
            ::close(STDIN_FILENO);
        }
        const auto role = Detach();
        if (!role) {
            ::_exit(EXIT_FAILURE);
        }
        if (*role == EDetached::Parent) {
            ::_exit(EXIT_SUCCESS);
        }
        Report report{.LeadsSession = ::getsid(0) == ::getpid(),
                      .OnDevNull = {},
                      .Valid = true};
        for (int fd = 0; fd < 3; ++fd) {
            report.OnDevNull[static_cast<std::size_t>(fd)] =
                IsDevNull(fd, devNull);
        }
        const auto written =
            ::write(pipe->Write.Get(), &report, sizeof(report));
        ::_exit(written == sizeof(report) ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status = 0;
    REQUIRE(::waitpid(launcher, &status, 0) == launcher);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == EXIT_SUCCESS);

    pipe->Write.Reset();
    pollfd waiting{.fd = pipe->Read.Get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&waiting, 1, static_cast<int>(REPORT_TIMEOUT.count())) == 1);
    Report report{};
    REQUIRE(::read(pipe->Read.Get(), &report, sizeof(report)) ==
            sizeof(report));
    CHECK(report.Valid);
    CHECK(report.LeadsSession);
    CHECK(report.OnDevNull[0]);
    CHECK(report.OnDevNull[1]);
    CHECK(report.OnDevNull[2]);

    // The grandchild, reparented to the test, exits on its own.
    int grandchildStatus = 0;
    const pid_t reaped = ::waitpid(-1, &grandchildStatus, 0);
    CHECK(reaped > 0);
    CHECK(WIFEXITED(grandchildStatus));
    CHECK(WEXITSTATUS(grandchildStatus) == EXIT_SUCCESS);
    REQUIRE(::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);
}
