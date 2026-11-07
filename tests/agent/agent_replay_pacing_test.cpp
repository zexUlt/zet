/// The real agent's replay of output after a reconnect (design.md §9),
/// driven through pump_test_api.hpp with a real pty and a real
/// ClientConnection over TCP.
///
/// While the client is away the output ring grows past ResendMax, up to
/// DisconnectMax. The agent's per-connection output queue is ResendMax plus a
/// fixed slack, so the replay is larger than the queue. The replay has to go
/// out as the socket drains; a connection that is merely behind is not "too
/// far behind".
///
/// Kills: the send gate in Agent::Pump removed (the whole replay is appended
/// to the connection's queue in one turn, which refuses it and closes the
/// connection, on every reconnect, forever).

#include "agent_rig.hpp"

namespace {

constexpr std::size_t RESEND = 65536;
constexpr std::size_t DISCONNECT = 3U * 1024U * 1024U;
// More than the agent's connection queue holds (RESEND + 1 MiB of slack),
// less than the ring.
constexpr std::size_t OUTPUT = 2U * 1024U * 1024U + 512U * 1024U;

std::vector<std::byte> Sequence(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(i * 131 % 251);
    }
    return bytes;
}

}  // namespace

// The shell writes more than the connection queue holds while no client is
// attached; the client then connects once. Every byte arrives, in order, on
// that one connection.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a replay larger than the connection's output queue "
                  "is paced by the socket") {
    auto limits = TinyLimits();
    limits.ResendMax = RESEND;
    limits.DisconnectMax = DISCONNECT;
    AgentRig rig{92, limits};

    const auto output = Sequence(OUTPUT);
    rig.PtyPrints(output);
    SleepMs(500);  // the agent has read it all

    int connections = 1;
    auto peer = rig.Connect();
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    std::vector<std::byte> got;
    while (got.size() + peer->Delivered.size() < output.size() &&
           std::chrono::steady_clock::now() < deadline) {
        if (peer->Ended()) {
            got.insert(got.end(), peer->Delivered.begin(),
                       peer->Delivered.end());
            ++connections;
            peer = rig.Connect();
        }
        peer->Pump();
    }
    got.insert(got.end(), peer->Delivered.begin(), peer->Delivered.end());

    rig.Finish();

    CHECK(connections == 1);
    CHECK(got.size() == output.size());
    CHECK(got == output);
}
