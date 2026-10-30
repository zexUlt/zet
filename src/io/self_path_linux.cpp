#include "zet/io/self_path.hpp"

#include <limits.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace zet::io {

IoResult<std::string> SelfPath() noexcept {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return std::unexpected(LastErrno("readlink"));
    }
    buf[n] = '\0';
    return std::string{buf, static_cast<std::size_t>(n)};
}

}  // namespace zet::io
