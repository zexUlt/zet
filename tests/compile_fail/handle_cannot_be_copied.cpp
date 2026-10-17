// A handle stands for one authenticated connection. A copy would let a
// connection that has since been replaced keep the right to change the session.

#include <utility>

#include "zet/session/agent_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToCopyAHandle(SessionHandle& handle) {
    SessionHandle moved{std::move(handle)};
    static_cast<void>(moved);
#ifdef ZET_EXPECT_FAIL
    SessionHandle copy{moved};
#endif
}

}  // namespace zet::session
