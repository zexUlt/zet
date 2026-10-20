#include "mux_link.hpp"

#include <poll.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/log.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/unix.hpp"
#include "zet/wire/message.hpp"

namespace zet::agent {

namespace {

using namespace std::chrono_literals;

/// The reconnect backoff of §9, the client's: doubling from here, with
/// jitter, to the ceiling. A mux that restarts is back within the first few.
constexpr Duration BACKOFF_MIN = 250ms;
constexpr Duration BACKOFF_MAX = 30s;

/// Our own MuxRegister, and nothing else, goes out on the link.
constexpr std::size_t OUTPUT_MAX = app::MUX_MAX_FRAME;

/// Descriptors waiting for their MuxHandoff. Each rides on the frame's first
/// byte, so more than one waiting means a mux that breaks the protocol.
constexpr std::size_t MAX_WAITING_DESCRIPTORS = 2;

/// Write permission for group or others on the socket's directory: whoever
/// has it could put a socket of their own in the mux's place.
constexpr mode_t FOREIGN_WRITE = S_IWGRP | S_IWOTH;

void Log(log::ELevel level, std::string_view message,
         std::span<const log::Field> fields = {}) noexcept {
    log::Emit(level, message, fields, io::MonotonicNow());
}

/// The trust rule of MuxLink: the peer runs as the owner of the socket file,
/// in a directory nobody else but root may write.
std::expected<void, std::string_view> CheckPeer(const std::string& path,
                                                int fd) noexcept {
    struct stat socketStat{};
    if (::lstat(path.c_str(), &socketStat) != 0 ||
        !S_ISSOCK(socketStat.st_mode)) {
        return std::unexpected("no socket at the mux's path");
    }
    const auto slash = path.rfind('/');
    const std::string directory = slash == std::string::npos
                                      ? "."
                                      : path.substr(0, slash == 0 ? 1 : slash);
    struct stat directoryStat{};
    if (::lstat(directory.c_str(), &directoryStat) != 0 ||
        !S_ISDIR(directoryStat.st_mode)) {
        return std::unexpected("the mux's directory is not a directory");
    }
    if ((directoryStat.st_uid != 0 &&
         directoryStat.st_uid != socketStat.st_uid) ||
        (directoryStat.st_mode & FOREIGN_WRITE) != 0) {
        return std::unexpected("others may write the mux's directory");
    }
    const auto peer = io::PeerCredentialsOf(fd);
    if (!peer) {
        return std::unexpected("cannot tell who the mux is");
    }
    if (peer->Uid != socketStat.st_uid) {
        return std::unexpected("the mux is not the socket's owner");
    }
    return {};
}

}  // namespace

std::string MuxSocketPath() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): one thread.
    const char* fromEnv = std::getenv("ZET_MUX_SOCKET");
    return fromEnv != nullptr ? std::string{fromEnv}
                              : std::string{DEFAULT_MUX_SOCKET};
}

bool MuxSocketExists(const std::string& path) noexcept {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

MuxLink::MuxLink(std::string path, crypto::Key registration,
                 std::uint64_t token) noexcept
    : Path_(std::move(path)),
      Registration_(std::move(registration)),
      Token_(token),
      Output_(OUTPUT_MAX) {}

std::expected<void, std::string_view> MuxLink::Connect() noexcept {
    auto socket = io::ConnectUnix(Path_.c_str());
    if (!socket) {
        return std::unexpected(socket.error().Description());
    }
    if (auto trusted = CheckPeer(Path_, socket->Get()); !trusted) {
        return trusted;
    }
    auto frame = app::CleartextFrame([&](ByteWriter& writer) {
        return wire::WriteMuxRegister(
                   writer,
                   wire::MuxRegisterMessage{
                       .Registration = crypto::Key{Registration_.Expose()}})
            .has_value();
    });
    if (frame.empty() || !Output_.Append(ByteSpan{frame})) {
        return std::unexpected("cannot write the registration");
    }
    Socket_ = std::move(*socket);
    Reader_ = app::MuxFrameReader{};
    Descriptors_.clear();
    Registered_ = false;
    return {};
}

std::expected<std::uint16_t, std::string_view> MuxLink::RegisterNow(
    Timestamp until) noexcept {
    if (auto connected = Connect(); !connected) {
        return std::unexpected(connected.error());
    }
    std::vector<HandedOver> none;
    while (!Registered_) {
        const Timestamp now = io::MonotonicNow();
        if (now >= until) {
            return std::unexpected("the mux did not answer");
        }
        pollfd waiting{.fd = Socket_.Get(),
                       .events = static_cast<short>(
                           Output_.Empty() ? POLLIN : POLLIN | POLLOUT),
                       .revents = 0};
        if (::poll(&waiting, 1, io::TimeoutMillis(Deadline{until}, now)) <= 0) {
            continue;
        }
        while (!Output_.Empty()) {
            const auto n = io::WriteSome(Socket_.Get(), Output_.Front());
            if (!n) {
                if (n.error().WouldBlock()) {
                    break;
                }
                return std::unexpected(n.error().Description());
            }
            Output_.Consume(*n);
        }
        if ((waiting.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
        }
        std::array<std::byte, app::MUX_MAX_FRAME> chunk{};
        const auto got = io::ReceiveWithFd(
            Socket_.Get(), MutableByteSpan{chunk}.first(
                               std::min(chunk.size(), Reader_.Room())));
        if (!got) {
            if (got.error().WouldBlock()) {
                continue;
            }
            return std::unexpected(got.error().Description());
        }
        if (got->Bytes == 0) {
            return std::unexpected("the mux closed the connection");
        }
        // Nothing is handed over before the client has the Accept.
        Reader_.Append(ByteSpan{chunk}.first(got->Bytes));
        for (;;) {
            auto body = Reader_.Next();
            if (!body) {
                return std::unexpected("the mux broke the protocol");
            }
            if (!*body) {
                break;
            }
            if (!Take(ByteSpan{**body}, none)) {
                return std::unexpected("the mux refused the registration");
            }
        }
    }
    return Port_;
}

bool MuxLink::Take(ByteSpan body, std::vector<HandedOver>& out) noexcept {
    ByteReader reader{body};
    const auto type = wire::ParseMessageType(reader);
    if (!type) {
        return false;
    }
    switch (*type) {
        case wire::EMessageType::MuxRegistered: {
            const auto message = wire::ParseMuxRegistered(reader);
            if (!message || Registered_) {
                return false;
            }
            if (Port_ != 0 && message->Port != Port_) {
                // The client knows only the port it was told at bootstrap.
                const std::array fields{
                    log::Field::Unsigned("was", Port_),
                    log::Field::Unsigned("now", message->Port)};
                Log(log::ELevel::Warn, "the mux came back on another port",
                    fields);
            }
            Port_ = message->Port;
            Registered_ = true;
            Attempts_ = 0;
            Log(log::ELevel::Info, "registered with the mux");
            return true;
        }
        case wire::EMessageType::MuxRefused: {
            const auto message = wire::ParseMuxRefused(reader);
            const std::array fields{log::Field::Unsigned(
                "reason",
                message ? static_cast<unsigned>(message->Reason) : 0)};
            Log(log::ELevel::Warn, "the mux refused the registration", fields);
            return false;
        }
        case wire::EMessageType::MuxHandoff: {
            const auto message = wire::ParseMuxHandoff(reader);
            if (!message || !Registered_ || Descriptors_.empty()) {
                return false;
            }
            out.push_back(HandedOver{
                .Socket = std::move(Descriptors_.front()),
                .Prefix = {message->Prefix.begin(), message->Prefix.end()}});
            Descriptors_.pop_front();
            return true;
        }
        default:
            return false;
    }
}

void MuxLink::OnEvent(const io::PollEvent& event, io::Poller& poller,
                      Timestamp now, std::vector<HandedOver>& out) noexcept {
    if (!Socket_) {
        return;
    }
    if (event.Writable) {
        Write(poller, now);
    }
    if (Socket_ && (event.Readable || event.Hangup || event.Error)) {
        Read(poller, now, out);
    }
}

void MuxLink::Read(io::Poller& poller, Timestamp now,
                   std::vector<HandedOver>& out) noexcept {
    std::array<std::byte, app::MUX_MAX_FRAME> chunk{};
    const std::size_t room = std::min(chunk.size(), Reader_.Room());
    if (room == 0) {
        Lose(poller, now, "the mux broke the protocol");
        return;
    }
    auto got =
        io::ReceiveWithFd(Socket_.Get(), MutableByteSpan{chunk}.first(room));
    if (!got) {
        if (!got.error().WouldBlock()) {
            Lose(poller, now, got.error().Description());
        }
        return;
    }
    if (got->Fd) {
        Descriptors_.push_back(std::move(*got->Fd));
    }
    if (got->Bytes == 0) {
        Lose(poller, now, "the mux closed the connection");
        return;
    }
    Reader_.Append(ByteSpan{chunk}.first(got->Bytes));
    for (;;) {
        auto body = Reader_.Next();
        if (!body || (*body && !Take(ByteSpan{**body}, out))) {
            Lose(poller, now, "the mux broke the protocol");
            return;
        }
        if (!*body) {
            break;
        }
    }
    if (Descriptors_.size() > MAX_WAITING_DESCRIPTORS) {
        Lose(poller, now, "the mux sent descriptors without handoffs");
    }
}

void MuxLink::Write(io::Poller& poller, Timestamp now) noexcept {
    while (!Output_.Empty()) {
        const auto n = io::WriteSome(Socket_.Get(), Output_.Front());
        if (!n) {
            if (!n.error().WouldBlock()) {
                Lose(poller, now, n.error().Description());
            }
            return;
        }
        Output_.Consume(*n);
    }
}

void MuxLink::Lose(io::Poller& poller, Timestamp now,
                   std::string_view why) noexcept {
    const std::array fields{log::Field::Text("reason", why)};
    Log(log::ELevel::Warn, "lost the mux; registering again", fields);
    Close(poller);
    RetryAt_ = Deadline{now + Backoff()};
}

void MuxLink::Close(io::Poller& poller) noexcept {
    if (Watch_) {
        Watch_->Forget(poller);
        Watch_.reset();
    }
    Socket_.Reset();
    // Unread handoffs and their descriptors go too: the clients reconnect.
    Descriptors_.clear();
    Output_.Consume(Output_.Size());
    Registered_ = false;
}

void MuxLink::OnTimer(io::Poller& poller, Timestamp now) noexcept {
    if (Socket_ || !RetryAt_.HasExpired(now)) {
        return;
    }
    RetryAt_ = Deadline{};
    if (auto connected = Connect(); !connected) {
        const std::array fields{log::Field::Text("reason", connected.error())};
        Log(log::ELevel::Info, "the mux is not back yet", fields);
        Close(poller);
        RetryAt_ = Deadline{now + Backoff()};
    }
}

void MuxLink::UpdateInterest(io::Poller& poller, Timestamp now) noexcept {
    if (!Socket_) {
        return;
    }
    if (!Watch_) {
        Watch_.emplace(Socket_.Get(), Token_);
    }
    if (!Watch_->Want(poller, true, !Output_.Empty())) {
        // A link the poller cannot watch is a lost link: Lose schedules the
        // retry that registers the session again.
        Lose(poller, now, "the poller refused the mux socket");
    }
}

Deadline MuxLink::NextDeadline() const noexcept {
    return Socket_ ? Deadline{} : RetryAt_;
}

Duration MuxLink::Backoff() noexcept {
    Duration delay = BACKOFF_MIN;
    for (unsigned i = 0; i < Attempts_ && delay < BACKOFF_MAX; ++i) {
        delay *= 2;
    }
    delay = std::min(delay, BACKOFF_MAX);
    ++Attempts_;
    // Jitter over the upper half, so that agents cut by the same restart do
    // not come back in step.
    std::array<std::byte, sizeof(std::uint64_t)> noiseBytes{};
    crypto::RandomBytes(MutableByteSpan{noiseBytes});
    std::uint64_t noise = 0;
    for (const std::byte b : noiseBytes) {
        constexpr unsigned byteBits = 8;
        noise = (noise << byteBits) | std::to_integer<std::uint64_t>(b);
    }
    const auto half = delay.count() / 2;
    return Duration{half + static_cast<Duration::rep>(
                               noise % static_cast<std::uint64_t>(half + 1))};
}

}  // namespace zet::agent
