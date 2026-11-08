/// The first EchoAck of a connection is a baseline (M7, design.md §9): the
/// point the agent counts from, which says nothing of the pty and is not
/// checked against what the client predicted. Through the real client and
/// agent, with a proxy that loses an EchoAck.
///
/// The agent queues EchoAck I=3 for the typed c and counts it reported. The
/// proxy swallows it and cuts the connection. The client reconnects; the
/// baseline of the new connection says I=3, the output it holds, and nothing
/// of the shell, which has not answered. Checked as an EchoAck it would find
/// the cell empty, call the prediction wrong and take it off the screen.
///
/// Kills: the baseline rule left out of Client::OnEchoAcked (every EchoAck goes
/// to the engine).

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: the first EchoAck of a connection is not checked "
                  "against the predictions") {
    ClientRig rig{
        221, {.Mode = zet::predict::EDisplayMode::Always, .GiveUp = 5000ms}};
    rig.Warm();
    REQUIRE(rig.Proxy().Connections() == 1);

    // The agent's EchoAck for c is lost on its way.
    rig.Proxy().Blackhole(true);
    rig.Keys("c");
    REQUIRE(rig.WaitFor([&] { return rig.Inserts().size() == 1; }));
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('c') != std::string::npos; }));
    rig.Pump(400ms);               // the agent sends it, into the proxy
    rig.Proxy().Blackhole(false);  // before the cut: the new connection's
                                   // handshake must get through
    rig.Proxy().Cut();
    rig.Pump(100ms);
    REQUIRE(rig.WaitFor([&] { return rig.Proxy().Connections() == 2; }));
    rig.Pump(600ms);  // handshake, resume, the new connection's EchoAck

    // The prediction is still on the terminal: nothing took it off.
    CHECK(rig.Inserts().size() == 1);

    // The shell answers; the stream is the agent's and the screen is right.
    rig.Print("c");
    CHECK(rig.WaitFor([&] { return rig.WithoutOverlay() == "\r$ aBc"; }));
    rig.Pump(200ms);
    rig.Finish();
    CHECK(rig.WithoutOverlay() == "\r$ aBc");
    const ScreenOf shown{rig.Out()};
    CHECK(shown.Row(0) == "$ aBc");
}
