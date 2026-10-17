// A ClientSessionHandle stands for one authenticated connection. A copy would
// let a stale connection keep the right to advance the session's seed.

#include <utility>

#include "zet/session/client_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToCopyAClientHandle(ClientSessionHandle& handle) {
    ClientSessionHandle moved{std::move(handle)};
    static_cast<void>(moved);
#ifdef ZET_EXPECT_FAIL
    ClientSessionHandle copy{moved};
#endif
}

}  // namespace zet::session
