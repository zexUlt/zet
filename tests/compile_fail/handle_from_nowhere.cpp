// A SessionHandle is proof of authentication. It can be received from a
// finished ResumeAttempt and nowhere else — not built from the session itself.

#include <utility>

#include "zet/session/agent_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToMintAHandle(ResumeAttempt& attempt,
                                       AgentSession& session) {
    auto resumed = std::move(attempt).Finish();
    static_cast<void>(resumed);
    static_cast<void>(session);
#ifdef ZET_EXPECT_FAIL
    SessionHandle forged{session};
#endif
}

}  // namespace zet::session
