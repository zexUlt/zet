#include "zet/app/registration.hpp"

namespace zet::app {

io::IoResult<void> Registration::Want(io::Poller& poller, bool read,
                                      bool write) noexcept {
    if (!read && !write) {
        Forget(poller);
        return {};
    }
    const io::EInterest interest = read && write ? io::EInterest::ReadWrite
                                   : read        ? io::EInterest::Read
                                                 : io::EInterest::Write;
    if (!Watched_) {
        if (auto watched = poller.Watch(Fd_, interest, Token_); !watched) {
            return watched;
        }
        Watched_ = true;
        Interest_ = interest;
        return {};
    }
    if (interest == Interest_) {
        return {};
    }
    if (auto modified = poller.Modify(Fd_, interest, Token_); !modified) {
        return modified;
    }
    Interest_ = interest;
    return {};
}

void Registration::Forget(io::Poller& poller) noexcept {
    if (Watched_) {
        static_cast<void>(poller.Unwatch(Fd_));
        Watched_ = false;
    }
}

}  // namespace zet::app
