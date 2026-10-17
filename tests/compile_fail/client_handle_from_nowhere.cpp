// A ClientSessionHandle is proof of a successful mutual handshake. It can only
// be received from a finished ClientResumeAttempt and nowhere else — not built
// from the session itself.

#include <utility>

#include "zet/session/client_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToMintAClientHandle(ClientResumeAttempt& attempt,
                                             ClientSession& session) {
    auto resumed = std::move(attempt).Finish();
    static_cast<void>(resumed);
    static_cast<void>(session);
#ifdef ZET_EXPECT_FAIL
    ClientSessionHandle forged{session};
#endif
}

}  // namespace zet::session
