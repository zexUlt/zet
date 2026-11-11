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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "deliver.hpp"
#include "escape.hpp"
#include "mirror.hpp"
#include "zet/app/byte_queue.hpp"
#include "zet/app/env.hpp"
#include "zet/app/registration.hpp"
#include "zet/core/shell_quote.hpp"
#include "zet/core/version.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/self_path.hpp"
#include "zet/io/tcp.hpp"
#include "zet/io/terminal.hpp"
#include "zet/predict/calibration.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/overlay.hpp"
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

/// With the overlay on, the client reads the socket only while the stdout
/// queue has room for the output of the read and for taking the overlay off
/// ahead of it, which cannot be left out; an overlay is drawn only if that
/// room stays (design.md §9).
static_assert(STDOUT_QUEUE_MAX >= 4 * predict::OVERLAY_BYTES_MAX);

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
static_assert(STDIN_CHUNK <= predict::Calibration::INPUT_MAX);
/// Room for what the stdin demultiplexer lets through: a read, and the bytes
/// it held back, twice (design.md §9, "Разбор stdin").
constexpr std::size_t RELEASED_MAX =
    predict::Calibration::INPUT_MAX + 2 * predict::CPR_HELD_MAX;
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
/// Printed with --predict=always when the agent answers without ever sending
/// an EchoAck: an old agent, or a protocol ceiling of 1 (§9). Prediction is
/// then off for the session, as with --predict=never.
constexpr std::string_view NO_ECHO_ACK_NOTICE =
    "zet: predictive echo is off: the agent does not support it\r\n";

constexpr std::string_view USAGE =
    "usage: zet [--agent PATH] [--ssh-arg ARG]... HOST\n"
    "  --agent PATH    use PATH as zet-agent on the remote host (disables "
    "delivery)\n"
    "  --ssh-arg ARG   passed to ssh as it is; repeat for several\n"
    "  --predict MODE  predictive echo: adaptive (the default), always or "
    "never;\n"
    "                  --predict=MODE works too\n"
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
    std::string Agent{DEFAULT_AGENT};
    std::vector<std::string> SshArgs;
    std::string Host;
    session::SessionLimits Limits;
    Duration BackoffMax{BACKOFF_MAX};
    /// Test hook ZET_TEST_MIRROR_DUMP: keep the mirror grid (§9, M7) and write
    /// its snapshot to this path on exit. Empty means no mirror: nothing else
    /// asks for one yet, and without it the client does no extra work.
    std::string MirrorDump;
    /// Test hook ZET_TEST_OVERLAY_LOG: one line "offset length" for every
    /// piece of overlay put into stdout, offsets counted in stdout bytes.
    std::string OverlayLog;
    /// Test seam (SessionTestConfig::SizeSource): stands in for the tty when
    /// SIGWINCH asks for the terminal's size. Empty in production.
    std::function<std::optional<wire::ResizeMessage>()> SizeSource;
    /// Test seam (SessionTestConfig::LoseMirrorAtSnapshot): the mirror cannot
    /// be rebuilt from this snapshot (counting from 1), as when its allocation
    /// fails. 0 in production.
    unsigned LoseMirrorAtSnapshot{0};
    /// Test hooks (ZET_TEST_VERSION_CEILING, ZET_TEST_NO_GRID_CAPABLE): the
    /// protocol ceiling offered in Hello, and whether GRID_CAPABLE goes with
    /// it. Production offers the default ceiling and always the TLV.
    std::uint16_t Ceiling{wire::PROTOCOL_VERSION_MAX};
    /// Predictive echo (--predict, §9). Never offers no ECHO_ACK and keeps no
    /// mirror: the stream reaches the terminal as it always did.
    predict::EDisplayMode Predict{predict::EDisplayMode::Adaptive};
    bool PrintVersion{false};
    /// True when --agent was given: skip delivery and run the agent directly.
    bool AgentExplicit{false};
    bool GridCapable{true};
    /// Ask the terminal where its cursor is (design.md §9, round C). On when
    /// stdin and stdout are a terminal: a pipe cannot answer, and the query
    /// would be written into a file. Off for the tests that read the client's
    /// stdout from a pipe.
    bool Calibrate{false};
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

std::optional<predict::EDisplayMode> ParsePredict(
    std::string_view value) noexcept {
    if (value == "adaptive") return predict::EDisplayMode::Adaptive;
    if (value == "always") return predict::EDisplayMode::Always;
    if (value == "never") return predict::EDisplayMode::Never;
    return std::nullopt;
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
        } else if (arg.starts_with("--predict=") ||
                   (arg == "--predict" && i + 1 < args.size())) {
            const std::string_view value =
                arg == "--predict"
                    ? std::string_view{args[++i]}
                    : arg.substr(std::string_view{"--predict="}.size());
            const auto mode = ParsePredict(value);
            if (!mode) {
                SayError("--predict", "unknown mode '" + SanitizeAscii(value) +
                                          "' (adaptive, always or never)");
                return std::nullopt;
            }
            options.Predict = *mode;
        } else if (arg == "--predict") {
            SayError("--predict", "needs a mode (adaptive, always or never)");
            return std::nullopt;
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
    if (const char* dump = ::getenv("ZET_TEST_MIRROR_DUMP");
        dump != nullptr && dump[0] != '\0') {
        options.MirrorDump = dump;
    }
    if (const char* log = ::getenv("ZET_TEST_OVERLAY_LOG");
        log != nullptr && log[0] != '\0') {
        options.OverlayLog = log;
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
          GridCapable_(options.GridCapable) {
        if (!options.MirrorDump.empty()) {
            StartMirror();
        }
    }

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
    void StartMirror() noexcept;
    void StartPredict(std::uint32_t streamGen, Timestamp now) noexcept;
    void OnEchoAcked(const wire::EchoAckMessage& ack, Timestamp now) noexcept;
    void Tick() noexcept;
    void PumpEvents(Timestamp now) noexcept;
    /// Takes the overlay off the terminal: queues the restore. False when it
    /// did not fit (the caller drops the connection).
    [[nodiscard]] bool TakeOffOverlay(
        std::size_t ceiling = STDOUT_QUEUE_MAX) noexcept;
    /// Puts the engine's visible predictions on the terminal, if the queue
    /// has room.
    void RefreshOverlay() noexcept;
    void LogOverlay(std::size_t length) noexcept;
    void LoseMirror() noexcept;
    [[nodiscard]] std::size_t StdoutReserve() const noexcept {
        return Overlay_ ? predict::OVERLAY_UNDO_BYTES_MAX : 0;
    }
    [[nodiscard]] predict::MirrorView View() const noexcept {
        return predict::MirrorView{Mirror_->Screen(), Mirror_->Parser()};
    }
    void WriteMirrorDump() noexcept;
    /// The mirror, the engine and the grid they watch as the calibration
    /// takes them; empty when there is no mirror.
    [[nodiscard]] predict::CalibrationTarget CalTarget() noexcept {
        if (!Mirror_ || !Predict_) {
            return {};
        }
        return predict::CalibrationTarget{&Mirror_->MutableScreen(),
                                          &Mirror_->MutableParser(),
                                          Predict_.get()};
    }
    /// Asks the terminal for the cursor position if a reason is pending and
    /// the queue has room (design.md §9, round C).
    void MaybeQuery(Timestamp now) noexcept;
    /// Bytes the stdout queue wrote just now: the demultiplexer counts them,
    /// and the query's last byte starts the answer's clock.
    void NoteWritten(ByteSpan written) noexcept;
    /// What the stdin demultiplexer let through goes to Released_, and from
    /// there to the escape scanner when In_ has room.
    void StageForwarded() noexcept;
    void MoveReleased() noexcept;
    [[nodiscard]] Duration Backoff() noexcept;
    [[nodiscard]] int Finish(int status) noexcept;

    const Options& Options_;
    std::optional<Duration> GiveUpAfter_;
    io::AddressList Addresses_;
    io::Poller Poller_;
    std::vector<std::byte> Storage_;
    session::ClientSession Session_;

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
    /// Typed bytes the demultiplexer has let through, waiting for In_. Empty
    /// without calibration.
    std::array<std::byte, RELEASED_MAX> Released_{};
    std::size_t ReleasedLen_{0};
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
    ELink Link_{ELink::Waiting};
    bool InEnded_{false};
    /// The first EchoAck of a connection only sets the baseline.
    bool EchoBaseline_{false};
    /// An EchoAck has come in this session, or Data has come first: the
    /// agent's answer to ECHO_ACK is known either way (§9).
    bool EchoDecided_{false};
    /// The mirror was started by a snapshot, before the first EchoAck: it
    /// shows the terminal exactly, the column included.
    bool SeededFromSnapshot_{false};
    bool Escaped_{false};
    /// The link went down after it had been live, and the user was told.
    bool Announced_{false};
    /// The loop must not wait: input was moved to In_ outside a read.
    bool Wake_{false};
    bool CalibrationCreated_{false};
    std::uint32_t QueriesWritten_{0};
    /// GridBegin flags of PendingSnapshot_.
    std::uint8_t PendingSnapshotFlags_{0};

    /// What the local terminal has been sent, as a grid; null when nothing
    /// wants it or it could not be kept in step (§9, M7).
    std::unique_ptr<Mirror> Mirror_;
    /// Predictive echo (§9): created with the first EchoAck of the session,
    /// never with --predict=never, an old agent or ver=1.
    std::unique_ptr<predict::PredictionEngine> Predict_;
    std::unique_ptr<predict::Overlay> Overlay_;
    /// Input bytes the session has taken: the input offset the agent counts.
    std::uint64_t InputWritten_{0};
    /// The last SRTT given to the engine.
    Duration LastSrtt_{Duration::min()};  // min: none yet
    /// Bytes ever queued for stdout, for ZET_TEST_OVERLAY_LOG offsets.
    std::uint64_t StdoutQueued_{0};
    /// Bytes written to the terminal, on the same scale.
    std::uint64_t StdoutWritten_{0};
    /// Where the cursor position query stands in the same offsets.
    std::uint64_t QueryAt_{0};
    /// Cursor position calibration: created with the predictive echo when
    /// Options::Calibrate is on, never with --predict=never, an old agent or
    /// ver=1.
    std::unique_ptr<predict::Calibration> Cal_;
    /// Snapshots applied to the mirror (for Options::LoseMirrorAtSnapshot).
    unsigned SnapshotsApplied_{0};

    struct PendingSnapshotInfo {
        std::uint32_t StreamGen{0};
    };
    /// Set when a grid snapshot is queued into Stdout_ but ConfirmApply has not
    /// been sent yet; cleared in WriteStdout once the queue drains. §9 path B.
    std::optional<PendingSnapshotInfo> PendingSnapshot_;

    std::optional<wire::SessionEndMessage> Ended_;
    std::optional<int> Signaled_;
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
        Wake_ = false;
        if (Link_ == ELink::Up) {
            Pump(now);
            // An Ack taken by Pump may have freed ring room that the input
            // left in In_ can now use. Stdin is not watched while In_ holds
            // bytes, so no event would wake the loop to feed it: feed it now,
            // and take another turn only if it did move, since the bytes fed
            // need a Pump. (The remaining count, not InFrom_: a feed that
            // drains In_ resets both offsets.)
            if (InFrom_ != InTo_) {
                const std::size_t pending = InTo_ - InFrom_;
                FeedStdin();
                if (InTo_ - InFrom_ != pending) {
                    continue;
                }
            }
        }
        Tick();
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
    for (;;) {
        while (InFrom_ < InTo_ && Session_.AcceptsWrites()) {
            const auto taken =
                Session_.Write(ByteSpan{In_}.subspan(InFrom_, InTo_ - InFrom_));
            if (!taken || *taken == 0) {
                break;
            }
            const ByteSpan sent = ByteSpan{In_}.subspan(InFrom_, *taken);
            InFrom_ += *taken;
            InputWritten_ += *taken;
            if (Predict_ && Mirror_) {
                Predict_->OnInput(sent, InputWritten_, io::MonotonicNow(),
                                  View());
                RefreshOverlay();
            }
        }
        if (InFrom_ != InTo_) {
            return;
        }
        InFrom_ = 0;
        InTo_ = 0;
        // What the demultiplexer let through while In_ was full.
        MoveReleased();
        if (InTo_ == 0) {
            return;
        }
    }
}

void Client::Pump(Timestamp now) noexcept {
    PumpEvents(now);
    // The query goes out before the draw, so that the overlay is not drawn to
    // be taken off at once.
    MaybeQuery(now);
    // One draw for everything the pass delivered: the overlay was taken off
    // before the first byte and goes back on after the last.
    RefreshOverlay();
}

void Client::PumpEvents(Timestamp now) noexcept {
    while (Connection_) {
        // An input replay can be larger than the queue (up to DisconnectMax
        // against ResendMax plus the slack): frames go in only while a whole
        // one fits, and the rest goes as WriteSocket drains it. §9.
        Connection_->SetSendGate(session::HasSendRoom(SocketOut_.Room()));
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
            if (!EchoDecided_) {
                // The agent sends its first EchoAck before any Data; Data
                // first means it does not (a 0.3 agent, or ver=1), and the
                // session goes on as with never.
                EchoDecided_ = true;
                if (!Predict_ && Options_.MirrorDump.empty()) {
                    Mirror_
                        .reset();  // started for an EchoAck that did not come
                    SeededFromSnapshot_ = false;
                }
                if (Options_.Predict == predict::EDisplayMode::Always) {
                    static_cast<void>(::write(StderrFd_,
                                              NO_ECHO_ACK_NOTICE.data(),
                                              NO_ECHO_ACK_NOTICE.size()));
                }
            }
            if (!TakeOffOverlay() || !Stdout_.Append(deliver->Bytes)) {
                Drop(now);
                return;
            }
            StdoutQueued_ += deliver->Bytes.size();
            if (Mirror_) {
                if (Cal_ && Predict_) {
                    Cal_->OnTerminalBytes(deliver->Bytes,
                                          Session_.ReceiveOffset(), now,
                                          CalTarget());
                } else {
                    Mirror_->Feed(deliver->Bytes);
                    if (Predict_) {
                        Predict_->OnOutput(Session_.ReceiveOffset(), now,
                                           View());
                    }
                }
            }
        } else if (const auto* ended = std::get_if<session::Ended>(&event)) {
            Ended_ = ended->End;
            return;
        } else if (const auto* snap =
                       std::get_if<session::ApplySnapshot>(&event)) {
            // Path B (ver=2, §9): queue the snapshot behind the output already
            // waiting and confirm it once its last byte is written, so stdin,
            // signals and keepalives are served while the terminal drains.
            if (!TakeOffOverlay(STDOUT_SNAPSHOT_CEILING) ||
                !Stdout_.Append(snap->Bytes, STDOUT_SNAPSHOT_CEILING)) {
                // Unreachable while the read gate and the wire limit hold.
                // Without GridApplied the agent offers it again on reconnect.
                Connection_->DenyApply(snap->StreamGen);
                Drop(now);
                return;
            }
            StdoutQueued_ += snap->Bytes.size();
            if (!Mirror_ && !EchoDecided_ &&
                Options_.Predict != predict::EDisplayMode::Never) {
                // The first connection of the session went by path B, and the
                // first EchoAck, which starts the mirror, comes after the
                // snapshot: a mirror started then would not know what the
                // snapshot drew. Start it here, from the snapshot.
                StartMirror();
                SeededFromSnapshot_ = Mirror_ != nullptr;
            }
            if (Mirror_) {
                if (++SnapshotsApplied_ == Options_.LoseMirrorAtSnapshot ||
                    !Mirror_->ResetFromSnapshot(snap->Bytes)) {
                    LoseMirror();
                } else if (Predict_) {
                    // C4: ResetFromSnapshot built a fresh Grid, so the width
                    // policy the solver settled on is gone with the old one;
                    // C4 must apply it to the new grid here.
                    // The held output and the query's answer go with the old
                    // grid; no query is asked for the snapshot's sake.
                    if (Cal_) {
                        Cal_->OnSnapshot(now, CalTarget());
                    }
                    // The mirror is a new grid; the engine rebases on it.
                    Predict_->OnSnapshot(snap->StreamGen, now, View());
                }
            }
            PendingSnapshot_ =
                PendingSnapshotInfo{.StreamGen = snap->StreamGen};
            PendingSnapshotFlags_ = snap->Flags;
        } else if (const auto* acked =
                       std::get_if<session::EchoAcked>(&event)) {
            OnEchoAcked(acked->Ack, now);
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
                        MutableByteSpan{SnapshotBuf_},
                        Options_.Predict != predict::EDisplayMode::Never);
    EchoBaseline_ = false;
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
    const bool readStdin = !InEnded_ && InTo_ == 0 && ReleasedLen_ == 0 &&
                           Session_.AcceptsWrites();
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
            const bool stalled =
                Connection_->IsLive() &&
                Stdout_.Room() < Connection_->ReadCapacity() + StdoutReserve();
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
    if (Predict_) {
        if (const auto glitch = Predict_->NextDeadline()) {
            next = Earliest(next, Deadline{*glitch});
        }
    }
    if (Cal_) {
        if (const auto cpr = Cal_->NextDeadline()) {
            next = Earliest(next, Deadline{*cpr});
        }
    }
    if (Wake_) {
        next = Earliest(next, Deadline{Timestamp{}});
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
    if (InTo_ != 0 || ReleasedLen_ != 0 || InEnded_) {
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
    if (Cal_) {
        // stdin, the demultiplexer, the `~.` scanner, the agent (design.md §9).
        Cal_->OnInput(ByteSpan{typed}.first(*n), io::MonotonicNow(),
                      CalTarget());
        StageForwarded();
        MoveReleased();
        return;
    }
    const auto scanned =
        Escape_.Feed(ByteSpan{typed}.first(*n), MutableByteSpan{In_});
    InTo_ = scanned.Written;
    Escaped_ = scanned.Quit;
}

void Client::StageForwarded() noexcept {
    if (!Cal_) {
        return;
    }
    const ByteSpan forwarded = Cal_->Forwarded();
    // Bounded by the contract: a read is released with at most the bytes held
    // back, and nothing is read while Released_ holds anything.
    const std::size_t n =
        std::min(forwarded.size(), Released_.size() - ReleasedLen_);
    if (n != 0) {
        std::memcpy(Released_.data() + ReleasedLen_, forwarded.data(), n);
        ReleasedLen_ += n;
    }
    Cal_->ClearForwarded();
}

void Client::MoveReleased() noexcept {
    while (ReleasedLen_ != 0 && InTo_ == 0) {
        const std::size_t n = std::min(ReleasedLen_, STDIN_CHUNK);
        const auto scanned =
            Escape_.Feed(ByteSpan{Released_}.first(n), MutableByteSpan{In_});
        InTo_ = scanned.Written;
        if (scanned.Quit) {
            Escaped_ = true;
        }
        std::memmove(Released_.data(), Released_.data() + n, ReleasedLen_ - n);
        ReleasedLen_ -= n;
        if (InTo_ != 0) {
            Wake_ = true;
        }
    }
}

void Client::WriteStdout() noexcept {
    while (!Stdout_.Empty()) {
        const ByteSpan front = Stdout_.Front();
        const auto n = io::WriteSome(StdoutFd_, front);
        if (!n) {
            if (!n.error().WouldBlock()) {
                Fatal_ = "the terminal went away";
            }
            return;
        }
        if (Cal_) {
            NoteWritten(front.first(*n));
        } else {
            StdoutWritten_ += *n;
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
    if (room == 0 ||
        (Connection_->IsLive() &&
         Stdout_.Room() < Connection_->ReadCapacity() + StdoutReserve())) {
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
    } else if (!TakeOffOverlay() || !Stdout_.Append(bytes)) {
        // The terminal is far behind; the notice is not worth stalling for.
    } else {
        StdoutQueued_ += bytes.size();
        if (Mirror_) {
            if (Cal_ && Predict_) {
                // A notice gives no reason to ask.
                Cal_->OnTerminalBytes(bytes, Session_.ReceiveOffset(),
                                      io::MonotonicNow(), CalTarget(), true);
            } else {
                Mirror_->Feed(bytes);
                if (Predict_) {
                    // Output nobody predicted, at the offset the stream is at.
                    Predict_->OnOutput(Session_.ReceiveOffset(),
                                       io::MonotonicNow(), View());
                }
            }
        }
        RefreshOverlay();
    }
}

void Client::StartPredict(std::uint32_t streamGen, Timestamp now) noexcept {
    if (!Mirror_) {
        // The output before this point is not in it: the column is unknown
        // until the stream sets it, and no prediction is made before (§9).
        StartMirror();
    }
    if (!Mirror_) {
        return;
    }
    auto overlay = predict::Overlay::Create();
    if (!overlay) {
        return;
    }
    Overlay_ = std::move(overlay);
    Predict_ = std::make_unique<predict::PredictionEngine>(
        View(), Options_.Predict, streamGen);
    const bool seeded = std::exchange(SeededFromSnapshot_, false);
    if (Options_.Calibrate &&
        Options_.Predict != predict::EDisplayMode::Never) {
        // The first EchoAck is the start of the session: the mirror begins
        // empty on a terminal that shows the local shell, unless a snapshot
        // seeded it. A failed allocation leaves the session as with never.
        Cal_ = predict::Calibration::Create(seeded, CalTarget());
        CalibrationCreated_ = Cal_ != nullptr;
    }
    if (seeded) {
        // The mirror was filled from a snapshot and fed everything since: the
        // cursor is known as it is after a snapshot.
        Predict_->OnSnapshot(streamGen, now, View());
    }
    Predict_->OnOutput(Session_.ReceiveOffset(), now, View());
    MaybeQuery(now);
}

void Client::OnEchoAcked(const wire::EchoAckMessage& ack,
                         Timestamp now) noexcept {
    // The first EchoAck of a connection is a baseline: it says what the agent
    // had read when it began, not what the user has typed since.
    if (!EchoBaseline_) {
        EchoBaseline_ = true;
        if (!EchoDecided_) {
            EchoDecided_ = true;
            StartPredict(ack.StreamGen, now);
        } else if (Predict_ && Mirror_) {
            // A new connection: what was predicted for input the agent had
            // before it began will get no EchoAck of its own.
            Predict_->OnBaseline(
                predict::EchoAck{.StreamGen = ack.StreamGen,
                                 .OutputOffset = ack.OutputOffset,
                                 .InputOffset = ack.InputOffset},
                now, View());
        }
        return;
    }
    if (Predict_ && Mirror_) {
        const predict::EchoAck echo{.StreamGen = ack.StreamGen,
                                    .OutputOffset = ack.OutputOffset,
                                    .InputOffset = ack.InputOffset};
        if (Cal_) {
            Cal_->OnEchoAck(echo, now, CalTarget());
        } else {
            Predict_->OnEchoAck(echo, now, View());
        }
    }
}

void Client::Tick() noexcept {
    const Timestamp now = io::MonotonicNow();
    if (Cal_) {
        Cal_->OnTick(now, CalTarget());
        StageForwarded();
        MoveReleased();
    }
    if (!Predict_ || !Mirror_) {
        return;
    }
    if (Connection_) {
        const auto srtt = Connection_->Srtt();
        if (srtt && *srtt != LastSrtt_) {
            LastSrtt_ = *srtt;
            Predict_->OnSrtt(*srtt, now);
        }
    }
    Predict_->OnTick(now);
    MaybeQuery(now);
    RefreshOverlay();
}

void Client::MaybeQuery(Timestamp now) noexcept {
    if (!Cal_ || !Predict_ || !Mirror_) {
        return;
    }
    const predict::CalibrationTarget target = CalTarget();
    const bool due = Cal_->QueryDue(now, target);
    // Settling may have let held bytes go.
    StageForwarded();
    MoveReleased();
    if (!due) {
        return;
    }
    // The overlay's undo, the query, and what the read gate keeps free.
    const std::size_t readRoom =
        Connection_ ? Connection_->ReadCapacity() : std::size_t{0};
    if (Stdout_.Room() <
        predict::Calibration::QUERY_SIZE + readRoom + 2 * StdoutReserve()) {
        return;
    }
    if (!Cal_->BeginQuery(now, target)) {
        return;
    }
    const ByteSpan query = predict::Calibration::Query();
    if (!TakeOffOverlay() || !Stdout_.Append(query)) {
        Cal_->AbortQuery(now, target);
        return;
    }
    LogOverlay(query.size());
    QueryAt_ = StdoutQueued_;
    StdoutQueued_ += query.size();
}

void Client::NoteWritten(ByteSpan written) noexcept {
    const Timestamp now = io::MonotonicNow();
    const std::uint64_t from = StdoutWritten_;
    const std::uint64_t to = from + written.size();
    StdoutWritten_ = to;
    if (!Cal_->QueryUnwritten()) {
        Cal_->OnWritten(written, now);
        return;
    }
    // The demultiplexer counts the agent's bytes, in stdout order; the query
    // itself is not one of them.
    const std::uint64_t queryEnd = QueryAt_ + predict::Calibration::QUERY_SIZE;
    if (from < QueryAt_) {
        Cal_->OnWritten(written.first(static_cast<std::size_t>(
                            std::min(to, QueryAt_) - from)),
                        now);
    }
    if (to >= queryEnd) {
        ++QueriesWritten_;
        Cal_->OnQueryWritten(now, CalTarget());
        StageForwarded();
        MoveReleased();
        if (to > queryEnd) {
            Cal_->OnWritten(
                written.subspan(static_cast<std::size_t>(queryEnd - from)),
                now);
        }
    }
}

bool Client::TakeOffOverlay(std::size_t ceiling) noexcept {
    if (!Overlay_ || !Overlay_->Drawn() || !Mirror_ || !Predict_) {
        return true;
    }
    const ByteSpan bytes = Overlay_->BuildUndo(View());
    if (bytes.empty()) {
        return true;
    }
    if (!Stdout_.Append(bytes, ceiling)) {
        return false;
    }
    LogOverlay(bytes.size());
    StdoutQueued_ += bytes.size();
    Overlay_->Commit(*Predict_);
    return true;
}

void Client::RefreshOverlay() noexcept {
    if (!Overlay_ || !Predict_ || !Mirror_) {
        return;
    }
    // The mirror waits at the query's point: it is not the terminal's screen,
    // and nothing is drawn on it.
    if (Cal_ && Cal_->Frozen()) {
        return;
    }
    const ByteSpan bytes = Overlay_->BuildUpdate(*Predict_, View());
    if (bytes.empty()) {
        return;
    }
    const std::size_t readRoom =
        Connection_ ? Connection_->ReadCapacity() : std::size_t{0};
    if (Stdout_.Room() < bytes.size() + readRoom + StdoutReserve() ||
        !Stdout_.Append(bytes)) {
        // No room: the predictions live on, undrawn.
        return;
    }
    LogOverlay(bytes.size());
    StdoutQueued_ += bytes.size();
    Overlay_->Commit(*Predict_);
}

void Client::LogOverlay(std::size_t length) noexcept {
    if (Options_.OverlayLog.empty()) {
        return;
    }
    const int fd = ::open(Options_.OverlayLog.c_str(),
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return;
    }
    const std::string line =
        std::to_string(StdoutQueued_) + " " + std::to_string(length) + "\n";
    static_cast<void>(::write(fd, line.data(), line.size()));
    ::close(fd);
}

void Client::LoseMirror() noexcept {
    // Without the mirror nothing can be taken off; what is drawn stays.
    if (Overlay_) {
        Overlay_->Forget();
    }
    if (Cal_) {
        // The late answer is still stripped from stdin.
        Cal_->Detach(io::MonotonicNow());
    }
    Predict_.reset();
    Overlay_.reset();
    Mirror_.reset();
    SeededFromSnapshot_ = false;
}

void Client::StartMirror() noexcept {
    // Before the terminal's size is known the mirror starts at the usual
    // 80x24; a later Resize sets it right.
    const auto cols = Size_ ? Size_->Cols : std::uint16_t{80};
    const auto rows = Size_ ? Size_->Rows : std::uint16_t{24};
    Mirror_ = Mirror::Create(cols, rows);
}

void Client::WriteMirrorDump() noexcept {
    if (!Mirror_ || Options_.MirrorDump.empty()) {
        return;
    }
    auto snapshot = grid::Snapshot::Create(Mirror_->Screen());
    if (!snapshot) {
        return;
    }
    const std::unique_ptr<grid::Snapshot> owned(*snapshot);
    const int fd =
        ::open(Options_.MirrorDump.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return;
    }
    ByteSpan rest = owned->Bytes();
    while (!rest.empty()) {
        const auto n = ::write(fd, rest.data(), rest.size());
        if (n <= 0) {
            break;
        }
        rest = rest.subspan(static_cast<std::size_t>(n));
    }
    ::close(fd);
}

void Client::Resize() noexcept {
    Size_ = Options_.SizeSource ? Options_.SizeSource() : TerminalSize();
    if (Size_ && Connection_) {
        Connection_->QueueResize(*Size_);
    }
    if (Size_ && Mirror_) {
        // Taken off by the grid the overlay was drawn on, before the resize,
        // and only where the terminal's reflow cannot have moved what is
        // restored: otherwise the glyphs stay, as the program's redraw (it
        // gets the same SIGWINCH) will repaint them, and no byte is written.
        if (Overlay_ && Predict_ && Overlay_->Drawn() &&
            !Overlay_->UndoSurvivesResize(View(), Size_->Cols)) {
            Overlay_->Forget();
            Predict_->ClearDrawn();
        } else if (!TakeOffOverlay()) {
            LoseMirror();
            return;
        }
        // The wait ends before the grid changes size: the held output goes in
        // at the old size.
        if (Cal_ && Predict_) {
            Cal_->OnResize(io::MonotonicNow(), CalTarget());
            StageForwarded();
            MoveReleased();
        }
        if (!Mirror_->Resize(Size_->Cols, Size_->Rows)) {
            LoseMirror();
        } else if (Predict_) {
            Predict_->OnResize(io::MonotonicNow(), View());
        }
    }
}

int Client::Finish(int status) noexcept {
    const bool wasDrawn = Overlay_ && Overlay_->Drawn();
    const bool undone = TakeOffOverlay(STDOUT_SNAPSHOT_CEILING) && wasDrawn;
    if (Fatal_ && undone) {
        // The loop below leaves the queue alone after a failure, but the
        // predicted glyphs must not be left on the user's screen: one try,
        // without waiting. Only when there is an undo to get out: with no
        // overlay a failure leaves the queue as it always did.
        while (!Stdout_.Empty()) {
            const auto n = io::WriteSome(StdoutFd_, Stdout_.Front());
            if (!n) {
                break;
            }
            Stdout_.Consume(*n);
        }
    }
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
    WriteMirrorDump();
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
        return {.Status = FAILURE_STATUS,
                .SnapshotBufferBytes = 0,
                .CalibrationCreated = false,
                .QueriesWritten = 0};
    }

    Options opts;
    opts.Limits = config.Limits;
    opts.BackoffMax = config.BackoffMax;
    opts.MirrorDump = config.MirrorDump;
    opts.Predict = config.Predict;
    opts.OverlayLog = config.OverlayLog;
    opts.Calibrate = config.Calibrate;
    opts.SizeSource = std::move(config.SizeSource);
    opts.LoseMirrorAtSnapshot = config.LoseMirrorAtSnapshot;

    session::BootstrapResult br;
    br.Secrets = std::move(config.Secrets);

    auto clientObj =
        std::make_unique<Client>(opts, std::move(br), config.Addresses,
                                 std::move(*maybePoller), config.GiveUpAfter);

    if (config.InitialSize) {
        clientObj->Size_ = config.InitialSize;
    }
    if (!opts.MirrorDump.empty()) {
        clientObj->StartMirror();
    }
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
            .SnapshotBufferBytes = clientObj->SnapshotBuf_.size(),
            .CalibrationCreated = clientObj->CalibrationCreated_,
            .QueriesWritten = clientObj->QueriesWritten_};
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
    options->Calibrate =
        io::IsTerminal(STDIN_FILENO) && io::IsTerminal(STDOUT_FILENO);
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
