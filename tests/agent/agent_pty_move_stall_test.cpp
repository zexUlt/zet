/// The real agent's output path when the ring is full (design.md §9), driven
/// with a real pty and a real ClientConnection over TCP.
///
/// The output ring is as small as ResendMax allows, and the client takes a
/// whole ring's worth before it acknowledges anything (a bare Ack, after it
/// has been idle). That one Ack is read in Agent::Pump, after the loop's
/// MovePtyOutput of the turn has run. The output still waiting in the agent's
/// pty buffer has to move into the freed ring in the same turn: the pty read
/// gate is closed while it waits, so nothing else wakes the loop.
///
/// Kills: the MovePtyOutput after the Pump loop in Agent::RunLoop removed
/// (the client gets exactly ResendMax bytes and then nothing).

#include "agent_rig.hpp"

namespace {

constexpr std::size_t RING = 65536;
constexpr std::size_t OUTPUT = 1024U * 1024U;

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: output left in the pty buffer moves when an Ack "
                  "frees the ring") {
    auto limits = TinyLimits();
    limits.ResendMax = RING;
    limits.DisconnectMax = RING;
    AgentRig rig{93, limits};

    auto peer = rig.Connect();
    peer->QueueResize(24, 80);
    REQUIRE(peer->RunUntil(8000, [&] { return peer->LiveFramesSent >= 2; }));
    rig.AwaitResumeTaken(*peer);
    const auto already = peer->Delivered.size();

    std::vector<std::byte> output(OUTPUT);
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::byte>('a' + i % 26);
    }
    // The shell prints a megabyte, as fast as the pty takes it; the client
    // reads as it goes.
    std::size_t printed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (peer->Delivered.size() - already < output.size() &&
           std::chrono::steady_clock::now() < deadline && !peer->Ended()) {
        if (printed < output.size()) {
            printed += rig.PtyPrintsSome(ByteSpan{output}.subspan(printed));
        }
        peer->Pump();
    }
    const std::vector<std::byte> got(
        peer->Delivered.begin() + static_cast<std::ptrdiff_t>(already),
        peer->Delivered.end());
    rig.Finish();

    CHECK(got.size() == output.size());
    CHECK(got == output);
}
