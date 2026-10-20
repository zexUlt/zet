#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"

/// systemd socket activation, per docs/design.md §6: systemd binds port 2022
/// and hands the listening socket over, so zet-muxd needs neither root nor
/// CAP_NET_BIND_SERVICE. The protocol of sd_listen_fds(3), written out:
/// libsystemd would be a second runtime dependency (invariant 8) for twenty
/// lines of it.
namespace zet::io {

/// The first descriptor systemd passes, SD_LISTEN_FDS_START.
inline constexpr int FIRST_ACTIVATED_FD = 3;

/// The most sockets taken over. The mux listens on one port; the margin is
/// for a unit that also passes, say, an IPv6-only socket separately.
inline constexpr std::size_t MAX_ACTIVATED_SOCKETS = 8;

struct ActivatedSockets {
    std::array<UniqueFd, MAX_ACTIVATED_SOCKETS> Items{};
    std::size_t Count{0};

    [[nodiscard]] std::span<const UniqueFd> View() const noexcept {
        return std::span{Items}.first(Count);
    }
};

/// Takes the sockets systemd passed, if it passed any to this process.
///
/// LISTEN_PID must name this process — a variable inherited from a parent
/// is not an offer — and LISTEN_FDS says how many, from FIRST_ACTIVATED_FD
/// on. Each comes back close-on-exec and non-blocking. LISTEN_PID,
/// LISTEN_FDS and LISTEN_FDNAMES are removed from the environment either way,
/// so that a child never mistakes them for its own. None passed is Count 0,
/// not an error; a malformed count or more than MAX_ACTIVATED_SOCKETS fails
/// with EINVAL, Call "LISTEN_FDS".
[[nodiscard]] IoResult<ActivatedSockets> TakeActivatedSockets() noexcept;

}  // namespace zet::io
