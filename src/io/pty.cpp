#include "zet/io/pty.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <utility>

namespace zet::io {

namespace {

/// Room for the slave's path. Linux names it /dev/pts/N, with N below the
/// kernel's pty limit of a few thousand; 64 leaves room for anything else.
constexpr std::size_t PTS_NAME_SIZE = 64;

}  // namespace

IoResult<Pty> OpenPty() noexcept {
    // posix_openpt rather than openpty: the latter lives in libutil on glibc,
    // a second library for one call.
    UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)};
    if (!master) {
        return std::unexpected(LastErrno("posix_openpt"));
    }
    if (::grantpt(master.Get()) != 0) {
        return std::unexpected(LastErrno("grantpt"));
    }
    if (::unlockpt(master.Get()) != 0) {
        return std::unexpected(LastErrno("unlockpt"));
    }
    std::array<char, PTS_NAME_SIZE> name{};
    // ptsname_r returns the error number rather than setting errno.
    if (const int error = ::ptsname_r(master.Get(), name.data(), name.size());
        error != 0) {
        return std::unexpected(Errno{.Code = error, .Call = "ptsname_r"});
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    UniqueFd slave{::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
    if (!slave) {
        return std::unexpected(LastErrno("open"));
    }
    if (auto nonBlocking = SetNonBlocking(master.Get()); !nonBlocking) {
        return std::unexpected(nonBlocking.error());
    }
    return Pty{.Master = std::move(master), .Slave = std::move(slave)};
}

IoResult<std::size_t> ReadPty(int master, MutableByteSpan into) noexcept {
    auto n = ReadSome(master, into);
    if (!n && n.error().Code == EIO) {
        return 0;
    }
    return n;
}

}  // namespace zet::io
