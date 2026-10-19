#include "zet/io/poller.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <utility>

// What both backends share: the timeout arithmetic and the bookkeeping of
// which signals this poller blocked. The rest is poller_epoll.cpp or
// poller_kqueue.cpp, picked by src/io/CMakeLists.txt.
namespace zet::io {

int TimeoutMillis(Deadline deadline, Timestamp now) noexcept {
    if (!deadline.IsSet()) {
        return -1;
    }
    const Duration left = deadline.At() - now;
    if (left <= Duration::zero()) {
        return 0;
    }
    const auto millis =
        std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(millis, INT_MAX));
}

Poller::Poller(PollerBackend backend) noexcept : Backend_(std::move(backend)) {
    sigemptyset(&Blocked_);
}

Poller::Poller(Poller&& other) noexcept
    : Backend_(std::move(other.Backend_)),
      WakerToken_(other.WakerToken_),
      Blocked_(other.Blocked_),
      SignalTokens_(other.SignalTokens_) {
    // The moved-from poller owns no signals any more, and must not unblock
    // them when it goes.
    sigemptyset(&other.Blocked_);
}

Poller::~Poller() {
    static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &Blocked_, nullptr));
}

std::optional<std::uint64_t> Poller::SignalToken(int signo) const noexcept {
    if (signo <= 0 || signo > MAX_SIGNAL) {
        return std::nullopt;
    }
    return SignalTokens_[static_cast<std::size_t>(signo)];
}

}  // namespace zet::io
