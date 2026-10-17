#include "zet/session/error.hpp"

namespace zet::session {

std::string_view Describe(ESessionError error) noexcept {
    switch (error) {
        case ESessionError::AckBeyondSent:
            return "peer acknowledged bytes that were never sent";
        case ESessionError::StreamGap:
            return "data arrived past the end of the stream";
        case ESessionError::DisconnectBufferOverflow:
            return "too much output while disconnected";
        case ESessionError::NotAuthenticated:
            return "resume did not authenticate";
        case ESessionError::UnknownSeed:
            return "resume opened under a seed the session does not hold";
        case ESessionError::PeerBehindBuffer:
            return "peer resumed from bytes it had already acknowledged";
    }
    return "unknown session error";
}

}  // namespace zet::session
