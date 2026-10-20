#include "zet/io/activation.hpp"

#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace zet::io {

namespace {

constexpr const char* LISTEN_PID = "LISTEN_PID";
constexpr const char* LISTEN_FDS = "LISTEN_FDS";
constexpr const char* LISTEN_FDNAMES = "LISTEN_FDNAMES";

/// A whole decimal number and nothing else, or nothing.
template <typename TNumber>
bool ParseWhole(const char* text, TNumber& out) noexcept {
    const std::string_view view{text};
    if (view.empty()) {
        return false;
    }
    const auto [end, error] =
        std::from_chars(view.data(), view.data() + view.size(), out);
    return error == std::errc{} && end == view.data() + view.size();
}

void Forget() noexcept {
    // The loop is one thread, and this runs before it (§10): setenv's lack of
    // thread safety has nobody to race.
    // NOLINTBEGIN(concurrency-mt-unsafe)
    ::unsetenv(LISTEN_PID);
    ::unsetenv(LISTEN_FDS);
    ::unsetenv(LISTEN_FDNAMES);
    // NOLINTEND(concurrency-mt-unsafe)
}

}  // namespace

IoResult<ActivatedSockets> TakeActivatedSockets() noexcept {
    // NOLINTBEGIN(concurrency-mt-unsafe)
    const char* pidText = std::getenv(LISTEN_PID);
    const char* countText = std::getenv(LISTEN_FDS);
    // NOLINTEND(concurrency-mt-unsafe)
    ActivatedSockets sockets;
    if (pidText == nullptr || countText == nullptr) {
        Forget();
        return sockets;
    }
    pid_t pid = 0;
    if (!ParseWhole(pidText, pid) || pid != ::getpid()) {
        Forget();
        return sockets;
    }
    std::size_t count = 0;
    const bool parsed = ParseWhole(countText, count);
    Forget();
    if (!parsed || count > MAX_ACTIVATED_SOCKETS) {
        return std::unexpected(Errno{.Code = EINVAL, .Call = "LISTEN_FDS"});
    }
    for (std::size_t i = 0; i < count; ++i) {
        UniqueFd fd{FIRST_ACTIVATED_FD + static_cast<int>(i)};
        if (auto prepared = PrepareDescriptor(fd.Get()); !prepared) {
            return std::unexpected(prepared.error());
        }
        sockets.Items.at(i) = std::move(fd);
        ++sockets.Count;
    }
    return sockets;
}

}  // namespace zet::io
