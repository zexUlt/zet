#include "mux.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "zet/app/byte_queue.hpp"
#include "zet/app/mux_frames.hpp"
#include "zet/app/registration.hpp"
#include "zet/core/byte_reader.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/activation.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"
#include "zet/io/seccomp.hpp"
#include "zet/io/tcp.hpp"
#include "zet/io/unix.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/mux_router.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/message.hpp"

namespace zet::mux {

namespace {

/// Where agents register when neither systemd nor ZET_MUX_SOCKET says
/// otherwise (§6).
constexpr const char* DEFAULT_SOCKET = "/run/zet/mux.sock";

/// The agents' socket is every user's: each user's agent registers on it.
/// Who registered what is told by SO_PEERCRED, not by the mode.
constexpr mode_t SOCKET_MODE = 0666;

/// Write permission for group or others on the socket's directory would let
/// them replace the socket with their own.
constexpr mode_t FOREIGN_WRITE = S_IWGRP | S_IWOTH;

/// Agents' connections at once, and of one uid. Every agent holds one; the
/// per-uid share is what keeps one user's flood from shutting the others
/// out, the same reasoning as MAX_REGISTRATIONS_PER_UID.
constexpr std::size_t MAX_AGENT_CONNECTIONS = session::MAX_MUX_REGISTRATIONS;
constexpr std::size_t MAX_AGENT_CONNECTIONS_PER_UID =
    2 * session::MAX_REGISTRATIONS_PER_UID;

/// Handoffs queued for one agent that has not read the ones before. An agent
/// is one loop that reads its socket as soon as it can; one this far behind
/// is stuck, and the connections are better closed — the clients reconnect.
constexpr std::size_t MAX_QUEUED_HANDOFFS = 64;

/// Decoy handshakes at once (§5, rule two). Each is a connection that named
/// no live sid, and lives at most HANDSHAKE_TIMEOUT; past this many they are
/// closed at once, which costs the one property only to someone who is
/// already flooding.
constexpr std::size_t MAX_DECOYS = session::MAX_PENDING_CONNECTIONS;

/// Sealed output a decoy may have waiting: its whole handshake.
constexpr std::size_t DECOY_OUTPUT_MAX = 4 * wire::MAX_HANDSHAKE_FRAME;

constexpr std::size_t READ_CHUNK = 4096;
constexpr std::size_t EVENTS_PER_WAIT = 64;

constexpr std::uint64_t TCP_TOKEN = 1;
constexpr std::uint64_t UNIX_TOKEN = 2;
constexpr std::uint64_t SIGNAL_TOKEN = 3;
constexpr std::uint64_t FIRST_TOKEN = 16;

constexpr int USAGE_STATUS = 2;
constexpr int FAILURE_STATUS = 1;
constexpr int DECIMAL = 10;

constexpr std::string_view USAGE =
    "usage: zet-muxd [--port N]\n"
    "The TCP port comes from systemd socket activation, or from --port.\n";

void Say(std::string_view text) noexcept {
    static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

void SayError(std::string_view what, std::string_view why) noexcept {
    Say("zet-muxd: ");
    Say(what);
    Say(": ");
    Say(why);
    Say("\n");
}

/// One line per event to ZET_MUX_LOG, opened for appending before seccomp.
/// A plain sink rather than the structured logger: the registration line is
/// a contract the M5 suite reads, `registered sid=<hex> uid=<n>`.
class Log {
public:
    void Open(const char* path) noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        Fd_.Reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
        if (!Fd_) {
            SayError("log", std::strerror(errno));
        }
    }

    void Line(std::string_view text) const noexcept {
        if (!Fd_) {
            return;
        }
        std::string line{text};
        line += '\n';
        // One write, so that concurrent appenders cannot interleave a line.
        static_cast<void>(::write(Fd_.Get(), line.data(), line.size()));
    }

private:
    io::UniqueFd Fd_;
};

std::string Hex(ByteSpan bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr unsigned nibble = 4;
    constexpr unsigned low = 0x0F;
    std::string out;
    out.reserve(2 * bytes.size());
    for (const std::byte b : bytes) {
        const auto value = std::to_integer<unsigned>(b);
        out += digits[value >> nibble];
        out += digits[value & low];
    }
    return out;
}

crypto::Key RandomKey() noexcept {
    crypto::KeyBytes bytes{};
    crypto::RandomBytes(MutableByteSpan{bytes});
    return crypto::Key{bytes};
}

/// The family a socket was bound with, which is how activated sockets are
/// told apart: by what they are, not by the order systemd passed them in.
int FamilyOf(int fd) noexcept {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        return AF_UNSPEC;
    }
    return address.ss_family;
}

std::uint16_t PortOf(int fd) noexcept {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        return 0;
    }
    if (address.ss_family == AF_INET6) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, &address, sizeof(v6));
        return ntohs(v6.sin6_port);
    }
    sockaddr_in v4{};
    std::memcpy(&v4, &address, sizeof(v4));
    return ntohs(v4.sin_port);
}

/// The peer's address as the per-address limit counts it: IPv6, or IPv4
/// mapped into it.
session::PeerAddress PeerOf(int fd) noexcept {
    session::PeerAddress out{};
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        return out;
    }
    if (address.ss_family == AF_INET6) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, &address, sizeof(v6));
        std::memcpy(out.data(), &v6.sin6_addr, out.size());
    } else if (address.ss_family == AF_INET) {
        sockaddr_in v4{};
        std::memcpy(&v4, &address, sizeof(v4));
        constexpr std::size_t mappedAt = 10;
        constexpr std::size_t addressAt = 12;
        out.at(mappedAt) = std::byte{0xFF};
        out.at(mappedAt + 1) = std::byte{0xFF};
        std::memcpy(out.data() + addressAt, &v4.sin_addr, sizeof(v4.sin_addr));
    }
    return out;
}

struct Config {
    std::optional<std::uint16_t> Port;
};

std::optional<Config> ReadConfig(std::span<char*> args) {
    Config config;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--port" && i + 1 < args.size()) {
            char* end = nullptr;
            const unsigned long port = std::strtoul(args[++i], &end, DECIMAL);
            if (end == nullptr || *end != '\0' || port == 0 ||
                port > UINT16_MAX) {
                return std::nullopt;
            }
            config.Port = static_cast<std::uint16_t>(port);
        } else {
            return std::nullopt;
        }
    }
    return config;
}

/// The directory of `path`: ours or root's, and nobody else may write it.
bool DirectoryIsSafe(const std::string& path) noexcept {
    const auto slash = path.rfind('/');
    const std::string directory = slash == std::string::npos
                                      ? "."
                                      : path.substr(0, slash == 0 ? 1 : slash);
    struct stat st{};
    if (::lstat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        SayError(directory, "not a directory");
        return false;
    }
    if ((st.st_uid != ::getuid() && st.st_uid != 0) ||
        (st.st_mode & FOREIGN_WRITE) != 0) {
        SayError(directory, "writable by someone other than the mux or root");
        return false;
    }
    return true;
}

/// Binds the agents' socket at `path`. A socket left there by a mux that is
/// gone — SIGKILL leaves it; after seccomp even a clean exit cannot remove
/// it — is replaced; one somebody still answers on is another mux.
io::IoResult<io::UniqueFd> BindAgentsSocket(const std::string& path) noexcept {
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid()) {
            return std::unexpected(io::Errno{.Code = EEXIST, .Call = "lstat"});
        }
        if (auto other = io::ConnectUnix(path.c_str())) {
            return std::unexpected(
                io::Errno{.Code = EADDRINUSE, .Call = "connect"});
        }
        if (::unlink(path.c_str()) != 0) {
            return std::unexpected(io::LastErrno("unlink"));
        }
    }
    return io::ListenUnix(path.c_str(), SOCKET_MODE);
}

/// A TCP connection whose Hello is not all in yet.
struct Pending {
    Pending(io::UniqueFd socket, std::uint64_t token)
        : Socket(std::move(socket)), Watch(Socket.Get(), token) {}
    io::UniqueFd Socket;
    app::Registration Watch;
    bool Closing{false};
};

/// A connection that named no live sid, answered by the decoy handshake.
struct Decoy {
    Decoy(io::UniqueFd socket, std::uint64_t token, Timestamp now)
        : Socket(std::move(socket)),
          Watch(Socket.Get(), token),
          Frames(session::CONNECTION_FRAME_STORAGE),
          Link(MutableByteSpan{Frames}, now),
          Output(DECOY_OUTPUT_MAX) {}
    io::UniqueFd Socket;
    app::Registration Watch;
    std::vector<std::byte> Frames;
    session::AgentConnection Link;
    app::ByteQueue Output;
    bool Closing{false};
};

/// What an agent is to be sent: a frame, and the descriptor that rides with
/// its first byte for a handoff.
struct Outgoing {
    std::vector<std::byte> Bytes;
    std::size_t Sent{0};
    std::optional<io::UniqueFd> Passed;
};

/// An agent's connection to the mux socket.
struct Agent {
    Agent(io::UniqueFd socket, std::uint64_t token, session::Uid uid)
        : Socket(std::move(socket)), Watch(Socket.Get(), token), Uid(uid) {}
    io::UniqueFd Socket;
    app::Registration Watch;
    session::Uid Uid;
    app::MuxFrameReader Reader;
    std::deque<Outgoing> Output;
    bool Closing{false};
};

class Mux {
public:
    Mux(io::UniqueFd tcp, io::UniqueFd agents, io::Poller poller, Log log)
        : Tcp_(std::move(tcp)),
          Agents_(std::move(agents)),
          Poller_(std::move(poller)),
          Log_(std::move(log)),
          Port_(PortOf(Tcp_.Get())),
          LocalSecret_(RandomKey()),
          PendingStorage_(session::MUX_PENDING_STORAGE),
          Router_(std::make_unique<session::MuxRouter>(
              MutableByteSpan{PendingStorage_})),
          TcpWatch_(Tcp_.Get(), TCP_TOKEN),
          AgentsWatch_(Agents_.Get(), UNIX_TOKEN) {}

    int Run() noexcept;

private:
    void Handle(const io::PollEvent& event, Timestamp now) noexcept;
    void AcceptTcp(Timestamp now) noexcept;
    void AcceptAgents() noexcept;
    void ReadPending(std::uint64_t id, Pending& pending,
                     Timestamp now) noexcept;
    void Decide(std::uint64_t id, const session::MuxDecision& decision,
                Timestamp now) noexcept;
    void ReadAgent(std::uint64_t id, Agent& agent) noexcept;
    [[nodiscard]] std::optional<wire::EMuxRefusal> Register(
        std::uint64_t id, const Agent& agent, ByteSpan body) noexcept;
    void Answer(std::uint64_t id, Agent& agent, ByteSpan body) noexcept;
    void WriteAgent(Agent& agent) noexcept;
    void PumpDecoy(Decoy& decoy, Timestamp now) noexcept;
    void ReadDecoy(Decoy& decoy) noexcept;
    void WriteDecoy(Decoy& decoy) noexcept;
    void DropClosed() noexcept;
    [[nodiscard]] bool UpdateInterest() noexcept;
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    io::UniqueFd Tcp_;
    io::UniqueFd Agents_;
    io::Poller Poller_;
    Log Log_;
    std::uint16_t Port_;
    crypto::Key LocalSecret_;
    std::vector<std::byte> PendingStorage_;
    std::unique_ptr<session::MuxRouter> Router_;
    app::Registration TcpWatch_;
    app::Registration AgentsWatch_;

    std::map<std::uint64_t, std::unique_ptr<Pending>> Pending_;
    std::map<std::uint64_t, std::unique_ptr<Decoy>> Decoys_;
    std::map<std::uint64_t, std::unique_ptr<Agent>> AgentPeers_;
    std::map<session::Uid, std::size_t> AgentsPerUid_;
    std::uint64_t NextToken_{FIRST_TOKEN};
    bool Stopping_{false};

    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
};

int Mux::Run() noexcept {
    Log_.Line("serving port=" + std::to_string(Port_));
    std::array<io::PollEvent, EVENTS_PER_WAIT> events{};
    while (!Stopping_) {
        const Timestamp now = io::MonotonicNow();
        while (const auto expired = Router_->Expire(now)) {
            if (auto found = Pending_.find(expired->Connection);
                found != Pending_.end()) {
                found->second->Closing = true;
            }
        }
        for (auto& [id, decoy] : Decoys_) {
            PumpDecoy(*decoy, now);
        }
        for (auto& [id, agent] : AgentPeers_) {
            WriteAgent(*agent);
        }
        DropClosed();
        if (!UpdateInterest()) {
            Log_.Line("the poller refused a descriptor; stopping");
            return FAILURE_STATUS;
        }
        const auto ready = Poller_.Wait(events, NextDeadline(), now);
        if (!ready) {
            Log_.Line("the poller failed; stopping");
            return FAILURE_STATUS;
        }
        for (std::size_t i = 0; i < *ready; ++i) {
            Handle(events.at(i), io::MonotonicNow());
        }
    }
    Log_.Line("stopping on a signal");
    return 0;
}

void Mux::Handle(const io::PollEvent& event, Timestamp now) noexcept {
    switch (event.Token) {
        case TCP_TOKEN:
            AcceptTcp(now);
            return;
        case UNIX_TOKEN:
            AcceptAgents();
            return;
        case SIGNAL_TOKEN:
            Stopping_ = true;
            return;
        default:
            break;
    }
    const bool readable = event.Readable || event.Hangup || event.Error;
    if (auto found = Pending_.find(event.Token); found != Pending_.end()) {
        if (readable && !found->second->Closing) {
            ReadPending(event.Token, *found->second, now);
        }
        return;
    }
    if (auto found = Decoys_.find(event.Token); found != Decoys_.end()) {
        Decoy& decoy = *found->second;
        if (event.Writable && !decoy.Closing) {
            WriteDecoy(decoy);
        }
        if (readable && !decoy.Closing) {
            ReadDecoy(decoy);
            PumpDecoy(decoy, now);
        }
        return;
    }
    if (auto found = AgentPeers_.find(event.Token);
        found != AgentPeers_.end()) {
        Agent& agent = *found->second;
        if (event.Writable && !agent.Closing) {
            WriteAgent(agent);
        }
        if (readable && !agent.Closing) {
            ReadAgent(event.Token, agent);
        }
    }
}

void Mux::AcceptTcp(Timestamp now) noexcept {
    for (;;) {
        auto socket = io::Accept(Tcp_.Get());
        if (!socket) {
            return;
        }
        const std::uint64_t id = NextToken_++;
        if (auto accepted = Router_->Accept(id, PeerOf(socket->Get()), now);
            !accepted) {
            // A pending limit: closed at once, by dropping it.
            continue;
        }
        Pending_.emplace(id, std::make_unique<Pending>(std::move(*socket), id));
    }
}

void Mux::AcceptAgents() noexcept {
    for (;;) {
        auto socket = io::AcceptUnix(Agents_.Get());
        if (!socket) {
            return;
        }
        const auto credentials = io::PeerCredentialsOf(socket->Get());
        if (!credentials) {
            continue;
        }
        const session::Uid uid = credentials->Uid;
        const auto ofUid = AgentsPerUid_.find(uid);
        if (AgentPeers_.size() >= MAX_AGENT_CONNECTIONS ||
            (ofUid != AgentsPerUid_.end() &&
             ofUid->second >= MAX_AGENT_CONNECTIONS_PER_UID)) {
            Log_.Line("refused an agent connection: limit uid=" +
                      std::to_string(uid));
            continue;
        }
        ++AgentsPerUid_[uid];
        const std::uint64_t id = NextToken_++;
        AgentPeers_.emplace(
            id, std::make_unique<Agent>(std::move(*socket), id, uid));
    }
}

void Mux::ReadPending(std::uint64_t id, Pending& pending,
                      Timestamp now) noexcept {
    std::array<std::byte, READ_CHUNK> chunk{};
    const std::size_t room = std::min(Router_->Room(id), chunk.size());
    if (room == 0) {
        return;
    }
    const auto n =
        io::ReadSome(pending.Socket.Get(), MutableByteSpan{chunk}.first(room));
    if (!n) {
        if (!n.error().WouldBlock()) {
            Router_->Closed(id);
            pending.Closing = true;
        }
        return;
    }
    if (*n == 0) {
        Router_->Closed(id);
        pending.Closing = true;
        return;
    }
    Decide(id, Router_->Feed(id, ByteSpan{chunk}.first(*n)), now);
}

void Mux::Decide(std::uint64_t id, const session::MuxDecision& decision,
                 Timestamp now) noexcept {
    if (std::holds_alternative<session::MuxNeedMore>(decision)) {
        return;
    }
    auto found = Pending_.find(id);
    if (found == Pending_.end()) {
        return;
    }
    Pending& pending = *found->second;
    pending.Watch.Forget(Poller_);
    pending.Closing = true;
    if (const auto* handoff = std::get_if<session::MuxHandoff>(&decision)) {
        auto agent = AgentPeers_.find(handoff->Agent);
        if (agent == AgentPeers_.end() || agent->second->Closing ||
            agent->second->Output.size() >= MAX_QUEUED_HANDOFFS) {
            Log_.Line("dropped a connection its agent could not take");
            return;
        }
        auto frame = app::CleartextFrame([&](ByteWriter& writer) {
            return wire::WriteMuxHandoff(
                       writer,
                       wire::MuxHandoffMessage{.Prefix = handoff->Prefix})
                .has_value();
        });
        if (frame.empty()) {
            return;
        }
        agent->second->Output.push_back(
            Outgoing{.Bytes = std::move(frame),
                     .Sent = 0,
                     .Passed = std::move(pending.Socket)});
        WriteAgent(*agent->second);
        return;
    }
    if (const auto* decoy = std::get_if<session::MuxDecoy>(&decision)) {
        if (Decoys_.size() >= MAX_DECOYS) {
            return;
        }
        const std::uint64_t token = NextToken_++;
        auto made =
            std::make_unique<Decoy>(std::move(pending.Socket), token, now);
        if (!made->Link.OnBytes(decoy->Prefix)) {
            return;
        }
        PumpDecoy(*made, now);
        Decoys_.emplace(token, std::move(made));
    }
    // MuxClose: the socket goes with `pending`.
}

void Mux::ReadAgent(std::uint64_t id, Agent& agent) noexcept {
    std::array<std::byte, READ_CHUNK> chunk{};
    const std::size_t room = std::min(agent.Reader.Room(), chunk.size());
    if (room == 0) {
        agent.Closing = true;
        return;
    }
    // Received as a message with room for descriptors only to close any an
    // agent sends: nothing flows that way.
    const auto got = io::ReceiveWithFd(agent.Socket.Get(),
                                       MutableByteSpan{chunk}.first(room));
    if (!got) {
        if (!got.error().WouldBlock()) {
            agent.Closing = true;
        }
        return;
    }
    if (got->Bytes == 0) {
        agent.Closing = true;
        return;
    }
    agent.Reader.Append(ByteSpan{chunk}.first(got->Bytes));
    for (;;) {
        auto body = agent.Reader.Next();
        if (!body) {
            agent.Closing = true;
            return;
        }
        if (!*body) {
            return;
        }
        Answer(id, agent, ByteSpan{**body});
        if (agent.Closing) {
            return;
        }
    }
}

std::optional<wire::EMuxRefusal> Mux::Register(std::uint64_t id,
                                               const Agent& agent,
                                               ByteSpan body) noexcept {
    ByteReader reader{body};
    const auto type = wire::ParseMessageType(reader);
    if (!type || *type != wire::EMessageType::MuxRegister) {
        return wire::EMuxRefusal::Malformed;
    }
    const auto message = wire::ParseMuxRegister(reader);
    if (!message) {
        return wire::EMuxRefusal::Malformed;
    }
    const auto sid = wire::SessionIdOf(message->Registration);
    const auto previous = Router_->Find(sid);
    if (auto registered =
            Router_->Register(id, agent.Uid, message->Registration);
        !registered) {
        return registered.error();
    }
    // The same uid with the same R is the agent itself, back before we saw
    // its old connection end (§6): that one is closed.
    if (previous && *previous != id) {
        if (auto old = AgentPeers_.find(*previous); old != AgentPeers_.end()) {
            old->second->Closing = true;
        }
    }
    Log_.Line("registered sid=" + Hex(ByteSpan{sid}) +
              " uid=" + std::to_string(agent.Uid));
    return std::nullopt;
}

void Mux::Answer(std::uint64_t id, Agent& agent, ByteSpan body) noexcept {
    const std::optional<wire::EMuxRefusal> refusal = Register(id, agent, body);
    std::vector<std::byte> frame;
    if (refusal) {
        Log_.Line("refused a registration uid=" + std::to_string(agent.Uid) +
                  " reason=" + std::to_string(static_cast<unsigned>(*refusal)));
        frame = app::CleartextFrame([&](ByteWriter& writer) {
            return wire::WriteMuxRefused(
                       writer, wire::MuxRefusedMessage{.Reason = *refusal})
                .has_value();
        });
    } else {
        frame = app::CleartextFrame([&](ByteWriter& writer) {
            return wire::WriteMuxRegistered(
                       writer, wire::MuxRegisteredMessage{.Port = Port_})
                .has_value();
        });
    }
    if (agent.Output.size() >= MAX_QUEUED_HANDOFFS) {
        agent.Closing = true;
        return;
    }
    agent.Output.push_back(
        Outgoing{.Bytes = std::move(frame), .Sent = 0, .Passed = std::nullopt});
    // Something that is not a registration is not an agent of ours.
    if (refusal == wire::EMuxRefusal::Malformed) {
        WriteAgent(agent);
        agent.Closing = true;
    }
}

void Mux::WriteAgent(Agent& agent) noexcept {
    while (!agent.Output.empty()) {
        Outgoing& front = agent.Output.front();
        const ByteSpan rest = ByteSpan{front.Bytes}.subspan(front.Sent);
        io::IoResult<std::size_t> n =
            front.Passed
                ? io::SendWithFd(agent.Socket.Get(), rest, front.Passed->Get())
                : io::WriteSome(agent.Socket.Get(), rest);
        if (!n) {
            if (!n.error().WouldBlock()) {
                agent.Closing = true;
            }
            return;
        }
        // The descriptor went with the first byte: the agent has its own copy
        // now, and ours closes here (§6).
        front.Passed.reset();
        front.Sent += *n;
        if (front.Sent == front.Bytes.size()) {
            agent.Output.pop_front();
        }
    }
}

void Mux::PumpDecoy(Decoy& decoy, Timestamp now) noexcept {
    while (!decoy.Closing) {
        auto result = decoy.Link.Pump(now, MutableByteSpan{Out_},
                                      MutableByteSpan{Scratch_});
        if (!result) {
            decoy.Closing = true;
            return;
        }
        const auto& event = *result;
        if (std::holds_alternative<session::NeedsSession>(event)) {
            wire::HandshakeNonce nonce{};
            crypto::RandomBytes(MutableByteSpan{nonce});
            decoy.Link.RouteUnknown(LocalSecret_, nonce);
        } else if (const auto* frame =
                       std::get_if<session::SendFrame>(&event)) {
            if (!decoy.Output.Append(ByteSpan{Out_}.first(frame->Written))) {
                decoy.Closing = true;
            }
            WriteDecoy(decoy);
        } else if (std::holds_alternative<session::Idle>(event)) {
            return;
        } else {
            // Deliver, Resized or Ended need a live session, which a decoy
            // never has.
            decoy.Closing = true;
        }
    }
}

void Mux::ReadDecoy(Decoy& decoy) noexcept {
    std::array<std::byte, READ_CHUNK> chunk{};
    const std::size_t room = std::min(decoy.Link.ReadRoom(), chunk.size());
    if (room == 0) {
        return;
    }
    const auto n =
        io::ReadSome(decoy.Socket.Get(), MutableByteSpan{chunk}.first(room));
    if (!n) {
        if (!n.error().WouldBlock()) {
            decoy.Closing = true;
        }
        return;
    }
    if (*n == 0 || !decoy.Link.OnBytes(ByteSpan{chunk}.first(*n))) {
        decoy.Closing = true;
    }
}

void Mux::WriteDecoy(Decoy& decoy) noexcept {
    while (!decoy.Output.Empty()) {
        const auto n = io::WriteSome(decoy.Socket.Get(), decoy.Output.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                decoy.Closing = true;
            }
            return;
        }
        decoy.Output.Consume(*n);
    }
}

void Mux::DropClosed() noexcept {
    std::erase_if(Pending_, [&](auto& entry) {
        if (!entry.second->Closing) {
            return false;
        }
        entry.second->Watch.Forget(Poller_);
        return true;
    });
    std::erase_if(Decoys_, [&](auto& entry) {
        if (!entry.second->Closing) {
            return false;
        }
        entry.second->Watch.Forget(Poller_);
        return true;
    });
    std::erase_if(AgentPeers_, [&](auto& entry) {
        if (!entry.second->Closing) {
            return false;
        }
        Router_->Unregister(entry.first);
        entry.second->Watch.Forget(Poller_);
        auto ofUid = AgentsPerUid_.find(entry.second->Uid);
        if (ofUid != AgentsPerUid_.end() && --ofUid->second == 0) {
            AgentsPerUid_.erase(ofUid);
        }
        return true;
    });
}

bool Mux::UpdateInterest() noexcept {
    if (!TcpWatch_.Want(Poller_, true, false) ||
        !AgentsWatch_.Want(Poller_, true, false)) {
        return false;
    }
    for (auto& [id, pending] : Pending_) {
        if (!pending->Closing &&
            !pending->Watch.Want(Poller_, Router_->Room(id) > 0, false)) {
            pending->Closing = true;
        }
    }
    for (auto& [id, decoy] : Decoys_) {
        if (!decoy->Closing &&
            !decoy->Watch.Want(Poller_, decoy->Link.ReadRoom() > 0,
                               !decoy->Output.Empty())) {
            decoy->Closing = true;
        }
    }
    for (auto& [id, agent] : AgentPeers_) {
        if (!agent->Closing &&
            !agent->Watch.Want(Poller_, true, !agent->Output.empty())) {
            agent->Closing = true;
        }
    }
    return true;
}

Deadline Mux::NextDeadline() const noexcept {
    Deadline next = Router_->NextDeadline();
    for (const auto& [id, decoy] : Decoys_) {
        next = Earliest(next, decoy->Link.NextDeadline());
    }
    return next;
}

}  // namespace

int Main(std::span<char*> args) noexcept {
    const auto config = ReadConfig(args);
    if (!config) {
        Say(USAGE);
        return USAGE_STATUS;
    }
    if (!crypto::Init()) {
        SayError("crypto", "libsodium did not initialise");
        return FAILURE_STATUS;
    }
    // A peer that closes mid-write is a closed connection, not a reason to die.
    static_cast<void>(::signal(SIGPIPE, SIG_IGN));

    Log log;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): one thread.
    if (const char* path = std::getenv("ZET_MUX_LOG")) {
        log.Open(path);
    }

    io::UniqueFd tcp;
    io::UniqueFd agents;
    auto activated = io::TakeActivatedSockets();
    if (!activated) {
        SayError("socket activation", activated.error().Description());
        return FAILURE_STATUS;
    }
    for (std::size_t i = 0; i < activated->Count; ++i) {
        io::UniqueFd& socket = activated->Items.at(i);
        const int family = FamilyOf(socket.Get());
        if (family == AF_UNIX && !agents) {
            agents = std::move(socket);
        } else if ((family == AF_INET || family == AF_INET6) && !tcp) {
            tcp = std::move(socket);
        }
    }
    if (!tcp) {
        if (!config->Port) {
            SayError("listen", "no activated TCP socket and no --port");
            return USAGE_STATUS;
        }
        auto listener = io::ListenTcp(*config->Port);
        if (!listener) {
            SayError("listen", listener.error().Description());
            return FAILURE_STATUS;
        }
        tcp = std::move(listener->Socket);
    }
    if (!agents) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): one thread.
        const char* fromEnv = std::getenv("ZET_MUX_SOCKET");
        const std::string path = fromEnv != nullptr ? fromEnv : DEFAULT_SOCKET;
        if (!DirectoryIsSafe(path)) {
            return FAILURE_STATUS;
        }
        auto bound = BindAgentsSocket(path);
        if (!bound) {
            SayError(path, bound.error().Description());
            return FAILURE_STATUS;
        }
        agents = std::move(*bound);
    }

    auto poller = io::Poller::Create();
    if (!poller) {
        SayError("poller", poller.error().Description());
        return FAILURE_STATUS;
    }
    for (const int signo : {SIGTERM, SIGINT, SIGHUP}) {
        if (auto watched = poller->WatchSignal(signo, SIGNAL_TOKEN); !watched) {
            SayError("signals", watched.error().Description());
            return FAILURE_STATUS;
        }
    }
    auto mux = std::make_unique<Mux>(std::move(tcp), std::move(agents),
                                     std::move(*poller), std::move(log));
    // From here the mux touches no file (§6): what it needs is open.
    if (auto filtered =
            io::InstallSeccomp(io::ESeccompProfile::NoFileOperations);
        !filtered) {
        SayError("seccomp", filtered.error().Description());
        return FAILURE_STATUS;
    }
    return mux->Run();
}

}  // namespace zet::mux
