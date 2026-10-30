#include "zet/io/self_path.hpp"

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace zet::io {

IoResult<std::string> SelfPath() noexcept {
    // _NSGetExecutablePath fills buf and returns 0 on success, or -1 when the
    // buffer is too small, updating size to the required number of bytes.
    // PATH_MAX is normally large enough; the retry handles the rare case where
    // it is not (e.g. a deeply nested symlink target after resolution).
    uint32_t size = PATH_MAX;
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        // size now holds the required capacity — retry once.
        buf.resize(size);
        if (_NSGetExecutablePath(buf.data(), &size) != 0) {
            return std::unexpected(
                Errno{.Code = ENOMEM, .Call = "_NSGetExecutablePath"});
        }
    }
    buf.resize(std::strlen(buf.c_str()));  // trim to actual string length

    // Resolve symlinks and ./ or ../ components so the returned path is
    // canonical, matching what Linux's /proc/self/exe gives.
    char resolved[PATH_MAX];
    if (::realpath(buf.c_str(), resolved) == nullptr) {
        return std::unexpected(LastErrno("realpath"));
    }
    return std::string{resolved};
}

}  // namespace zet::io
