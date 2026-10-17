#include "zet/io/errno.hpp"

#include <netdb.h>

#include <cerrno>
#include <cstring>

namespace zet::io {

bool Errno::WouldBlock() const noexcept {
    // The same value on Linux, but POSIX lets them differ.
    // NOLINTNEXTLINE(misc-redundant-expression)
    return Code == EAGAIN || Code == EWOULDBLOCK;
}

std::string_view Errno::Description() const noexcept {
    // strerror is not thread-safe, and the event loop is one thread (§10). The
    // resolver worker, the one other thread, reports through its own channel.
    // getaddrinfo reports in a code space of its own (zet/io/tcp.hpp).
    if (Call == "getaddrinfo") {
        return ::gai_strerror(Code);
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    return std::strerror(Code);
}

Errno LastErrno(std::string_view call) noexcept {
    return Errno{.Code = errno, .Call = call};
}

}  // namespace zet::io
