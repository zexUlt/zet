#include "agent.hpp"

#include "mux_link.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
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

#include "zet/app/byte_queue.hpp"
#include "zet/app/env.hpp"
#include "zet/app/fd_sink.hpp"
#include "zet/app/registration.hpp"
#include "zet/core/log.hpp"
#include "zet/core/version.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/daemon.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/spawn.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/bootstrap.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"

extern char** environ;

namespace zet::agent {

namespace {

using namespace std::chrono_literals;

/// How long the Offer may take to arrive on stdin. The client writes it as
/// soon as ssh is up, so this only ends an agent nobody talks to.
constexpr Duration BOOTSTRAP_TIMEOUT = 60s;

/// One read off the pty or a socket.
constexpr std::size_t PTY_READ_CHUNK = 64 * 1024;
constexpr std::size_t SOCKET_READ_CHUNK = 64 * 1024;

/// Keystrokes the pty has not taken yet. A socket is read only while this has
/// room for everything that one read could deliver — a whole frame of the
/// live stage — so it never overflows. Two frames, so that one can wait for
/// the pty while the next is read.
constexpr std::size_t PTY_INPUT_MAX = 2 * session::CONNECTION_FRAME_STORAGE;

// Smaller than one frame, and the gate on reading the socket never opens once
// the connection is live: the stall the first version of this file had.
static_assert(PTY_INPUT_MAX >= session::CONNECTION_FRAME_STORAGE);

/// Sealed frames a socket has not taken yet, on top of the resend window.
/// The channel never has more than the window in flight, so a working
/// connection never comes near it; a connection that does is dropped.
constexpr std::size_t SOCKET_OUTPUT_SLACK = 1024 * 1024;

/// Connections at once: the live one, the one replacing it, a few strangers.
constexpr std::size_t MAX_CONNECTIONS = 8;

/// After SIGHUP, how long the shell has before SIGKILL.
constexpr Duration HANGUP_GRACE = 5s;

/// The size the pty starts at, until the client's first Resize arrives.
constexpr io::WindowSize INITIAL_SIZE{.Rows = 24, .Cols = 80};

/// TERM for the shell when the client sent none.
constexpr std::string_view DEFAULT_TERM = "xterm-256color";

constexpr std::size_t EVENTS_PER_WAIT = 32;

constexpr std::uint64_t LISTENER_TOKEN = 1;
constexpr std::uint64_t PTY_TOKEN = 2;
constexpr std::uint64_t SIGNAL_TOKEN = 3;
constexpr std::uint64_t MUX_TOKEN = 4;
constexpr std::uint64_t FIRST_CONNECTION_TOKEN = 16;

constexpr int USAGE_STATUS = 2;
constexpr int FAILURE_STATUS = 1;

constexpr std::string_view USAGE =
    "usage: zet-agent --bootstrap\n"
    "Started by zet over ssh; not meant to be run by hand.\n"
    "  --version       print the release version and exit\n";

void Say(std::string_view text) noexcept {
    static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

void SayError(std::string_view what, std::string_view why) noexcept {
    Say("zet-agent: ");
    Say(what);
    Say(": ");
    Say(why);
    Say("\n");
}

void Log(log::ELevel level, std::string_view message,
         std::span<const log::Field> fields = {}) noexcept {
    log::Emit(level, message, fields, io::MonotonicNow());
}

struct Config {
    bool PrintVersion{false};
    session::SessionLimits Limits;
};

/// `--bootstrap`, and the environment of e2e.py.
std::optional<Config> ReadConfig(std::span<char*> args) {
    Config config;
    bool bootstrap = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--bootstrap") {
            bootstrap = true;
        } else if (arg == "--version") {
            config.PrintVersion = true;
            return config;
        } else {
            return std::nullopt;
        }
    }
    if (!bootstrap) {
        return std::nullopt;
    }
    using app::EUnit;
    if (auto keepalive = app::DurationFromEnv("ZET_KEEPALIVE_INTERVAL",
                                              EUnit::Milliseconds)) {
        config.Limits.Liveness.KeepaliveInterval = *keepalive;
    }
    if (auto silence = app::DurationFromEnv("ZET_PEER_SILENCE_TIMEOUT",
                                            EUnit::Milliseconds)) {
        config.Limits.Liveness.PeerSilenceTimeout = *silence;
    }
    if (auto orphan =
            app::DurationFromEnv("ZET_ORPHAN_TIMEOUT", EUnit::Seconds)) {
        config.Limits.OrphanTimeout = *orphan;
    }
    return config;
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

/// Writes all of `bytes` to a blocking descriptor: stdout to ssh, before the
/// loop exists.
bool WriteAll(int fd, ByteSpan bytes) noexcept {
    while (!bytes.empty()) {
        const auto n = io::WriteSome(fd, bytes);
        if (!n) {
            return false;
        }
        bytes = bytes.subspan(*n);
    }
    return true;
}

struct Bootstrapped {
    session::BootstrapResult Result;
    /// Ours, as in M4; or none, and the mux's port in the Accept (§6).
    std::optional<io::Listener> Listener;
    std::unique_ptr<MuxLink> Mux;
};

/// The §4 exchange on stdin and stdout. Without a mux the port is bound
/// first so that the Accept can name it; with one it is the mux's, learned
/// by registering the R the Offer gives — before the Accept goes out, so
/// that the client's first connect finds the sid registered (§6).
std::optional<Bootstrapped> Bootstrap(const Config& config) noexcept {
    const std::string muxPath = MuxSocketPath();
    std::optional<io::Listener> listener;
    if (!MuxSocketExists(muxPath)) {
        auto bound = io::ListenTcp();
        if (!bound) {
            SayError("listen", bound.error().Description());
            return std::nullopt;
        }
        listener = std::move(*bound);
    }
    // The client learns how long we keep a session without it, which is how
    // long its reconnecting can make sense.
    const auto orphanSeconds = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            config.Limits.OrphanTimeout)
            .count());
    session::BootstrapAgent bootstrap{
        RandomKey(), RandomNonce<wire::BootstrapNonce>(),
        listener ? listener->Port : std::uint16_t{0}, orphanSeconds};

    const Timestamp until = io::MonotonicNow() + BOOTSTRAP_TIMEOUT;
    std::array<std::byte, 4096> chunk{};
    std::optional<session::BootstrapResult> result;
    while (!result) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            SayError("bootstrap", "no offer arrived on stdin");
            return std::nullopt;
        }
        pollfd waiting{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        const int ready =
            ::poll(&waiting, 1, io::TimeoutMillis(Deadline{until}, now));
        if (ready <= 0) {
            continue;
        }
        const auto n = io::ReadSome(STDIN_FILENO, MutableByteSpan{chunk});
        if (!n) {
            if (n.error().WouldBlock()) {
                continue;
            }
            SayError("read stdin", n.error().Description());
            return std::nullopt;
        }
        if (*n == 0) {
            SayError("bootstrap", "stdin ended before an offer");
            return std::nullopt;
        }
        auto step = bootstrap.Feed(ByteSpan{chunk}.first(*n));
        if (!step) {
            SayError("bootstrap", step.error().Description());
            return std::nullopt;
        }
        result = std::move(step->Result);
    }

    std::unique_ptr<MuxLink> mux;
    if (!listener) {
        mux = std::make_unique<MuxLink>(
            muxPath, crypto::Key{result->Secrets.Registration.Expose()},
            MUX_TOKEN);
        if (auto port = mux->RegisterNow(until)) {
            bootstrap.AnnouncePort(*port);
        } else {
            // A mux that is down or not to be trusted leaves the agent where
            // M4 had it: on a port of its own.
            SayError("mux", port.error());
            mux.reset();
            auto bound = io::ListenTcp();
            if (!bound) {
                SayError("listen", bound.error().Description());
                return std::nullopt;
            }
            listener = std::move(*bound);
            bootstrap.AnnouncePort(listener->Port);
        }
    }

    std::array<char, session::BOOTSTRAP_ACCEPT_TEXT_SIZE> text{};
    const auto length = bootstrap.WriteAccept(text);
    if (!length || !WriteAll(STDOUT_FILENO,
                             std::as_bytes(std::span{text}.first(*length)))) {
        SayError("bootstrap", "could not write the accept");
        return std::nullopt;
    }
    return Bootstrapped{.Result = std::move(*result),
                        .Listener = std::move(listener),
                        .Mux = std::move(mux)};
}

/// The user's login shell and the environment it starts with: the agent's
/// own, which sshd built for the user, with what the client sent on top
/// (`NAME=value` entries: the terminal and the language, §4).
class ShellCommand {
public:
    explicit ShellCommand(const wire::BootstrapEnvironment& fromClient) {
        const passwd* user = ::getpwuid(::getuid());
        Path_ = user != nullptr && user->pw_shell != nullptr &&
                        user->pw_shell[0] != '\0'
                    ? user->pw_shell
                    : "/bin/sh";
        if (user != nullptr && user->pw_dir != nullptr) {
            Home_ = user->pw_dir;
        }
        // A leading dash is how a shell learns it is a login shell.
        const auto slash = Path_.rfind('/');
        Name_ = "-" + Path_.substr(slash == std::string::npos ? 0 : slash + 1);
        Argv_ = {Name_.c_str(), nullptr};

        const auto sentByClient = [&](std::string_view text) {
            const auto name = text.substr(0, text.find('='));
            return std::ranges::any_of(fromClient.Variables(),
                                       [&](const wire::EnvironmentVariable& v) {
                                           return v.Name() == name;
                                       });
        };
        bool hasTerm = false;
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string_view text{*entry};
            // zet's own settings are the agent's, not the shell's.
            if (!text.starts_with("ZET_") && !sentByClient(text)) {
                Environment_.emplace_back(text);
                hasTerm = hasTerm || text.starts_with("TERM=");
            }
        }
        for (const wire::EnvironmentVariable& variable :
             fromClient.Variables()) {
            Environment_.push_back(std::string{variable.Name()} + "=" +
                                   std::string{variable.Value()});
            hasTerm = hasTerm || variable.Name() == "TERM";
        }
        // ssh -T gives the remote side no TERM, and a shell without one runs
        // nothing that draws.
        if (!hasTerm) {
            Environment_.push_back("TERM=" + std::string{DEFAULT_TERM});
        }
        for (const std::string& entry : Environment_) {
            Envp_.push_back(entry.c_str());
        }
        Envp_.push_back(nullptr);
    }

    [[nodiscard]] io::SpawnSpec Spec() const noexcept {
        return io::SpawnSpec{
            .Path = Path_.c_str(),
            .Argv = Argv_,
            .Envp = Envp_,
            .Size = INITIAL_SIZE,
            .WorkingDirectory = Home_.empty() ? nullptr : Home_.c_str()};
    }

private:
    std::string Path_;
    std::string Name_;
    std::string Home_;
    std::array<const char*, 2> Argv_{};
    std::vector<std::string> Environment_;
    std::vector<const char*> Envp_;
};

/// One TCP connection and what it has not written yet.
struct Connection {
    Connection(io::UniqueFd socket, std::uint64_t token, Timestamp now,
               std::size_t outputLimit)
        : Socket(std::move(socket)),
          Watch(Socket.Get(), token),
          Frames(session::CONNECTION_FRAME_STORAGE),
          Link(MutableByteSpan{Frames}, now),
          Output(outputLimit) {}

    io::UniqueFd Socket;
    app::Registration Watch;
    std::vector<std::byte> Frames;
    session::AgentConnection Link;
    app::ByteQueue Output;
    /// Done with: dropped at the end of this turn of the loop.
    bool Closing{false};
    /// The client closed its end, which after SessionEnd is how it says it
    /// has the whole stream.
    bool PeerClosed{false};
    /// What AgentConnection::DeliveredEnd said at the last Pump: the read
    /// that sees the close comes after it.
    bool DeliveredEnd{false};
};

wire::SessionEndMessage EndOf(const io::ExitStatus& status) noexcept {
    return wire::SessionEndMessage{
        .Kind = status.Kind == io::ExitStatus::EKind::Exited
                    ? wire::ESessionEndKind::Exited
                    : wire::ESessionEndKind::Signaled,
        .Value = static_cast<std::uint8_t>(status.Value)};
}

class Agent {
public:
    Agent(Config config, Bootstrapped bootstrapped, io::Poller poller)
        : Config_(std::move(config)),
          ShellEnvironment_(bootstrapped.Result.Environment),
          Listener_(std::move(bootstrapped.Listener)),
          Mux_(std::move(bootstrapped.Mux)),
          Poller_(std::move(poller)),
          Storage_(
              std::max(Config_.Limits.ResendMax, Config_.Limits.DisconnectMax)),
          Session_(std::move(bootstrapped.Result.Secrets),
                   session::SessionStreams{.Storage = MutableByteSpan{Storage_},
                                           .Now = io::MonotonicNow(),
                                           .Limits = Config_.Limits}),
          LocalSecret_(RandomKey()),
          PtyInput_(PTY_INPUT_MAX) {
        if (Listener_) {
            ListenerWatch_.emplace(Listener_->Socket.Get(), LISTENER_TOKEN);
        }
    }

    int Run() noexcept;

private:
    [[nodiscard]] bool Start() noexcept;
    void MovePtyOutput() noexcept;
    void Pump(Connection& connection, Timestamp now) noexcept;
    void DropClosed(Timestamp now) noexcept;
    [[nodiscard]] bool UpdateInterest() noexcept;
    [[nodiscard]] Deadline NextDeadline() const noexcept;
    void Handle(const io::PollEvent& event) noexcept;
    void AcceptAll() noexcept;
    void Adopt(io::UniqueFd socket, ByteSpan prefix) noexcept;
    void ReadPty() noexcept;
    void WritePty() noexcept;
    void ReadSocket(Connection& connection) noexcept;
    void WriteSocket(Connection& connection) noexcept;
    void TakeChild() noexcept;
    [[nodiscard]] int Hangup() noexcept;
    [[nodiscard]] Connection* Find(std::uint64_t token) noexcept;

    Config Config_;
    /// What the client asked the shell's environment to have (§4).
    wire::BootstrapEnvironment ShellEnvironment_;
    std::optional<io::Listener> Listener_;
    /// Behind zet-muxd, in place of the listener.
    std::unique_ptr<MuxLink> Mux_;
    io::Poller Poller_;
    std::vector<std::byte> Storage_;
    session::AgentSession Session_;
    crypto::Key LocalSecret_;
    std::optional<app::Registration> ListenerWatch_;
    std::vector<HandedOver> HandedOver_;

    io::Child Child_;
    std::optional<app::Registration> PtyWatch_;
    std::optional<io::ExitStatus> ChildStatus_;
    bool PtyEnded_{false};
    std::array<std::byte, PTY_READ_CHUNK> PtyRead_{};
    std::size_t PtyFrom_{0};
    std::size_t PtyTo_{0};
    app::ByteQueue PtyInput_;

    std::vector<std::unique_ptr<Connection>> Connections_;
    std::uint64_t NextToken_{FIRST_CONNECTION_TOKEN};
    bool EndDelivered_{false};
    bool Terminating_{false};

    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
};

bool Agent::Start() noexcept {
    for (const int signo : {SIGCHLD, SIGTERM, SIGHUP, SIGINT}) {
        if (auto watched = Poller_.WatchSignal(signo, SIGNAL_TOKEN); !watched) {
            Log(log::ELevel::Error, "cannot watch a signal");
            return false;
        }
    }
    const ShellCommand shell{ShellEnvironment_};
    auto child = io::Spawn(shell.Spec());
    if (!child) {
        const std::array fields{
            log::Field::Text("call", child.error().Call),
            log::Field::Text("error", child.error().Description())};
        Log(log::ELevel::Error, "cannot start the shell", fields);
        return false;
    }
    Child_ = std::move(*child);
    PtyWatch_.emplace(Child_.Master.Get(), PTY_TOKEN);
    const std::array fields{
        log::Field::Unsigned("port",
                             Listener_ ? Listener_->Port : Mux_->Port()),
        log::Field::Text("via", Mux_ ? "mux" : "own"),
        log::Field::Signed("shell", Child_.Pid)};
    Log(log::ELevel::Info, "serving", fields);
    return true;
}

int Agent::Run() noexcept {
    if (!Start()) {
        return FAILURE_STATUS;
    }
    std::array<io::PollEvent, EVENTS_PER_WAIT> events{};
    for (;;) {
        const Timestamp now = io::MonotonicNow();
        Session_.OnTimer(now);
        if (Session_.State() == session::ESessionState::Reaped ||
            Terminating_) {
            return Hangup();
        }
        if (Mux_) {
            Mux_->OnTimer(Poller_, now);
        }
        MovePtyOutput();
        if (ChildStatus_ && PtyEnded_ && PtyFrom_ == PtyTo_ &&
            !Session_.HasEnded()) {
            Session_.End(EndOf(*ChildStatus_));
            const std::array fields{
                log::Field::Signed("status", ChildStatus_->Value)};
            Log(log::ELevel::Info, "the shell has ended", fields);
        }
        for (auto& connection : Connections_) {
            Pump(*connection, now);
        }
        DropClosed(now);
        if (EndDelivered_) {
            Log(log::ELevel::Info, "the client has the whole session; done");
            return 0;
        }
        if (Session_.State() == session::ESessionState::Reaped) {
            continue;
        }
        if (!UpdateInterest()) {
            return Hangup();
        }
        const auto ready = Poller_.Wait(events, NextDeadline(), now);
        if (!ready) {
            Log(log::ELevel::Error, "the poller failed");
            return Hangup();
        }
        for (std::size_t i = 0; i < *ready; ++i) {
            Handle(events[i]);
        }
    }
}

void Agent::MovePtyOutput() noexcept {
    while (PtyFrom_ < PtyTo_ && Session_.AcceptsWrites()) {
        const auto taken = Session_.Write(
            ByteSpan{PtyRead_}.subspan(PtyFrom_, PtyTo_ - PtyFrom_));
        if (!taken || *taken == 0) {
            // An overflow while detached reaps the session; the top of the
            // loop sees it.
            return;
        }
        PtyFrom_ += *taken;
    }
    if (PtyFrom_ == PtyTo_) {
        PtyFrom_ = 0;
        PtyTo_ = 0;
    }
}

void Agent::Pump(Connection& connection, Timestamp now) noexcept {
    while (!connection.Closing) {
        auto result = connection.Link.Pump(now, MutableByteSpan{Out_},
                                           MutableByteSpan{Scratch_});
        if (!result) {
            const std::array fields{
                log::Field::Text("reason", result.error().Description())};
            Log(log::ELevel::Info, "connection closed", fields);
            connection.Closing = true;
            return;
        }
        connection.DeliveredEnd = connection.Link.DeliveredEnd();
        const auto& event = *result;
        if (const auto* needs = std::get_if<session::NeedsSession>(&event)) {
            const auto nonce = RandomNonce<wire::HandshakeNonce>();
            if (needs->Id == Session_.Ref().Id()) {
                connection.Link.Route(Session_.Ref(), nonce);
                Log(log::ELevel::Debug, "hello for our session");
            } else {
                // Looks the same on the wire (§5); the attempt cannot end live.
                connection.Link.RouteUnknown(LocalSecret_, nonce);
                Log(log::ELevel::Info, "hello for an unknown session");
            }
        } else if (const auto* frame =
                       std::get_if<session::SendFrame>(&event)) {
            if (!connection.Output.Append(
                    ByteSpan{Out_}.first(frame->Written))) {
                Log(log::ELevel::Warn, "a connection fell too far behind");
                connection.Closing = true;
            }
        } else if (const auto* deliver =
                       std::get_if<session::Deliver>(&event)) {
            if (!PtyInput_.Append(deliver->Bytes)) {
                Log(log::ELevel::Warn, "keystrokes past the pty's queue");
                connection.Closing = true;
            }
        } else if (const auto* resized =
                       std::get_if<session::Resized>(&event)) {
            static_cast<void>(
                io::Resize(Child_.Master.Get(),
                           io::WindowSize{.Rows = resized->Size.Rows,
                                          .Cols = resized->Size.Cols,
                                          .XPixels = resized->Size.XPixel,
                                          .YPixels = resized->Size.YPixel}));
        } else if (std::holds_alternative<session::Idle>(event)) {
            return;
        }
    }
}

void Agent::DropClosed(Timestamp now) noexcept {
    std::erase_if(Connections_, [&](const std::unique_ptr<Connection>& c) {
        if (!c->Closing) {
            return false;
        }
        // Only the connection that carries the session, closed by the
        // client after SessionEnd went out on it, says the client has
        // everything; a superseded one closing says nothing.
        if (c->PeerClosed && c->DeliveredEnd) {
            EndDelivered_ = true;
        }
        c->Link.Close(now);
        c->Watch.Forget(Poller_);
        return true;
    });
}

bool Agent::UpdateInterest() noexcept {
    bool ok = !ListenerWatch_ ||
              static_cast<bool>(ListenerWatch_->Want(Poller_, true, false));
    if (Mux_) {
        // A refusal loses the link and schedules its retry inside MuxLink.
        Mux_->UpdateInterest(Poller_, io::MonotonicNow());
    }
    if (PtyWatch_) {
        const bool read =
            !PtyEnded_ && PtyFrom_ == PtyTo_ && Session_.AcceptsWrites();
        // Once the shell has gone, nothing reads what we write to it.
        const bool write = !PtyInput_.Empty() && !ChildStatus_;
        ok = static_cast<bool>(PtyWatch_->Want(Poller_, read, write)) && ok;
    }
    for (auto& connection : Connections_) {
        const bool read = connection->Link.ReadRoom() > 0 &&
                          PtyInput_.Room() >= connection->Link.ReadCapacity();
        const bool write = !connection->Output.Empty();
        if (!connection->Watch.Want(Poller_, read, write)) {
            connection->Closing = true;
        }
    }
    return ok;
}

Deadline Agent::NextDeadline() const noexcept {
    Deadline next = Session_.NextDeadline();
    if (Mux_) {
        next = Earliest(next, Mux_->NextDeadline());
    }
    for (const auto& connection : Connections_) {
        next = Earliest(next, connection->Link.NextDeadline());
    }
    return next;
}

Connection* Agent::Find(std::uint64_t token) noexcept {
    for (auto& connection : Connections_) {
        if (connection->Watch.Token() == token) {
            return connection.get();
        }
    }
    return nullptr;
}

void Agent::Handle(const io::PollEvent& event) noexcept {
    switch (event.Token) {
        case LISTENER_TOKEN:
            AcceptAll();
            return;
        case MUX_TOKEN:
            if (Mux_) {
                Mux_->OnEvent(event, Poller_, io::MonotonicNow(), HandedOver_);
                for (HandedOver& handed : HandedOver_) {
                    Adopt(std::move(handed.Socket), ByteSpan{handed.Prefix});
                }
                HandedOver_.clear();
            }
            return;
        case PTY_TOKEN:
            if (event.Writable) {
                WritePty();
            }
            if (event.Readable || event.Hangup || event.Error) {
                ReadPty();
            }
            return;
        case SIGNAL_TOKEN:
            if (event.Signal == SIGCHLD) {
                TakeChild();
            } else {
                Terminating_ = true;
            }
            return;
        default:
            break;
    }
    Connection* connection = Find(event.Token);
    if (connection == nullptr || connection->Closing) {
        return;
    }
    if (event.Writable) {
        WriteSocket(*connection);
    }
    if (event.Readable || event.Hangup || event.Error) {
        ReadSocket(*connection);
    }
}

void Agent::AcceptAll() noexcept {
    for (;;) {
        auto socket = io::Accept(Listener_->Socket.Get());
        if (!socket) {
            if (!socket.error().WouldBlock()) {
                const std::array fields{
                    log::Field::Text("error", socket.error().Description())};
                Log(log::ELevel::Warn, "accept failed", fields);
            }
            return;
        }
        if (Connections_.size() >= MAX_CONNECTIONS) {
            Log(log::ELevel::Warn, "too many connections; refusing one");
            continue;
        }
        Connections_.push_back(std::make_unique<Connection>(
            std::move(*socket), NextToken_++, io::MonotonicNow(),
            Config_.Limits.ResendMax + SOCKET_OUTPUT_SLACK));
        Log(log::ELevel::Info, "connection accepted");
    }
}

/// A connection zet-muxd has passed over (§6): what the mux read off it
/// goes in before anything the socket has, as if it had been read here.
void Agent::Adopt(io::UniqueFd socket, ByteSpan prefix) noexcept {
    if (Connections_.size() >= MAX_CONNECTIONS) {
        Log(log::ELevel::Warn, "too many connections; refusing a handoff");
        return;
    }
    auto connection = std::make_unique<Connection>(
        std::move(socket), NextToken_++, io::MonotonicNow(),
        Config_.Limits.ResendMax + SOCKET_OUTPUT_SLACK);
    if (!connection->Link.OnBytes(prefix)) {
        Log(log::ELevel::Info, "a handoff with a bad prefix");
        return;
    }
    Connections_.push_back(std::move(connection));
    Log(log::ELevel::Info, "connection handed over");
}

void Agent::ReadPty() noexcept {
    if (PtyEnded_ || PtyTo_ != 0) {
        return;
    }
    const auto n = io::ReadPty(Child_.Master.Get(),
                               MutableByteSpan{PtyRead_}.first(PTY_READ_CHUNK));
    if (!n) {
        if (!n.error().WouldBlock()) {
            PtyEnded_ = true;
        }
        return;
    }
    if (*n == 0) {
        PtyEnded_ = true;
        return;
    }
    PtyFrom_ = 0;
    PtyTo_ = *n;
}

void Agent::WritePty() noexcept {
    while (!PtyInput_.Empty()) {
        const auto n = io::WriteSome(Child_.Master.Get(), PtyInput_.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                // The shell is gone; what it would have read goes nowhere.
                PtyInput_.Consume(PtyInput_.Size());
            }
            return;
        }
        PtyInput_.Consume(*n);
    }
}

void Agent::ReadSocket(Connection& connection) noexcept {
    std::array<std::byte, SOCKET_READ_CHUNK> chunk{};
    const std::size_t room = std::min(connection.Link.ReadRoom(), chunk.size());
    if (room == 0 || PtyInput_.Room() < connection.Link.ReadCapacity()) {
        return;
    }
    const auto n = io::ReadSome(connection.Socket.Get(),
                                MutableByteSpan{chunk}.first(room));
    if (!n) {
        if (!n.error().WouldBlock()) {
            connection.Closing = true;
        }
        return;
    }
    if (*n == 0) {
        connection.PeerClosed = true;
        connection.Closing = true;
        return;
    }
    if (!connection.Link.OnBytes(ByteSpan{chunk}.first(*n))) {
        connection.Closing = true;
    }
}

void Agent::WriteSocket(Connection& connection) noexcept {
    while (!connection.Output.Empty()) {
        const auto n =
            io::WriteSome(connection.Socket.Get(), connection.Output.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                connection.Closing = true;
            }
            return;
        }
        connection.Output.Consume(*n);
    }
}

void Agent::TakeChild() noexcept {
    if (ChildStatus_) {
        return;
    }
    const auto reaped = io::TryReap(Child_.Pid);
    if (reaped && *reaped) {
        ChildStatus_ = **reaped;
    }
}

int Agent::Hangup() noexcept {
    const auto reason = Session_.ReapReason();
    const std::array fields{log::Field::Text(
        "reason", reason ? session::Describe(*reason) : "terminated")};
    Log(log::ELevel::Info, "ending the session", fields);

    const Timestamp now = io::MonotonicNow();
    for (auto& connection : Connections_) {
        connection->Link.Close(now);
        connection->Watch.Forget(Poller_);
    }
    Connections_.clear();
    if (Mux_) {
        Mux_->Close(Poller_);
    }
    if (PtyWatch_) {
        PtyWatch_->Forget(Poller_);
    }
    if (!ChildStatus_) {
        // The shell leads its own session, so its pid is its process group.
        static_cast<void>(::kill(-Child_.Pid, SIGHUP));
    }
    Child_.Master.Reset();

    std::array<io::PollEvent, EVENTS_PER_WAIT> events{};
    const Timestamp until = now + HANGUP_GRACE;
    while (!ChildStatus_) {
        const Timestamp at = io::MonotonicNow();
        if (at >= until) {
            static_cast<void>(::kill(-Child_.Pid, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(Child_.Pid, &status, 0));
            break;
        }
        const auto ready = Poller_.Wait(events, Deadline{until}, at);
        if (!ready) {
            break;
        }
        for (std::size_t i = 0; i < *ready; ++i) {
            if (events[i].Signal == SIGCHLD) {
                TakeChild();
            }
        }
    }
    return 0;
}

}  // namespace

int Main(std::span<char*> args) noexcept {
    auto config = ReadConfig(args);
    if (!config) {
        Say(USAGE);
        return USAGE_STATUS;
    }
    if (config->PrintVersion) {
        constexpr std::string_view prefix = "zet-agent ";
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
    // A peer that closes mid-write is a closed connection, not a reason to die.
    static_cast<void>(::signal(SIGPIPE, SIG_IGN));

    // NOLINTNEXTLINE(concurrency-mt-unsafe): one thread.
    if (const char* path = std::getenv("ZET_AGENT_LOG")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                              S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            app::LogToFd(fd);
            log::SetMinLevel(log::ELevel::Debug);
        } else {
            SayError("log", std::strerror(errno));
        }
    }

    auto bootstrapped = Bootstrap(*config);
    if (!bootstrapped) {
        return FAILURE_STATUS;
    }
    const auto role = io::Detach();
    if (!role) {
        SayError("detach", role.error().Description());
        return FAILURE_STATUS;
    }
    if (*role == io::EDetached::Parent) {
        // ssh waits for this process; the session goes on in the child.
        ::_exit(0);
    }
    auto poller = io::Poller::Create();
    if (!poller) {
        Log(log::ELevel::Error, "cannot create the poller");
        return FAILURE_STATUS;
    }
    auto agent = std::make_unique<Agent>(
        std::move(*config), std::move(*bootstrapped), std::move(*poller));
    return agent->Run();
}

}  // namespace zet::agent
