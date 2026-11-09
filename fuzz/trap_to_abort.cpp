// Turns SIGTRAP into SIGABRT in every fuzzer.
//
// __builtin_trap is brk on aarch64 and ud2 on x86-64: SIGTRAP on the first,
// SIGILL on the second. libFuzzer writes the failing input from its handlers
// for SIGILL and SIGABRT, and has none for SIGTRAP, so on aarch64 a trap from
// a fuzzer's own check or from libc++ hardening killed the process with no
// artifact. Aborting from the handler puts the trap through the path that does
// save the input, and prints the sanitizer's stack on the way.
#include <csignal>
#include <cstdlib>

namespace {

void OnTrap(int /*signo*/) { std::abort(); }

[[gnu::constructor]] void InstallTrapHandler() {
    struct sigaction action{};
    action.sa_handler = OnTrap;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, nullptr);
}

}  // namespace
