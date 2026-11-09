/// `--predict=never` is byte-identical to a client without the overlay (§9),
/// failures included: after a failure Finish leaves the stdout queue as it
/// found it. Only an undo that is queued is flushed.
///
/// Stdout is a regular file here. The poller refuses to watch one, so the
/// client fails ("the terminal went away") as soon as the first output is
/// queued, with the fd writable and the output still in the queue: a flush at
/// Finish would put it in the file.
///
/// Kills: the flush after a failure not gated on an undo having been queued.

#include "client_predict_rig.hpp"

TEST_CASE_FIXTURE(SodiumFixture,
                  "Client: never leaves the queue unwritten after a failure") {
    ClientRig rig{
        234, {.Mode = zet::predict::EDisplayMode::Never, .StdoutIsFile = true}};
    rig.Keys("\n");
    REQUIRE(rig.WaitFor(
        [&] { return rig.ShellInput().find('\n') != std::string::npos; }));
    rig.Print("\r$ hello");
    // The client ends by itself: the failure is what ends it.
    CHECK(rig.WaitFor([&] { return rig.ClientEnded(); }));
    rig.Finish();
    CHECK(rig.ClientStatus() != 0);
    CHECK(rig.Out().empty());
}
