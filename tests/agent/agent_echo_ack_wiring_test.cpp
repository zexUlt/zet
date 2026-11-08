/// The agent's EchoAck wiring (src/agent/agent.cpp, design.md §9, «Предиктивное
/// эхо»), through the real agent loop with a real pty: the session-level rules
/// are in tests/session/agent_echo_ack_test.cpp on virtual time; these tests
/// are aimed at the glue between them and the loop, each at one mutation
/// checked by hand:
///
///   ServiceEchoAcks(now) not called in RunLoop
///   AgentConnection::NextDeadline without the EchoDeadline term
///   ConfirmPtyWrite replaced by ConfirmPtyInput in WritePty
///       "Agent: an EchoAck follows the input and the echo on the agent's own
///        timer"
///
/// The client is muted once the shell has the input, so it sends nothing, not
/// even its idle Ack: the only thing that wakes the agent after the echo is
/// its own echo deadline, and the test needs no tight wall-clock bound, only
/// a generous deadline for the EchoAck to arrive at all.

#include "agent_rig.hpp"

#include <doctest/doctest.h>

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: an EchoAck follows the input and the echo on the "
                  "agent's own timer") {
    AgentRig rig{91, TinyLimits(), /*inputToPty=*/true};
    auto client = rig.Connect(/*echoAck=*/true);

    // The connection is up when the EchoAck that opens it has come.
    REQUIRE(client->RunUntil(8000, [&] { return !client->EchoAcks.empty(); }));
    CHECK(client->Delivered.empty());

    const auto typed = Bytes("a");
    REQUIRE(rig.Session().Write(typed) == 1);
    std::vector<std::byte> shellRead;
    REQUIRE(client->RunUntil(8000, [&] {
        const auto more = rig.PtyInput();
        shellRead.insert(shellRead.end(), more.begin(), more.end());
        return !shellRead.empty();
    }));
    client->Mute = true;

    rig.PtyPrints(Bytes("a"));
    REQUIRE(
        client->RunUntil(10000, [&] { return client->EchoAcks.size() >= 2; }));
    CHECK(client->Delivered.size() == 1);
    const EchoAckMessage& ack = client->EchoAcks.back();
    CHECK(ack.StreamGen == 0);
    CHECK(ack.OutputOffset == 1);
    CHECK(ack.InputOffset == 1);
}
