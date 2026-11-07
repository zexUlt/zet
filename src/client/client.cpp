#include "client.hpp"
#include "pump_test_api.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "deliver.hpp"
#include "escape.hpp"
#include "zet/app/byte_queue.hpp"
#include "zet/app/env.hpp"
#include "zet/app/registration.hpp"
#include "zet/core/shell_quote.hpp"
#include "zet/core/version.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/self_path.hpp"
#include "zet/io/tcp.hpp"
#include "zet/io/terminal.hpp"
#include "zet/session/bootstrap.hpp"
#include "zet/session/client_session.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/probe.hpp"

extern char** environ;

namespace zet::client {

namespace {

using namespace std::chrono_literals;

/// Minimum transfer budget once the remote has responded (ZET-PROBE received).
/// The actual budget is max(DELIVER_TIMEOUT, size_bytes / MIN_RATE + margin)
/// so that a 16 MiB binary at 1 Mbit/s (128 KiB/s) always fits.
constexpr Duration DELIVER_TIMEOUT = 60s;

/// How long the bootstrap phase (after delivery) may take.  ssh may be asking
/// for a passphrase on the terminal meanwhile.
constexpr Duration BOOTSTRAP_TIMEOUT = 120s;

/// How long ssh gets to exit once the agent has answered and detached.
constexpr Duration SSH_EXIT_TIMEOUT = 10s;

/// How long one connect may take before the next address, or the backoff.
constexpr Duration CONNECT_TIMEOUT = 5s;

/// The reconnect backoff of §9: doubling from here, with jitter, to the
/// ceiling, which ZET_RECONNECT_MAX lowers.
constexpr Duration BACKOFF_MIN = 250ms;
constexpr Duration BACKOFF_MAX = 30s;

/// How long the last of the agent's output gets to reach the terminal once
/// the session has ended.
constexpr Duration FLUSH_TIMEOUT = 10s;

/// Output the terminal has not taken yet. A socket is read only while this has
/// room for all that one read could deliver.
constexpr std::size_t STDOUT_QUEUE_MAX = 1024 * 1024;

// The socket is read only while this has room for a whole live frame; less,
// and the gate would never open.
static_assert(STDOUT_QUEUE_MAX >= session::CONNECTION_FRAME_STORAGE);

/// A snapshot is queued whole, on top of whatever output is ahead of it: the
/// read gate keeps that under STDOUT_QUEUE_MAX, and the wire caps the
/// snapshot, so every legal one fits.
constexpr std::size_t STDOUT_SNAPSHOT_CEILING =
    STDOUT_QUEUE_MAX + wire::SNAPSHOT_MAX_BYTES;

/// Sealed frames the socket has not taken yet, past the resend window.
constexpr std::size_t SOCKET_OUTPUT_SLACK = 1024 * 1024;

constexpr std::size_t STDIN_CHUNK = 4096;
constexpr std::size_t SOCKET_READ_CHUNK = 64 * 1024;
constexpr std::size_t SSH_OUTPUT_MAX = 256 * 1024;
constexpr std::size_t EVENTS_PER_WAIT = 16;

constexpr std::uint64_t STDIN_TOKEN = 1;
constexpr std::uint64_t STDOUT_TOKEN = 2;
constexpr std::uint64_t SOCKET_TOKEN = 3;
constexpr std::uint64_t SIGNAL_TOKEN = 4;

/// The convention of shells for a child that a signal ended.
constexpr int SIGNAL_STATUS_BASE = 128;
constexpr int USAGE_STATUS = 2;
constexpr int FAILURE_STATUS = 255;
constexpr int EXEC_FAILED_STATUS = 127;
/// SessionEnd kind=2: ring overflowed, client lacked GRID_CAPABLE (§7).
/// 70 is EX_SOFTWARE (sysexits.h): an error in the protocol layer, not in the
/// remote command.  Distinct from 128+n (signal) so scripts can tell them
/// apart.
constexpr int OVERFLOW_STATUS = 70;

constexpr std::string_view DEFAULT_AGENT = "zet-agent";

/// The two lines zet writes to stderr when its link to the agent goes and
/// comes back. On stderr and never into the session: the agent's stream on
/// stdout stays exactly what the agent sent (tests/integration compare it).
constexpr std::string_view LOST_NOTICE =
    "zet: connection lost, reconnecting\u2026 (Enter ~ . to quit)\r\n";
constexpr std::string_view REGAINED_NOTICE = "zet: reconnected\r\n";
constexpr std::string_view ESCAPE_NOTICE = "zet: closed by ~.\r\n";
/// Printed when the agent sends SessionEnd kind=2 (\u00a77): the ring
/// overflowed while the client did not have GRID_CAPABLE.
constexpr std::string_view OVERFLOW_NOTICE =
    "zet: session output overflowed \u2014 reconnect to resume\r\n";
/// Printed (via Notice) after a grid snapshot replaces missed output on
/// path B reconnect (ver=2, \u00a79).  On stderr so the user knows the screen
/// was restored from a snapshot rather than a byte-identical replay.
constexpr std::string_view SNAPSHOT_RESTORE_NOTICE =
    "zet: screen restored, output during the cut skipped\r\n";
/// Printed after SNAPSHOT_RESTORE_NOTICE when GridBegin carries the
/// PossiblyDiverged flag (bit 0): the agent's parser lost sync at some point,
/// so the restored screen may differ from what the program drew.
constexpr std::string_view SNAPSHOT_DIVERGED_NOTICE =
    "zet: warning: the restored screen may differ from the remote one\r\n";
constexpr std::uint8_t GRID_FLAG_POSSIBLY_DIVERGED = 0x01;

constexpr std::string_view USAGE =
    "usage: zet [--agent PATH] [--ssh-arg ARG]... HOST\n"
    "  --agent PATH    use PATH as zet-agent on the remote host (disables "
    "delivery)\n"
    "  --ssh-arg ARG   passed to ssh as it is; repeat for several\n"
    "  --version       print the release version and exit\n";

void Say(std::string_view text) noexcept {
    static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

/// Returns a copy of `text` with every byte outside printable ASCII (0x20–0x7E)
/// replaced by '?'.  Used so that remote-controlled strings (arch names, error
/// reasons) are safe to print on the local terminal.
[[nodiscard]] std::string SanitizeAscii(std::string_view text) noexcept {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        out += (c >= 0x20U && c < 0x7FU) ? raw : '?';
    }
    return out;
}

void SayError(std::string_view what, std::string_view why) noexcept {
    Say("zet: ");
    Say(what);
    Say(": ");
    Say(why);
    Say("\r\n");
}

struct Options {
    bool PrintVersion{false};
    std::string Agent{DEFAULT_AGENT};
    /// True when --agent was given: skip delivery and run the agent directly.
    bool AgentExplicit{false};
    std::vector<std::string> SshArgs;
    std::string Host;
    session::SessionLimits Limits;
    Duration BackoffMax{BACKOFF_MAX};
    /// Test hooks (ZET_TEST_VERSION_CEILING, ZET_TEST_NO_GRID_CAPABLE): the
    /// protocol ceiling offered in Hello, and whether GRID_CAPABLE goes with
    /// it. Production offers the default ceiling and always the TLV.
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_MAX};
    bool GridCapable{true};
};

/// What of the local environment goes to the shell on the far side: the
/// terminal and the language (§4, wire::IsForwardedVariable). A variable the
/// bootstrap cannot carry — too long, not printable — stays home.
wire::BootstrapEnvironment ForwardedEnvironment() noexcept {
    wire::BootstrapEnvironment environment;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        const auto equals = text.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto name = text.substr(0, equals);
        if (wire::IsForwardedVariable(name)) {
            static_cast<void>(environment.Add(name, text.substr(equals + 1)));
        }
    }
    return environment;
}

std::optional<Options> ReadOptions(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--agent" && i + 1 < args.size()) {
            options.Agent = args[++i];
            options.AgentExplicit = true;
        } else if (arg == "--ssh-arg" && i + 1 < args.size()) {
            options.SshArgs.emplace_back(args[++i]);
        } else if (arg == "--version") {
            options.PrintVersion = true;
            return options;
        } else if (!arg.starts_with("-") && options.Host.empty()) {
            options.Host = arg;
        } else {
            return std::nullopt;
        }
    }
    if (options.Host.empty()) {
        return std::nullopt;
    }
    using app::EUnit;
    if (auto keepalive = app::DurationFromEnv("ZET_KEEPALIVE_INTERVAL",
                                              EUnit::Milliseconds)) {
        options.Limits.Liveness.KeepaliveInterval = *keepalive;
    }
    if (auto silence = app::DurationFromEnv("ZET_PEER_SILENCE_TIMEOUT",
                                            EUnit::Milliseconds)) {
        options.Limits.Liveness.PeerSilenceTimeout = *silence;
    }
    if (auto ceiling =
            app::DurationFromEnv("ZET_RECONNECT_MAX", EUnit::Milliseconds)) {
        options.BackoffMax = std::max(*ceiling, Duration{BACKOFF_MIN});
    }
    // Test hooks, read as the agent's are: anything but the exact values is
    // reported and ignored.
    static constexpr std::array<std::string_view, 2> CEILINGS{"1", "2"};
    if (const auto ceiling =
            app::ChoiceFromEnv("ZET_TEST_VERSION_CEILING", CEILINGS)) {
        options.Ceiling = *ceiling == "1" ? wire::PROTOCOL_VERSION_MIN
                                          : wire::PROTOCOL_VERSION_MAX;
    }
    static constexpr std::array<std::string_view, 1> ONLY_ONE{"1"};
    if (app::ChoiceFromEnv("ZET_TEST_NO_GRID_CAPABLE", ONLY_ONE)) {
        options.GridCapable = false;
    }
    return options;
}

std::string Quoted(std::string_view text) {
    std::string out(ShellQuotedSize(text), '\0');
    const auto written = ShellQuote(text, std::span{out});
    out.resize(written ? *written : 0);
    return out;
}

/// The one remote command: the agent's path, quoted, and a constant word
/// (§4). Nothing variable goes into it but the path.
std::string RemoteCommand(const Options& options) {
    return Quoted(options.Agent) + " --bootstrap";
}

/// A child with pipes on its stdin and stdout; stderr stays ours, so that
/// ssh can speak to the user.
struct Process {
    pid_t Pid{-1};
    io::UniqueFd In;
    io::UniqueFd Out;
};

std::optional<Process> Run(const std::vector<std::string>& argv) {
    auto in = io::MakeBlockingPipe();
    if (!in) {
        return std::nullopt;
    }
    Process process;
    process.In = std::move(in->Write);
    io::UniqueFd childIn = std::move(in->Read);
    auto out = io::MakeBlockingPipe();
    if (!out) {
        return std::nullopt;
    }
    process.Out = std::move(out->Read);
    io::UniqueFd childOut = std::move(out->Write);

    std::vector<char*> pointers;
    pointers.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        pointers.push_back(const_cast<char*>(arg.c_str()));
    }
    pointers.push_back(nullptr);

    process.Pid = ::fork();
    if (process.Pid < 0) {
        return std::nullopt;
    }
    if (process.Pid == 0) {
        // dup2 clears close-on-exec on the copies; the originals close.
        if (::dup2(childIn.Get(), STDIN_FILENO) < 0 ||
            ::dup2(childOut.Get(), STDOUT_FILENO) < 0) {
            ::_exit(EXEC_FAILED_STATUS);
        }
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::signal(SIGPIPE, SIG_DFL);
        ::execvp(pointers[0], pointers.data());
        ::_exit(EXEC_FAILED_STATUS);
    }
    if (!io::SetNonBlocking(process.In.Get()) ||
        !io::SetNonBlocking(process.Out.Get())) {
        return std::nullopt;
    }
    return process;
}

/// Reaps `pid`. A child whose stdout has ended is exiting and is waited
/// for; one that has not is told to stop first, since nothing is left to
/// read from it.
int Reap(pid_t pid, bool outputEnded) noexcept {
    if (!outputEnded) {
        static_cast<void>(::kill(pid, SIGTERM));
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

struct Output {
    std::string Text;
    /// The stream reached its end, rather than `until` or the limit.
    bool Ended{false};
};

/// Reads `process`'s stdout to its end or `until`, at most `limit` bytes.
Output ReadAll(Process& process, Timestamp until, std::size_t limit) {
    Output output;
    std::string& text = output.Text;
    std::array<char, 4096> chunk{};
    while (text.size() < limit) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            break;
        }
        pollfd waiting{.fd = process.Out.Get(), .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, io::TimeoutMillis(Deadline{until}, now)) <= 0) {
            continue;
        }
        const auto n = io::ReadSome(
            process.Out.Get(),
            MutableByteSpan{reinterpret_cast<std::byte*>(chunk.data()),
                            chunk.size()});
        if (!n) {
            if (n.error().WouldBlock()) {
                continue;
            }
            output.Ended = true;
            break;
        }
        if (*n == 0) {
            output.Ended = true;
            break;
        }
        text.append(chunk.data(), *n);
    }
    return output;
}

std::vector<std::string> SshArgv(const Options& options,
                                 std::initializer_list<std::string_view> mode) {
    std::vector<std::string> argv{"ssh"};
    for (const std::string_view word : mode) {
        argv.emplace_back(word);
    }
    argv.insert(argv.end(), options.SshArgs.begin(), options.SshArgs.end());
    argv.push_back(options.Host);
    return argv;
}

/// The host ssh would connect to, after its config has had its say: an alias
/// in ~/.ssh/config is not a name the resolver knows.
std::optional<std::string> HostName(const Options& options) {
    auto process = Run(SshArgv(options, {"-G"}));
    if (!process) {
        return std::nullopt;
    }
    process->In.Reset();
    const Output output = ReadAll(
        *process, io::MonotonicNow() + SSH_EXIT_TIMEOUT, SSH_OUTPUT_MAX);
    static_cast<void>(Reap(process->Pid, output.Ended));
    const std::string& config = output.Text;
    constexpr std::string_view key = "hostname ";
    std::size_t at = 0;
    while (at < config.size()) {
        const std::size_t end = std::min(config.find('\n', at), config.size());
        const std::string_view line{config.data() + at, end - at};
        if (line.starts_with(key)) {
            return std::string{line.substr(key.size())};
        }
        at = end + 1;
    }
    return std::nullopt;
}

crypto::Key RandomKey() noexcept {
    crypto::KeyBytes bytes{};
    crypto::RandomBytes(MutableByteSpan{bytes});
    return crypto::Key{bytes};
}

template <typename TNonce>
TNonce RandomNonce() noexcept {
    TNonce nonce{};
    crypto::RandomBytes(MutableByteSpan{nonce});
    return nonce;
}

/// Delivery remote command (M5.5): wraps the constant installer script in
/// `exec /bin/sh -c '...'` so the user's login shell cannot interfere.
std::string DeliveryCommand() {
    return "exec /bin/sh -c " + Quoted(INSTALLER_SCRIPT);
}

}  // namespace

// Defined at zet::client scope (outside the anonymous namespace) so tests can
// call it without a separate symbol.  WriteAllPipe (below) is a thin wrapper.
bool TryWriteSnapshot(int fd, ByteSpan bytes, Timestamp until) noexcept {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            return false;
        }
        const auto wr = io::WriteSome(
            fd, ByteSpan{bytes.data() + sent, bytes.size() - sent});
        if (!wr) {
            if (!wr.error().WouldBlock()) {
                return false;
            }
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            static_cast<void>(
                ::poll(&pfd, 1, io::TimeoutMillis(Deadline{until}, now)));
            continue;
        }
        sent += *wr;
    }
    return true;
}

namespace {

/// Thin wrapper so delivery code keeps its original call site.
[[nodiscard]] bool WriteAllPipe(int fd, ByteSpan bytes,
                                Timestamp until) noexcept {
    return client::TryWriteSnapshot(fd, bytes, until);
}

/// Returns true when the agent binary for `arch` is simply absent from
/// `agentDir` (stat fails with ENOENT or ENOTDIR).  Returns false when the
/// file exists (regardless of readability or size) or when any other error is
/// reported.
/// Used to decide whether to fall through to the compile-time default: a
/// missing binary warrants a fallback, an existing but unloadable one does not.
static bool AgentMissing(std::string_view agentDir,
                         std::string_view arch) noexcept {
    std::string path;
    path.reserve(agentDir.size() + 12 + arch.size());
    path.append(agentDir);
    path += '/';
    path.append("zet-agent-");
    path.append(arch);
    struct ::stat st{};
    // ENOTDIR: a path component (e.g. a file named libexec) is not a
    // directory, which is as absent as a missing one.
    return ::stat(path.c_str(), &st) != 0 &&
           (errno == ENOENT || errno == ENOTDIR);
}

/// Runs the probe exchange with the installer and delivers the agent binary
/// if needed (design.md §4, "Доставка агента (M5.5)").  Returns any bytes
/// that followed the last installer line — they belong to the bootstrap
/// armor and must be fed to BootstrapClient next. Returns std::nullopt on
/// any error; the caller must SIGTERM and reap the process.
///
/// agentDir is the primary directory to search.  If the agent is not found
/// there and fallbackAgentDir is given, it is tried next before failing.
/// The final error message lists all directories that were tried.
std::optional<std::vector<std::byte>> DeliverAgent(
    Process& process, std::string_view agentDir,
    std::optional<std::string_view> fallbackAgentDir,
    Timestamp until) noexcept {
    wire::ProbeScanner scanner;
    std::array<std::byte, 4096> buf{};
    /// Bytes the scanner did not consume yet — carried to the next stage.
    std::vector<std::byte> leftover;
    bool ended = false;

    // Returns the next ProbeAnswer from the installer. Bytes that arrived
    // after the answer's newline are left in `leftover` for later.
    const auto next = [&]() -> std::optional<wire::ProbeAnswer> {
        while (!leftover.empty()) {
            const auto scan =
                scanner.Feed(ByteSpan{leftover.data(), leftover.size()});
            leftover.erase(
                leftover.begin(),
                leftover.begin() + static_cast<std::ptrdiff_t>(scan.Consumed));
            if (scan.Answer) {
                return scan.Answer;
            }
        }
        while (!ended) {
            const Timestamp now = io::MonotonicNow();
            if (now >= until) {
                return std::nullopt;
            }
            pollfd w{.fd = process.Out.Get(), .events = POLLIN, .revents = 0};
            if (::poll(&w, 1, io::TimeoutMillis(Deadline{until}, now)) <= 0) {
                continue;
            }
            const auto n =
                io::ReadSome(process.Out.Get(), MutableByteSpan{buf});
            if (!n) {
                if (!n.error().WouldBlock()) {
                    ended = true;
                }
                continue;
            }
            if (*n == 0) {
                ended = true;
                break;
            }
            const auto scan = scanner.Feed(ByteSpan{buf.data(), *n});
            if (scan.Consumed < *n) {
                const std::byte* rem = buf.data() + scan.Consumed;
                leftover.insert(leftover.end(), rem,
                                rem + (*n - scan.Consumed));
            }
            if (scan.Answer) {
                return scan.Answer;
            }
        }
        return std::nullopt;
    };

    // Stage 1: ZET-PROBE <version> <arch> exec|noexec
    // The login shell may emit ZET-HAVE, ZET-SEND, ZET-STORED, or ZET-FAILED
    // before the installer starts (e.g. a noisy .bashrc).  Skip them: only
    // ZET-PROBE opens the exchange.
    // answer1 is declared outside the loop so that probeInfo (which points
    // into it) remains valid after the loop exits.
    std::optional<wire::ProbeAnswer> answer1;
    const wire::ProbeInfo* probeInfo = nullptr;
    for (;;) {
        answer1 = next();
        if (!answer1) {
            SayError("deliver", "no ZET-PROBE from installer");
            return std::nullopt;
        }
        probeInfo = std::get_if<wire::ProbeInfo>(&*answer1);
        if (probeInfo) {
            break;
        }
        // Out-of-stage marker: treat as noise and keep waiting.
    }
    // Copy arch before the next scanner.Feed() call overwrites the line buffer.
    // Sanitize to printable ASCII so remote-controlled text is safe to print.
    const std::string arch = SanitizeAscii(probeInfo->Arch);
    if (arch != AGENT_ARCH_X86_64 && arch != AGENT_ARCH_AARCH64) {
        SayError("deliver", "unsupported remote architecture: " + arch);
        return std::nullopt;
    }
    if (probeInfo->NoExec) {
        SayError("deliver", "remote agent cache is on a noexec filesystem");
        return std::nullopt;
    }

    // Try directories in order: agentDir first, then fallbackAgentDir.
    //
    // When agentDir is the beside-binary directory, a missing binary is a
    // normal case — the tarball may have been installed without agents, and
    // the compile-time default may have them.  An existing but unloadable
    // binary (empty, corrupt, unreadable) is a local misconfiguration: report
    // it rather than silently falling through to a possibly-different version.
    std::string loadReason;
    auto binary = LoadAgentBinary(agentDir, arch, &loadReason);
    if (binary.empty() && fallbackAgentDir) {
        if (AgentMissing(agentDir, arch)) {
            // Binary absent in the beside-binary directory — try the default.
            std::string fallbackReason;
            auto fallbackBinary =
                LoadAgentBinary(*fallbackAgentDir, arch, &fallbackReason);
            if (!fallbackBinary.empty()) {
                binary = std::move(fallbackBinary);
                loadReason.clear();
            } else if (!fallbackReason.empty() && loadReason.empty()) {
                loadReason = fallbackReason;
            }
        } else {
            // Binary exists beside the binary but could not be loaded.  Do
            // not silently fall through to a possibly-different version.
            if (loadReason.empty()) {
                loadReason = "agent binary for " + arch + " in " +
                             std::string{agentDir} +
                             " exists but could not be read";
            }
            fallbackAgentDir = std::nullopt;  // suppress from the error line
        }
    }
    if (binary.empty()) {
        if (!loadReason.empty()) {
            SayError("deliver",
                     loadReason + " (set ZET_AGENT_DIR or use --agent PATH)");
        } else {
            std::string tried = "no agent binary for " + arch + " (tried " +
                                std::string{agentDir};
            if (fallbackAgentDir) {
                tried += ", ";
                tried += *fallbackAgentDir;
            }
            tried += "); set ZET_AGENT_DIR or use --agent PATH";
            SayError("deliver", tried);
        }
        return std::nullopt;
    }

    // ZET-PROBE proves SSH auth is done; reset the deadline to a transfer
    // budget scaled by actual binary size: at 1 Mbit/s (128 KiB/s) plus 30s
    // margin, a 16 MiB binary gets ~158 s, a 1 MiB binary gets at least
    // DELIVER_TIMEOUT (60 s).
    {
        constexpr std::size_t kMinBytesPerSec = 128UZ * 1024UZ;  // 1 Mbit/s
        constexpr Duration kMargin = 30s;
        const auto secs =
            (binary.size() + kMinBytesPerSec - 1UZ) / kMinBytesPerSec;
        const Duration budget =
            std::max(DELIVER_TIMEOUT, std::chrono::seconds(secs) + kMargin);
        until = io::MonotonicNow() + budget;
    }

    const std::string hashSize =
        HashHex(std::span{binary}) + " " + std::to_string(binary.size()) + "\n";
    if (!WriteAllPipe(
            process.In.Get(),
            std::as_bytes(std::span{hashSize.data(), hashSize.size()}),
            until)) {
        SayError("deliver", "could not send hash to installer");
        return std::nullopt;
    }

    // Stage 2: ZET-HAVE (cached), ZET-SEND (needs binary), or ZET-FAILED
    const auto answer2 = next();
    if (!answer2) {
        SayError("deliver", "no ZET-HAVE/ZET-SEND from installer");
        return std::nullopt;
    }
    if (const auto* f2 = std::get_if<wire::ProbeFailed>(&*answer2)) {
        SayError("deliver",
                 "installer failed at probe: " + SanitizeAscii(f2->Reason));
        return std::nullopt;
    }
    const bool needSend = std::holds_alternative<wire::ProbeSend>(*answer2);
    if (!needSend && !std::holds_alternative<wire::ProbeHave>(*answer2)) {
        SayError(
            "deliver",
            "unexpected installer message (expected ZET-HAVE or ZET-SEND)");
        return std::nullopt;
    }

    if (needSend) {
        if (!WriteAllPipe(process.In.Get(),
                          ByteSpan{binary.data(), binary.size()}, until)) {
            // Close stdin so the installer is unblocked, then drain briefly:
            // it may have already printed ZET-FAILED with a reason.
            process.In.Reset();
            const Timestamp drainUntil = io::MonotonicNow() + 2s;
            until = drainUntil;
            const auto drainAnswer = next();
            if (drainAnswer) {
                if (const auto* fd =
                        std::get_if<wire::ProbeFailed>(&*drainAnswer)) {
                    SayError("deliver",
                             "installer failed: " + SanitizeAscii(fd->Reason));
                    return std::nullopt;
                }
            }
            SayError("deliver", "could not send agent binary");
            return std::nullopt;
        }

        // Stage 3: ZET-STORED or ZET-FAILED <reason>
        const auto answer3 = next();
        if (!answer3) {
            SayError("deliver", "no ZET-STORED/ZET-FAILED from installer");
            return std::nullopt;
        }
        if (std::holds_alternative<wire::ProbeStored>(*answer3)) {
            // Delivery complete.
        } else if (const auto* f = std::get_if<wire::ProbeFailed>(&*answer3)) {
            SayError("deliver",
                     "installer failed: " + SanitizeAscii(f->Reason));
            return std::nullopt;
        } else {
            SayError("deliver",
                     "unexpected installer message (expected "
                     "ZET-STORED/ZET-FAILED)");
            return std::nullopt;
        }
    }

    return leftover;
}

/// §4 over `ssh -T HOST agent --bootstrap` (direct) or `exec /bin/sh -c
/// '<installer>'` (delivery, M5.5).  `agentDir` is set for delivery mode;
/// nullopt means the caller gave --agent PATH and wants the old direct path.
std::optional<session::BootstrapResult> Bootstrap(
    const Options& options, std::optional<std::string_view> agentDir,
    std::optional<std::string_view> agentFallbackDir = std::nullopt) {
    session::BootstrapClient bootstrap{RandomKey(),
                                       RandomNonce<wire::BootstrapNonce>(),
                                       ForwardedEnvironment()};
    std::array<char, session::BOOTSTRAP_OFFER_TEXT_SIZE> offer{};
    const auto length = bootstrap.WriteOffer(offer);
    if (!length) {
        SayError("bootstrap", "could not write the offer");
        return std::nullopt;
    }
    auto argv = SshArgv(options, {"-T"});
    argv.push_back(agentDir ? DeliveryCommand() : RemoteCommand(options));
    auto process = Run(argv);
    if (!process) {
        SayError("ssh", std::strerror(errno));
        return std::nullopt;
    }

    // Delivery mode: probe exchange before the bootstrap offer.
    // Auth cap is BOOTSTRAP_TIMEOUT — generous enough for passphrase / 2FA.
    // DeliverAgent resets the deadline to a size-scaled budget once ZET-PROBE
    // arrives (proving auth is done), so the 60-second transfer minimum only
    // starts after the first remote byte.  Bootstrap restarts fresh after.
    std::vector<std::byte> leftovers;
    if (agentDir) {
        const Timestamp authUntil = io::MonotonicNow() + BOOTSTRAP_TIMEOUT;
        auto maybe =
            DeliverAgent(*process, *agentDir, agentFallbackDir, authUntil);
        if (!maybe) {
            process->In.Reset();
            static_cast<void>(Reap(process->Pid, false));
            return std::nullopt;
        }
        leftovers = std::move(*maybe);
    }

    const Timestamp until = io::MonotonicNow() + BOOTSTRAP_TIMEOUT;

    // Far below what a pipe holds, so one write takes it all.
    const auto written = io::WriteSome(
        process->In.Get(), std::as_bytes(std::span{offer}.first(*length)));
    if (!written || *written != *length) {
        // Waiting would only spend BOOTSTRAP_TIMEOUT on an Accept that cannot
        // come: the agent never saw a whole Offer.
        SayError("bootstrap", "ssh did not take the offer");
        process->In.Reset();
        static_cast<void>(Reap(process->Pid, false));
        return std::nullopt;
    }

    std::array<std::byte, 4096> chunk{};
    std::optional<session::BootstrapResult> result;
    bool ended = false;

    // Feed bytes that arrived before the offer (delivery leftovers) first.
    if (!leftovers.empty()) {
        auto step =
            bootstrap.Feed(ByteSpan{leftovers.data(), leftovers.size()});
        if (!step) {
            SayError("bootstrap", step.error().Description());
            process->In.Reset();
            static_cast<void>(Reap(process->Pid, false));
            return std::nullopt;
        }
        result = std::move(step->Result);
    }

    while (!result && !ended) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            SayError("bootstrap", "no answer from the agent");
            break;
        }
        pollfd waiting{
            .fd = process->Out.Get(), .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, io::TimeoutMillis(Deadline{until}, now)) <= 0) {
            continue;
        }
        const auto n = io::ReadSome(process->Out.Get(), MutableByteSpan{chunk});
        if (!n) {
            ended = !n.error().WouldBlock();
            continue;
        }
        if (*n == 0) {
            ended = true;
            continue;
        }
        auto step = bootstrap.Feed(ByteSpan{chunk}.first(*n));
        if (!step) {
            SayError("bootstrap", step.error().Description());
            break;
        }
        result = std::move(step->Result);
    }
    process->In.Reset();
    // The agent detaches once it has answered, and ssh ends with it.
    const bool sshEnded =
        ended ||
        ReadAll(*process, io::MonotonicNow() + SSH_EXIT_TIMEOUT, SSH_OUTPUT_MAX)
            .Ended;
    // Not const: macOS's WIFEXITED casts its argument to a non-const int*.
    int status = Reap(process->Pid, sshEnded);
    if (!result && ended) {
        SayError("bootstrap", WIFEXITED(status) && WEXITSTATUS(status) != 0
                                  ? "ssh failed before the agent answered"
                                  : "the agent did not answer");
    }
    return result;
}

/// Puts stdin and stdout back as they were: the shell that started zet shares
/// their open file descriptions, O_NONBLOCK included.
class DescriptorFlags {
public:
    DescriptorFlags() noexcept {
        for (std::size_t i = 0; i < Saved_.size(); ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            Saved_[i] = ::fcntl(static_cast<int>(i), F_GETFL);
        }
    }
    ~DescriptorFlags() { Restore(); }
    DescriptorFlags(const DescriptorFlags&) = delete;
    DescriptorFlags& operator=(const DescriptorFlags&) = delete;
    DescriptorFlags(DescriptorFlags&&) = delete;
    DescriptorFlags& operator=(DescriptorFlags&&) = delete;

    /// Once: the destructor after an explicit call does nothing.
    void Restore() noexcept {
        for (std::size_t i = 0; i < Saved_.size(); ++i) {
            if (Saved_[i] >= 0) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                static_cast<void>(
                    ::fcntl(static_cast<int>(i), F_SETFL, Saved_[i]));
                Saved_[i] = -1;
            }
        }
    }

private:
    /// stdin and stdout.
    std::array<int, 2> Saved_{-1, -1};
};

std::optional<wire::ResizeMessage> TerminalSize() noexcept {
    const auto size = io::GetWindowSize(STDIN_FILENO);
    if (!size) {
        return std::nullopt;
    }
    return wire::ResizeMessage{.Rows = size->Rows,
                               .Cols = size->Cols,
                               .XPixel = size->XPixels,
                               .YPixel = size->YPixels};
}

/// Whether two descriptors are the same file. A terminal's stdout and
/// stderr are, and then they share the O_NONBLOCK zet puts on stdout.
bool SameFile(int a, int b) noexcept {
    struct stat first{};
    struct stat second{};
    return ::fstat(a, &first) == 0 && ::fstat(b, &second) == 0 &&
           first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

class Client {
public:
    /// `giveUpAfter` is how long the agent keeps a session nobody is
    /// connected to (BootstrapAccept): reconnecting for longer than that
    /// cannot succeed. Zero, or none, means never give up for that reason.
    Client(const Options& options, session::BootstrapResult bootstrapped,
           io::AddressList addresses, io::Poller poller,
           std::optional<Duration> giveUpAfter)
        : Options_(options),
          GiveUpAfter_(giveUpAfter && giveUpAfter->count() > 0 ? giveUpAfter
                                                               : std::nullopt),
          Addresses_(addresses),
          Poller_(std::move(poller)),
          Storage_(
              std::max(options.Limits.ResendMax, options.Limits.DisconnectMax)),
          Session_(std::move(bootstrapped.Secrets),
                   session::SessionStreams{.Storage = MutableByteSpan{Storage_},
                                           .Now = io::MonotonicNow(),
                                           .Limits = options.Limits}),
          Frames_(session::CONNECTION_FRAME_STORAGE),
          SocketOut_(options.Limits.ResendMax + SOCKET_OUTPUT_SLACK),
          Stdout_(STDOUT_QUEUE_MAX),
          StdinWatch_(STDIN_FILENO, STDIN_TOKEN),
          StdoutWatch_(STDOUT_FILENO, STDOUT_TOKEN),
          Size_(TerminalSize()),
          NoticesInline_(SameFile(STDOUT_FILENO, STDERR_FILENO)),
          Ceiling_(options.Ceiling),
          GridCapable_(options.GridCapable) {}

    /// Runs the session; the result is zet's exit status.
    [[nodiscard]] int Run() noexcept;

    /// Why the session could not go on, for stderr once the terminal is back.
    [[nodiscard]] const std::optional<std::string>& Fatal() const noexcept {
        return Fatal_;
    }

    /// Whether the user ended it with ~.
    [[nodiscard]] bool Escaped() const noexcept { return Escaped_; }

private:
    enum class ELink : std::uint8_t {
        /// No socket; LinkDeadline_ says when to try.
        Waiting,
        /// A connect in flight; LinkDeadline_ says when to give it up.
        Connecting,
        Up,
    };

    friend SessionTestResult RunSessionForTestImpl(
        SessionTestConfig config) noexcept;

    void FeedStdin() noexcept;
    void Pump(Timestamp now) noexcept;
    void StartConnecting(Timestamp now) noexcept;
    void FinishConnecting(Timestamp now) noexcept;
    void Drop(Timestamp now) noexcept;
    void CreditProgress(Timestamp now) noexcept;
    [[nodiscard]] bool UpdateInterest() noexcept;
    [[nodiscard]] Deadline NextDeadline() const noexcept;
    void Handle(const io::PollEvent& event, Timestamp now) noexcept;
    void ReadStdin() noexcept;
    void WriteStdout() noexcept;
    void ReadSocket(Timestamp now) noexcept;
    void WriteSocket(Timestamp now) noexcept;
    void Resize() noexcept;
    void Notice(std::string_view text) noexcept;
    [[nodiscard]] Duration Backoff() noexcept;
    [[nodiscard]] int Finish(int status) noexcept;

    const Options& Options_;
    std::optional<Duration> GiveUpAfter_;
    io::AddressList Addresses_;
    io::Poller Poller_;
    std::vector<std::byte> Storage_;
    session::ClientSession Session_;

    ELink Link_{ELink::Waiting};
    Deadline LinkDeadline_{Timestamp{}};
    std::size_t AddressIndex_{0};
    unsigned Attempts_{0};
    io::UniqueFd Socket_;
    std::optional<app::Registration> SocketWatch_;
    std::vector<std::byte> Frames_;
    /// Receive buffer for incoming grid snapshots (§9 path B).  Owned here so
    /// that each ClientConnection has a stable buffer for the lifetime of the
    /// Client.  SNAPSHOT_MAX_BYTES (4 MiB); not used at v1.
    std::vector<std::byte> SnapshotBuf_;
    std::optional<session::ClientConnection> Connection_;
    app::ByteQueue SocketOut_;

    EscapeScanner Escape_;
    /// Room for what the scanner may make of one read: a held ~ and every
    /// byte of it.
    std::array<std::byte, 2 * STDIN_CHUNK + 1> In_{};
    std::size_t InFrom_{0};
    std::size_t InTo_{0};
    bool InEnded_{false};
    app::ByteQueue Stdout_;
    app::Registration StdinWatch_;
    /// Where typed input is read from; tests point it at a pipe.
    int StdinFd_{STDIN_FILENO};
    app::Registration StdoutWatch_;
    std::optional<wire::ResizeMessage> Size_;
    /// stderr is the terminal stdout writes to: a notice then goes in line
    /// with the output, whole, instead of a write that a full, non-blocking
    /// terminal would cut short.
    bool NoticesInline_;

    /// Injected fds for testing; default to the real stdio descriptors.
    int StdoutFd_{STDOUT_FILENO};
    int StderrFd_{STDERR_FILENO};
    /// Protocol ceiling for FinishConnecting (default: v1 production).
    uint16_t Ceiling_{wire::PROTOCOL_VERSION_MAX};
    /// Whether Hello carries GRID_CAPABLE.
    bool GridCapable_{true};

    struct PendingSnapshotInfo {
        std::uint32_t StreamGen{0};
    };
    /// Set when a grid snapshot is queued into Stdout_ but ConfirmApply has not
    /// been sent yet; cleared in WriteStdout once the queue drains. §9 path B.
    std::optional<PendingSnapshotInfo> PendingSnapshot_;

    std::optional<wire::SessionEndMessage> Ended_;
    std::optional<int> Signaled_;
    bool Escaped_{false};
    /// The link went down after it had been live, and the user was told.
    bool Announced_{false};
    /// GridBegin flags of PendingSnapshot_.
    std::uint8_t PendingSnapshotFlags_{0};
    /// When the last live connection was lost; the give-up clock runs from it.
    std::optional<Timestamp> LostSince_;
    /// What the current connection started from, to tell whether it got
    /// anywhere: the backoff restarts only if it did (§9).
    struct ConnectionMark {
        Timestamp Since;
        std::uint64_t Received;
        std::uint64_t Acked;
    };
    std::optional<ConnectionMark> Mark_;
    std::optional<std::string> Fatal_;

    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
};

int Client::Run() noexcept {
    for (const int signo : {SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGQUIT}) {
        if (!Poller_.WatchSignal(signo, SIGNAL_TOKEN)) {
            Fatal_ = "cannot watch signals";
            return Finish(FAILURE_STATUS);
        }
    }
    std::array<io::PollEvent, EVENTS_PER_WAIT> events{};
    for (;;) {
        const Timestamp now = io::MonotonicNow();
        FeedStdin();
        if (Link_ == ELink::Up) {
            Pump(now);
        }
        if (Ended_) {
            int status;
            if (Ended_->Kind == wire::ESessionEndKind::Exited) {
                status = Ended_->Value;
            } else if (Ended_->Kind == wire::ESessionEndKind::Overflow) {
                // §7: ring overflowed, client lacked GRID_CAPABLE.  Print a
                // message then return OVERFLOW_STATUS so scripts can detect it.
                Notice(OVERFLOW_NOTICE);
                status = OVERFLOW_STATUS;
            } else {
                status = SIGNAL_STATUS_BASE + Ended_->Value;
            }
            return Finish(status);
        }
        if (Escaped_) {
            // Not through Finish: ~. is for when the link is dead or the
            // session wedged, and it quits at once, as ssh's does, rather than
            // spend Finish's drain on output that may never be written.
            return ESCAPE_QUIT_STATUS;
        }
        if (Fatal_) {
            return Finish(FAILURE_STATUS);
        }
        if (GiveUpAfter_ && LostSince_ && now - *LostSince_ > *GiveUpAfter_) {
            Fatal_ =
                "the agent has not answered for longer than it keeps a "
                "session; it is gone";
            return Finish(FAILURE_STATUS);
        }
        if (Signaled_) {
            return Finish(SIGNAL_STATUS_BASE + *Signaled_);
        }
        if (Link_ == ELink::Waiting && LinkDeadline_.HasExpired(now)) {
            StartConnecting(now);
        } else if (Link_ == ELink::Connecting &&
                   LinkDeadline_.HasExpired(now)) {
            ++AddressIndex_;
            Drop(now);
        }
        if (!UpdateInterest()) {
            Fatal_ = "the terminal went away";
            return Finish(FAILURE_STATUS);
        }
        const auto ready = Poller_.Wait(events, NextDeadline(), now);
        if (!ready) {
            Fatal_ = "the poller failed";
            return Finish(FAILURE_STATUS);
        }
        const Timestamp woke = io::MonotonicNow();
        for (std::size_t i = 0; i < *ready; ++i) {
            Handle(events[i], woke);
        }
    }
}

void Client::FeedStdin() noexcept {
    while (InFrom_ < InTo_ && Session_.AcceptsWrites()) {
        const auto taken =
            Session_.Write(ByteSpan{In_}.subspan(InFrom_, InTo_ - InFrom_));
        if (!taken || *taken == 0) {
            break;
        }
        InFrom_ += *taken;
    }
    if (InFrom_ == InTo_) {
        InFrom_ = 0;
        InTo_ = 0;
    }
}

void Client::Pump(Timestamp now) noexcept {
    while (Connection_) {
        // An input replay can be larger than the queue (up to DisconnectMax
        // against ResendMax plus the slack): frames go in only while a whole
        // one fits, and the rest goes as WriteSocket drains it. §9.
        Connection_->SetSendGate(SocketOut_.Room() >=
                                 session::CONNECTION_OUT_SIZE);
        auto result = Connection_->Pump(now, MutableByteSpan{Out_},
                                        MutableByteSpan{Scratch_});
        if (!result) {
            const session::SessionError& error = result.error();
            if (error.Disposition() == EDisposition::KillSession) {
                Fatal_ = std::string{"the session is over: "} +
                         std::string{error.Description()};
            } else if (!Connection_->IsLive() &&
                       error.Proto() == EProtoError::AuthenticationFailed) {
                // The answer to our Auth did not verify: an agent that does
                // not know this session gives exactly that (§5 rule two).
                Fatal_ = "the agent does not know this session any more";
            }
            Drop(now);
            return;
        }
        const auto& event = *result;
        if (const auto* frame = std::get_if<session::SendFrame>(&event)) {
            if (!SocketOut_.Append(ByteSpan{Out_}.first(frame->Written))) {
                Drop(now);
                return;
            }
        } else if (const auto* deliver =
                       std::get_if<session::Deliver>(&event)) {
            if (!Stdout_.Append(deliver->Bytes)) {
                Drop(now);
                return;
            }
        } else if (const auto* ended = std::get_if<session::Ended>(&event)) {
            Ended_ = ended->End;
            return;
        } else if (const auto* snap =
                       std::get_if<session::ApplySnapshot>(&event)) {
            // Path B (ver=2, §9): queue the snapshot behind the output already
            // waiting and confirm it once its last byte is written, so stdin,
            // signals and keepalives are served while the terminal drains.
            if (!Stdout_.Append(snap->Bytes, STDOUT_SNAPSHOT_CEILING)) {
                // Unreachable while the read gate and the wire limit hold.
                // Without GridApplied the agent offers it again on reconnect.
                Connection_->DenyApply(snap->StreamGen);
                Drop(now);
                return;
            }
            PendingSnapshot_ =
                PendingSnapshotInfo{.StreamGen = snap->StreamGen};
            PendingSnapshotFlags_ = snap->Flags;
        } else if (std::holds_alternative<session::SessionOverflowed>(event)) {
            // Agent killed the session because the ring overflowed and this
            // client does not have GRID_CAPABLE (ver=2 only, §7). Record it
            // as a distinct end kind so the Run loop exits without retrying.
            Ended_ = {.Kind = wire::ESessionEndKind::Overflow, .Value = 0};
            return;
        } else if (std::holds_alternative<session::Idle>(event)) {
            break;
        }
    }
    if (Connection_ && Connection_->IsLive()) {
        CreditProgress(now);
        LostSince_.reset();
        if (Announced_) {
            Notice(REGAINED_NOTICE);
            Announced_ = false;
        }
    }
}

Duration Client::Backoff() noexcept {
    Duration delay = BACKOFF_MIN;
    for (unsigned i = 0; i < Attempts_ && delay < Options_.BackoffMax; ++i) {
        delay *= 2;
    }
    delay = std::min(delay, Options_.BackoffMax);
    ++Attempts_;
    // Jitter over the upper half, so that clients cut by the same outage do
    // not come back in step.
    std::uint64_t noise = 0;
    crypto::RandomBytes(
        MutableByteSpan{reinterpret_cast<std::byte*>(&noise), sizeof(noise)});
    const auto half = delay.count() / 2;
    return Duration{half + static_cast<Duration::rep>(
                               noise % static_cast<std::uint64_t>(half + 1))};
}

void Client::StartConnecting(Timestamp now) noexcept {
    const auto addresses = Addresses_.View();
    if (AddressIndex_ >= addresses.size()) {
        AddressIndex_ = 0;
    }
    if (addresses.empty()) {
        LinkDeadline_ = Deadline{now + Backoff()};
        return;
    }
    auto socket = io::StartConnect(addresses[AddressIndex_]);
    if (!socket) {
        ++AddressIndex_;
        LinkDeadline_ = Deadline{now + Backoff()};
        return;
    }
    Socket_ = std::move(*socket);
    SocketWatch_.emplace(Socket_.Get(), SOCKET_TOKEN);
    Link_ = ELink::Connecting;
    LinkDeadline_ = Deadline{now + CONNECT_TIMEOUT};
}

void Client::FinishConnecting(Timestamp now) noexcept {
    if (!io::FinishConnect(Socket_.Get())) {
        ++AddressIndex_;
        Drop(now);
        return;
    }
    // Advertise GRID_CAPABLE (unless a test hook says not to) so the agent can
    // choose path B on reconnect (ver=2 only; at v1 the flag is sent but has
    // no effect).
    // SnapshotBuf_ gives the connection a place to reassemble incoming chunks.
    // Allocate lazily: v1 sessions never receive snapshots, so the 4 MiB buffer
    // is only needed when the negotiated ceiling allows v2.
    if (Ceiling_ >= wire::PROTOCOL_VERSION_GRID && SnapshotBuf_.empty()) {
        SnapshotBuf_.resize(wire::SNAPSHOT_MAX_BYTES);
    }
    Connection_.emplace(Session_, RandomNonce<wire::HandshakeNonce>(),
                        MutableByteSpan{Frames_}, now, GridCapable_, Ceiling_,
                        MutableByteSpan{SnapshotBuf_});
    if (Size_) {
        Connection_->QueueResize(*Size_);
    }
    Mark_ = ConnectionMark{.Since = now,
                           .Received = Session_.ReceiveOffset(),
                           .Acked = Session_.AckedOffset()};
    Link_ = ELink::Up;
    LinkDeadline_ = Deadline{};
}

void Client::CreditProgress(Timestamp now) noexcept {
    // Live is not progress: an agent whose pty queue is full lets the
    // handshake through and closes on the first frame it cannot take. The
    // backoff restarts when the agent took our input or the client took its
    // output, or when the connection outlived a keepalive (never with
    // keepalive off: every connection would then count).
    if (!Mark_ || !Connection_ || !Connection_->IsLive()) {
        return;
    }
    const Duration keepalive = Options_.Limits.Liveness.KeepaliveInterval;
    if (Session_.ReceiveOffset() != Mark_->Received ||
        Session_.AckedOffset() != Mark_->Acked ||
        (keepalive > Duration::zero() && now - Mark_->Since >= keepalive)) {
        Attempts_ = 0;
    }
}

void Client::Drop(Timestamp now) noexcept {
    if (Connection_ && Connection_->IsLive() && !Fatal_) {
        if (!Announced_) {
            Notice(LOST_NOTICE);
            Announced_ = true;
        }
    }
    // Progress of the final iteration counts, before the backoff is drawn.
    CreditProgress(now);
    if (!LostSince_) {
        LostSince_ = now;
    }
    // The snapshot can only be confirmed on the connection that carried it.
    PendingSnapshot_.reset();
    if (Connection_) {
        Connection_->Close(now);
        Connection_.reset();
    }
    Mark_.reset();
    if (SocketWatch_) {
        SocketWatch_->Forget(Poller_);
        SocketWatch_.reset();
    }
    Socket_.Reset();
    SocketOut_.Consume(SocketOut_.Size());
    Link_ = ELink::Waiting;
    LinkDeadline_ = Deadline{now + Backoff()};
}

bool Client::UpdateInterest() noexcept {
    const bool readStdin = !InEnded_ && InTo_ == 0 && Session_.AcceptsWrites();
    if (!StdinWatch_.Want(Poller_, readStdin, false) ||
        !StdoutWatch_.Want(Poller_, false, !Stdout_.Empty())) {
        return false;
    }
    if (SocketWatch_) {
        bool read = false;
        bool write = false;
        if (Link_ == ELink::Connecting) {
            write = true;
        } else if (Link_ == ELink::Up && Connection_) {
            // Holding back for a full terminal is no sign of a dead path:
            // the silence timer waits until reading resumes (§9).
            // Handshake frames are tiny and need no room on the terminal;
            // holding them back would only time the handshake out.
            const bool stalled = Connection_->IsLive() &&
                                 Stdout_.Room() < Connection_->ReadCapacity();
            Connection_->SetReadGate(!stalled, io::MonotonicNow());
            read = Connection_->ReadRoom() > 0 && !stalled;
            write = !SocketOut_.Empty();
        }
        if (!SocketWatch_->Want(Poller_, read, write)) {
            Drop(io::MonotonicNow());
        }
    }
    return true;
}

Deadline Client::NextDeadline() const noexcept {
    Deadline next = Link_ == ELink::Up && Connection_
                        ? Connection_->NextDeadline()
                        : LinkDeadline_;
    if (GiveUpAfter_ && LostSince_) {
        next =
            Earliest(next, Deadline{*LostSince_ + *GiveUpAfter_ + Duration{1}});
    }
    return next;
}

void Client::Handle(const io::PollEvent& event, Timestamp now) noexcept {
    switch (event.Token) {
        case STDIN_TOKEN:
            ReadStdin();
            return;
        case STDOUT_TOKEN:
            WriteStdout();
            return;
        case SIGNAL_TOKEN:
            if (event.Signal == SIGWINCH) {
                Resize();
            } else {
                Signaled_ = event.Signal;
            }
            return;
        case SOCKET_TOKEN:
            if (Link_ == ELink::Connecting) {
                FinishConnecting(now);
                return;
            }
            if (Link_ != ELink::Up) {
                return;
            }
            if (event.Writable) {
                WriteSocket(now);
            }
            if (Link_ == ELink::Up &&
                (event.Readable || event.Hangup || event.Error)) {
                ReadSocket(now);
            }
            return;
        default:
            return;
    }
}

void Client::ReadStdin() noexcept {
    if (InTo_ != 0 || InEnded_) {
        return;
    }
    std::array<std::byte, STDIN_CHUNK> typed{};
    const auto n = io::ReadSome(StdinFd_, MutableByteSpan{typed});
    if (!n) {
        if (!n.error().WouldBlock()) {
            InEnded_ = true;
        }
        return;
    }
    if (*n == 0) {
        InEnded_ = true;
        return;
    }
    const auto scanned =
        Escape_.Feed(ByteSpan{typed}.first(*n), MutableByteSpan{In_});
    InTo_ = scanned.Written;
    Escaped_ = scanned.Quit;
}

void Client::WriteStdout() noexcept {
    while (!Stdout_.Empty()) {
        const auto n = io::WriteSome(StdoutFd_, Stdout_.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                Fatal_ = "the terminal went away";
            }
            return;
        }
        Stdout_.Consume(*n);
    }
    // Deferred ConfirmApply: sent only once the snapshot's last byte has been
    // written to the terminal.  §9 path B.
    if (PendingSnapshot_ && Connection_) {
        Connection_->ConfirmApply(PendingSnapshot_->StreamGen);
        Notice(SNAPSHOT_RESTORE_NOTICE);
        if ((PendingSnapshotFlags_ & GRID_FLAG_POSSIBLY_DIVERGED) != 0) {
            Notice(SNAPSHOT_DIVERGED_NOTICE);
        }
        PendingSnapshot_.reset();
    }
}

void Client::ReadSocket(Timestamp now) noexcept {
    std::array<std::byte, SOCKET_READ_CHUNK> chunk{};
    const std::size_t room = std::min(Connection_->ReadRoom(), chunk.size());
    if (room == 0 || (Connection_->IsLive() &&
                      Stdout_.Room() < Connection_->ReadCapacity())) {
        return;
    }
    const auto n =
        io::ReadSome(Socket_.Get(), MutableByteSpan{chunk}.first(room));
    if (!n) {
        if (!n.error().WouldBlock()) {
            Drop(now);
        }
        return;
    }
    if (*n == 0 || !Connection_->OnBytes(ByteSpan{chunk}.first(*n))) {
        Drop(now);
    }
}

void Client::WriteSocket(Timestamp now) noexcept {
    while (!SocketOut_.Empty()) {
        const auto n = io::WriteSome(Socket_.Get(), SocketOut_.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                Drop(now);
            }
            return;
        }
        SocketOut_.Consume(*n);
    }
}

void Client::Notice(std::string_view text) noexcept {
    const ByteSpan bytes{reinterpret_cast<const std::byte*>(text.data()),
                         text.size()};
    if (!NoticesInline_) {
        static_cast<void>(::write(StderrFd_, text.data(), text.size()));
    } else if (!Stdout_.Append(bytes)) {
        // The terminal is far behind; the notice is not worth stalling for.
    }
}

void Client::Resize() noexcept {
    Size_ = TerminalSize();
    if (Size_ && Connection_) {
        Connection_->QueueResize(*Size_);
    }
}

int Client::Finish(int status) noexcept {
    // What the agent wrote last reaches the terminal before zet lets go of it.
    const Timestamp until = io::MonotonicNow() + FLUSH_TIMEOUT;
    while (!Stdout_.Empty() && !Fatal_) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            break;
        }
        pollfd waiting{.fd = StdoutFd_, .events = POLLOUT, .revents = 0};
        if (::poll(&waiting, 1, io::TimeoutMillis(Deadline{until}, now)) > 0) {
            WriteStdout();
        }
    }
    // Closing the connection is how the agent learns it can go (§9).
    const Timestamp now = io::MonotonicNow();
    if (Connection_) {
        Connection_->Close(now);
        Connection_.reset();
    }
    if (SocketWatch_) {
        SocketWatch_->Forget(Poller_);
    }
    Socket_.Reset();
    return status;
}

// Defined inside the anonymous namespace so it can access Client's private
// members (via the friend declaration) and ELink.
SessionTestResult RunSessionForTestImpl(SessionTestConfig config) noexcept {
    auto maybePoller = io::Poller::Create();
    if (!maybePoller) {
        return {.Status = FAILURE_STATUS, .SnapshotBufferBytes = 0};
    }

    Options opts;
    opts.Limits = config.Limits;
    opts.BackoffMax = config.BackoffMax;

    session::BootstrapResult br;
    br.Secrets = std::move(config.Secrets);

    auto clientObj =
        std::make_unique<Client>(opts, std::move(br), config.Addresses,
                                 std::move(*maybePoller), config.GiveUpAfter);

    clientObj->StdoutFd_ = config.StdoutFd;
    clientObj->StderrFd_ = config.StderrFd;
    clientObj->NoticesInline_ = config.NoticesInline;
    clientObj->Ceiling_ = config.Ceiling;
    clientObj->StdoutWatch_ = app::Registration{config.StdoutFd, STDOUT_TOKEN};
    // Without a test stdin the input is ended, so the poller never watches
    // fd 0.
    if (config.StdinFd >= 0) {
        clientObj->StdinFd_ = config.StdinFd;
        clientObj->StdinWatch_ = app::Registration{config.StdinFd, STDIN_TOKEN};
    } else {
        clientObj->InEnded_ = true;
    }
    if (config.InitialInput.size() <= clientObj->In_.size()) {
        std::memcpy(clientObj->In_.data(), config.InitialInput.data(),
                    config.InitialInput.size());
        clientObj->InTo_ = config.InitialInput.size();
    }

    const int status = clientObj->Run();
    return {.Status = status,
            .SnapshotBufferBytes = clientObj->SnapshotBuf_.size()};
}

}  // namespace

// Public entry point; delegates to the anonymous-namespace implementation
// which has friend access to Client's private members.
SessionTestResult RunSessionForTest(SessionTestConfig config) noexcept {
    return RunSessionForTestImpl(std::move(config));
}

int Main(std::span<char*> args) noexcept {
    auto options = ReadOptions(args);
    if (!options) {
        Say(USAGE);
        return USAGE_STATUS;
    }
    if (options->PrintVersion) {
        constexpr std::string_view prefix = "zet ";
        const bool ok = ::write(STDOUT_FILENO, prefix.data(), prefix.size()) ==
                            static_cast<ssize_t>(prefix.size()) &&
                        ::write(STDOUT_FILENO, zet::RELEASE_VERSION.data(),
                                zet::RELEASE_VERSION.size()) ==
                            static_cast<ssize_t>(zet::RELEASE_VERSION.size()) &&
                        ::write(STDOUT_FILENO, "\n", 1) == 1;
        return ok ? 0 : FAILURE_STATUS;
    }
    if (!crypto::Init()) {
        SayError("crypto", "libsodium did not initialise");
        return FAILURE_STATUS;
    }
    static_cast<void>(::signal(SIGPIPE, SIG_IGN));

    // Delivery mode unless --agent was given explicitly.
    // Agent binary lookup order (design.md §4, "Откуда клиент берёт агентов"):
    //   1. ZET_AGENT_DIR if set and non-empty — used as the sole directory.
    //   2. <directory of running executable>/../libexec/zet — beside-binary.
    //   3. Compile-time default ZET_DEFAULT_AGENT_DIR from deliver.hpp.
    // When ZET_AGENT_DIR is not set, (2) is tried first and (3) is the
    // fallback: a tarball unpacked under any prefix works without any env var.
    // An empty ZET_AGENT_DIR is treated the same as unset.
    std::optional<std::string_view> agentDir;
    std::string agentDirBuf;
    std::optional<std::string_view> agentFallbackDir;
    std::string agentFallbackDirBuf;
    if (!options->AgentExplicit) {
        const char* env = ::getenv("ZET_AGENT_DIR");
        if (env != nullptr && env[0] != '\0') {
            // Explicit override — no fallback.
            agentDirBuf = env;
            agentDir = agentDirBuf;
        } else {
            // Beside-binary lookup: find the executable's own directory and
            // go up one level then into libexec/zet.  SelfPath() returns a
            // canonical absolute path (no symlinks, no ..), so we strip two
            // trailing components to get the prefix directory rather than
            // appending "/.." and leaving a non-canonical path in error output.
            if (auto selfPath = io::SelfPath()) {
                const std::string& exe = *selfPath;
                // exe = /prefix/bin/zet  →  exeDir = /prefix/bin
                const auto slash = exe.rfind('/');
                if (slash != std::string::npos && slash > 0) {
                    const std::string exeDir = exe.substr(0, slash);
                    // exeDir = /prefix/bin  →  prefixSlash = position of
                    // /prefix
                    const auto prefixSlash = exeDir.rfind('/');
                    if (prefixSlash != std::string::npos) {
                        const std::string prefix =
                            (prefixSlash == 0) ? std::string{"/"}
                                               : exeDir.substr(0, prefixSlash);
                        agentDirBuf = prefix + "/libexec/zet";
                        agentDir = agentDirBuf;
                    }
                }
            }
            // Always keep the compile-time default as a fallback.  If
            // SelfPath() failed or returned a path without a slash, promote
            // the fallback to primary so delivery still works.
            agentFallbackDirBuf = ZET_DEFAULT_AGENT_DIR;
            if (agentDir) {
                agentFallbackDir = agentFallbackDirBuf;
            } else {
                agentDirBuf = agentFallbackDirBuf;
                agentDir = agentDirBuf;
            }
        }
    }

    auto bootstrapped = Bootstrap(*options, agentDir, agentFallbackDir);
    if (!bootstrapped) {
        return FAILURE_STATUS;
    }
    const auto host = HostName(*options);
    if (!host) {
        SayError("ssh -G", "no hostname for " + options->Host);
        return FAILURE_STATUS;
    }
    const auto addresses = io::Resolve(host->c_str(), bootstrapped->Port);
    if (!addresses || addresses->Count == 0) {
        SayError("resolve " + *host, addresses
                                         ? std::string_view{"no address"}
                                         : addresses.error().Description());
        return FAILURE_STATUS;
    }
    auto poller = io::Poller::Create();
    if (!poller) {
        SayError("poller", poller.error().Description());
        return FAILURE_STATUS;
    }

    std::optional<io::RawMode> raw;
    if (io::IsTerminal(STDIN_FILENO)) {
        auto entered = io::RawMode::Enter(STDIN_FILENO);
        if (!entered) {
            SayError("terminal", entered.error().Description());
            return FAILURE_STATUS;
        }
        raw.emplace(std::move(*entered));
    }
    DescriptorFlags flags;
    if (!io::SetNonBlocking(STDIN_FILENO) ||
        !io::SetNonBlocking(STDOUT_FILENO)) {
        SayError("terminal", "cannot make stdio non-blocking");
        return FAILURE_STATUS;
    }

    // Reconnecting for longer than the agent keeps a session cannot succeed.
    const std::optional<Duration> giveUpAfter =
        bootstrapped->OrphanTimeoutSeconds == 0
            ? std::nullopt
            : std::optional<Duration>{
                  std::chrono::seconds{bootstrapped->OrphanTimeoutSeconds}};
    auto client =
        std::make_unique<Client>(*options, std::move(*bootstrapped), *addresses,
                                 std::move(*poller), giveUpAfter);
    const int status = client->Run();
    const std::optional<std::string> fatal = client->Fatal();
    const bool escaped = client->Escaped();
    client.reset();
    flags.Restore();
    if (raw) {
        static_cast<void>(raw->Restore());
    }
    // Once stdio is blocking again: these must not be lost to EAGAIN.
    if (escaped) {
        Say(ESCAPE_NOTICE);
    } else if (fatal) {
        SayError("session", *fatal);
    }
    return status;
}

}  // namespace zet::client
