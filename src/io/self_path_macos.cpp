#include "zet/io/self_path.hpp"

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>

#include <cerrno>
#include <cstdint>
#include <string>

namespace zet::io {

IoResult<std::string> SelfPath() noexcept {
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        // _NSGetExecutablePath returns -1 when the buffer is too small and
        // sets size to the required length.  PATH_MAX is always large enough
        // for a valid path, so reaching here means a kernel-level failure.
        return std::unexpected(
            Errno{.Code = ENOMEM, .Call = "_NSGetExecutablePath"});
    }
    char resolved[PATH_MAX];
    if (::realpath(buf, resolved) == nullptr) {
        return std::unexpected(LastErrno("realpath"));
    }
    return std::string{resolved};
}

}  // namespace zet::io
