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
/// (the client gets exactly ResendMax bytes and then nothing); and the loop
/// taking another turn only when PtyFrom_ changed, not the pending count (a
/// move that drains the buffer resets both offsets), which the second test
/// catches: the client's idle Ack would otherwise wake the loop and hide it.

#include "agent_rig.hpp"

namespace {

constexpr std::size_t RING = 65536;
constexpr std::size_t OUTPUT = 1024U * 1024U;
/// What the output ring takes before the client has to acknowledge: one byte
/// short of ResendMax.
constexpr std::size_t RING_ROOM = RING - 1;
/// Less than a pty read (4095 bytes), so the agent has read all of it, and
/// the pty is empty, when the Ack comes: nothing else wakes the loop then.
constexpr std::size_t TAIL = 1000;

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

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: the output moved by the Ack that freed the ring is "
                  "sent in the same turn, without another wake-up") {
    auto limits = TinyLimits();
    limits.ResendMax = RING;
    limits.DisconnectMax = RING;
    AgentRig rig{94, limits};

    auto peer = rig.Connect();
    peer->QueueResize(24, 80);
    REQUIRE(peer->RunUntil(8000, [&] { return peer->LiveFramesSent >= 2; }));
    rig.AwaitResumeTaken(*peer);
    const auto already = peer->Delivered.size();

    // A full ring, then a bit that the agent reads from the pty behind the full
    // ring: the buffer then holds only that, from offset 0, and the one Ack
    // that frees the ring lets all of it move and leaves both offsets at 0.
    std::vector<std::byte> output(RING_ROOM + TAIL);
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::byte>('a' + i % 26);
    }
    // The client goes mute after the first frame it sends past the Resume and
    // the Resize: with nothing to send, that is the idle Ack that frees the
    // ring. Nothing after it wakes the agent, whose next wake-up is the
    // keepalive.
    std::size_t printed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    std::optional<std::chrono::steady_clock::time_point> mutedAt;
    while (peer->Delivered.size() - already < output.size() &&
           std::chrono::steady_clock::now() < deadline && !peer->Ended()) {
        // The tail only once the ring is full: the idle Ack is 200 ms away.
        const std::size_t allowed =
            peer->Delivered.size() - already >= RING_ROOM ? output.size()
                                                          : RING_ROOM;
        if (printed < allowed) {
            printed += rig.PtyPrintsSome(
                ByteSpan{output}.subspan(printed, allowed - printed));
        }
        peer->Pump();
        if (peer->LiveFramesSent > 2 && !mutedAt) {
            peer->Mute = true;
            mutedAt = std::chrono::steady_clock::now();
        }
        // Clean code delivers within milliseconds of the Ack; a stall would
        // last until the keepalive (5 s). Two seconds tell them apart.
        if (mutedAt && std::chrono::steady_clock::now() > *mutedAt + 2s) {
            break;
        }
    }
    const std::vector<std::byte> got(
        peer->Delivered.begin() + static_cast<std::ptrdiff_t>(already),
        peer->Delivered.end());
    rig.Finish();

    CHECK(mutedAt.has_value());
    CHECK(got.size() == output.size());
    CHECK(got == output);
}
