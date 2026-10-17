// Whoever owns the client session holds ClientSession itself. That must not be
// a way around the handle: the session has no public mutators.

#include "zet/session/client_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToChangeTheClientSession(
    ClientSession& session, const wire::HandshakeNonce& nonce) {
    static_cast<void>(session.Id());
    static_cast<void>(nonce);
#ifdef ZET_EXPECT_FAIL
    session.Seen_.Remember(nonce);
#endif
}

}  // namespace zet::session
