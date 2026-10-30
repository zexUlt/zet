#include "zet/io/self_path.hpp"

#include <linux/limits.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace zet::io {

IoResult<std::string> SelfPath() noexcept {
    // Start at PATH_MAX and double the buffer if readlink fills it exactly —
    // that is the sign that the result was truncated.  In practice Linux paths
    // cannot exceed PATH_MAX, but the retry costs nothing and keeps the code
    // correct under any kernel that relaxes that limit.
    std::string buf(PATH_MAX, '\0');
    for (;;) {
        const std::size_t capacity = buf.size();
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), capacity);
        if (n < 0) {
            return std::unexpected(LastErrno("readlink"));
        }
        if (static_cast<std::size_t>(n) < capacity) {
            buf.resize(static_cast<std::size_t>(n));
            break;
        }
        // Buffer was exactly full — may be truncated; retry with more space.
        buf.resize(capacity * 2);
    }

    // The kernel appends " (deleted)" when the binary has been replaced on
    // disk since the process started.  Strip it: the original path is still
    // what callers want (e.g. to find agents beside the binary).
    constexpr std::string_view kDeleted = " (deleted)";
    if (buf.size() > kDeleted.size() &&
        std::string_view{buf}.substr(buf.size() - kDeleted.size()) ==
            kDeleted) {
        buf.resize(buf.size() - kDeleted.size());
    }
    return buf;
}

}  // namespace zet::io
