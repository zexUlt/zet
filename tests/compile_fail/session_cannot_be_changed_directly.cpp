// Whoever owns the session table holds AgentSession itself. That must not be a
// way around the handle either: the session has no public mutators.

#include "zet/session/agent_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToChangeTheSession(AgentSession& session,
                                            const wire::HandshakeNonce& nonce) {
    static_cast<void>(session.Ref());
    static_cast<void>(nonce);
#ifdef ZET_EXPECT_FAIL
    session.Seen_.Remember(nonce);
#endif
}

}  // namespace zet::session
