#include "zet/session/error.hpp"

#include <doctest/doctest.h>

namespace {

using zet::EDisposition;
using zet::session::ESessionError;

}  // namespace

TEST_CASE(
    "a peer wrong about the stream costs the connection, not the session") {
    CHECK(DispositionOf(ESessionError::AckBeyondSent) ==
          EDisposition::CloseConnection);
    CHECK(DispositionOf(ESessionError::StreamGap) ==
          EDisposition::CloseConnection);
}

TEST_CASE("output that cannot be replayed whole ends the session") {
    CHECK(DispositionOf(ESessionError::DisconnectBufferOverflow) ==
          EDisposition::KillSession);
}

TEST_CASE("every session error has a reason worth showing the user") {
    for (const ESessionError error :
         {ESessionError::AckBeyondSent, ESessionError::StreamGap,
          ESessionError::DisconnectBufferOverflow}) {
        CAPTURE(static_cast<int>(error));
        CHECK_FALSE(Describe(error).empty());
        CHECK(Describe(error) != "unknown session error");
    }
}
