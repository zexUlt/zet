// A SessionRef points at the session, but the pointer is not the caller's to
// follow: what an unauthenticated connection holds reads, and nothing else.

#include "zet/session/agent_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToReachThroughARef(SessionRef ref) {
    static_cast<void>(ref.Id());
#ifdef ZET_EXPECT_FAIL
    static_cast<void>(ref.Session_);
#endif
}

}  // namespace zet::session
