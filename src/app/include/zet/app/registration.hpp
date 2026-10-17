#pragma once

#include <cstdint>

#include "zet/io/errno.hpp"
#include "zet/io/poller.hpp"

namespace zet::app {

/// One descriptor's place in the Poller, changed only when what the loop wants
/// from it changes. EInterest has no "nothing", so wanting nothing unwatches
/// it: a pty under backpressure must not wake the loop just for being
/// readable.
class Registration {
public:
    Registration(int fd, std::uint64_t token) noexcept
        : Fd_(fd), Token_(token) {}

    /// Makes the poller report exactly `read` and `write` for the descriptor.
    [[nodiscard]] io::IoResult<void> Want(io::Poller& poller, bool read,
                                          bool write) noexcept;

    /// Takes the descriptor out of the poller, before it is closed.
    void Forget(io::Poller& poller) noexcept;

    [[nodiscard]] int Fd() const noexcept { return Fd_; }
    [[nodiscard]] std::uint64_t Token() const noexcept { return Token_; }

private:
    int Fd_;
    std::uint64_t Token_;
    bool Watched_{false};
    io::EInterest Interest_{io::EInterest::Read};
};

}  // namespace zet::app
