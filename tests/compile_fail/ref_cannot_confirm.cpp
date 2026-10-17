// Moving the seed on is the §8 ratchet, and only a handle may do it.

#include "zet/session/agent_session.hpp"

namespace zet::session {

[[maybe_unused]] void TryToRatchetFromARef(SessionRef ref,
                                           SessionHandle& handle) {
    static_cast<void>(handle.ConfirmResume(EResumeKeys::Current));
    static_cast<void>(ref);
#ifdef ZET_EXPECT_FAIL
    static_cast<void>(ref.ConfirmResume(EResumeKeys::Current));
#endif
}

}  // namespace zet::session
