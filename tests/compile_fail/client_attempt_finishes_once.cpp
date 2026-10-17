// Finish spends the attempt, and the call site has to say so with std::move.
// An attempt asked twice would hand out two handles for one proof.

#include <utility>

#include "zet/session/client_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToFinishClientAttemptInPlace(
    ClientResumeAttempt& attempt) {
#ifdef ZET_EXPECT_FAIL
    static_cast<void>(attempt.Finish());
#else
    static_cast<void>(std::move(attempt).Finish());
#endif
}

}  // namespace zet::session
