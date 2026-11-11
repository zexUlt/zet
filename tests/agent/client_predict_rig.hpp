/// The real client (RunSessionForTest) against the real agent
/// (RunAgentForTest) over TCP and a real pty, for the predictive echo (M7,
/// design.md §9). The test plays the user (keystrokes into the client's stdin)
/// and the shell (what the agent's pty gives it, what it prints back), and
/// reads what the client wrote to its terminal.
///
/// A TCP proxy sits between the two. It records the client's Hello, and can
/// swallow the agent's side of the stream and cut the connection, which is how
/// a test loses an EchoAck on the way.
///
/// The ZET_TEST_OVERLAY_LOG hook ("offset length" lines) says which stretches
/// of the client's stdout are overlay: removing them leaves the agent's stream.
#pragma once

#include "../../src/agent/pump_test_api.hpp"
#include "../../src/client/pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
#include <csignal>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/tcp.hpp"
#include "zet/predict/engine.hpp"

namespace {

using namespace std::chrono_literals;

/// One stretch of the client's stdout that the overlay put there.
struct OverlayInsert {
    std::size_t Offset;
    std::size_t Length;
};

/// Sits between the client and the agent.
class SpyProxy {
public:
    SpyProxy(io::Listener listener, std::uint16_t agentPort)
        : Listener_(std::move(listener)), AgentPort_(agentPort) {
        Thread_ = std::thread{[this] { Run(); }};
    }
    ~SpyProxy() { Stop(); }

    [[nodiscard]] std::uint16_t Port() const { return Listener_.Port; }

    void Stop() {
        Stop_ = true;
        if (Thread_.joinable()) {
            Thread_.join();
        }
    }

    /// The agent's bytes are read and thrown away from now on.
    void Blackhole(bool on) { Blackhole_ = on; }
    /// Closes the connection now; the client reconnects.
    void Cut() { Cut_ = true; }
    [[nodiscard]] int Connections() const { return Connections_; }
    /// The first bytes the client sent on its first connection.
    [[nodiscard]] std::vector<std::byte> Hello() {
        const std::lock_guard lock{Mutex_};
        return Hello_;
    }

private:
    void Run() {
        while (!Stop_) {
            pollfd wait{
                .fd = Listener_.Socket.Get(), .events = POLLIN, .revents = 0};
            if (::poll(&wait, 1, 20) <= 0) {
                continue;
            }
            auto client = io::Accept(Listener_.Socket.Get());
            if (!client) {
                continue;
            }
            auto addresses = io::Resolve("127.0.0.1", AgentPort_);
            if (!addresses || addresses->Count == 0) {
                continue;
            }
            auto agent = io::StartConnect(addresses->Items[0]);
            if (!agent) {
                continue;
            }
            pollfd connecting{
                .fd = agent->Get(), .events = POLLOUT, .revents = 0};
            if (::poll(&connecting, 1, 2000) <= 0 ||
                !io::FinishConnect(agent->Get())) {
                continue;
            }
            ++Connections_;
            Cut_ = false;
            Pipe(*client, *agent);
        }
    }

    void Pipe(io::UniqueFd& client, io::UniqueFd& agent) {
        std::vector<std::byte> toAgent;
        std::vector<std::byte> toClient;
        std::array<std::byte, 4096> buf{};
        while (!Stop_ && !Cut_) {
            pollfd fds[2] = {
                {.fd = client.Get(),
                 .events =
                     static_cast<short>((toAgent.size() < 65536 ? POLLIN : 0) |
                                        (toClient.empty() ? 0 : POLLOUT)),
                 .revents = 0},
                {.fd = agent.Get(),
                 .events =
                     static_cast<short>((toClient.size() < 65536 ? POLLIN : 0) |
                                        (toAgent.empty() ? 0 : POLLOUT)),
                 .revents = 0},
            };
            if (::poll(fds, 2, 10) < 0) {
                return;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                const auto n = io::ReadSome(client.Get(), MutableByteSpan{buf});
                if (n && *n == 0) return;
                if (n) {
                    toAgent.insert(
                        toAgent.end(), buf.begin(),
                        buf.begin() + static_cast<std::ptrdiff_t>(*n));
                    const std::lock_guard lock{Mutex_};
                    if (Connections_ == 1 && Hello_.size() < 512) {
                        Hello_.insert(
                            Hello_.end(), buf.begin(),
                            buf.begin() + static_cast<std::ptrdiff_t>(*n));
                    }
                } else if (!n.error().WouldBlock()) {
                    return;
                }
            }
            if ((fds[1].revents & POLLIN) != 0) {
                const auto n = io::ReadSome(agent.Get(), MutableByteSpan{buf});
                if (n && *n == 0) return;
                if (n) {
                    if (!Blackhole_) {
                        toClient.insert(
                            toClient.end(), buf.begin(),
                            buf.begin() + static_cast<std::ptrdiff_t>(*n));
                    }
                } else if (!n.error().WouldBlock()) {
                    return;
                }
            }
            if (!toClient.empty() && (fds[0].revents & POLLOUT) != 0) {
                const auto n = io::WriteSome(client.Get(), ByteSpan{toClient});
                if (n)
                    toClient.erase(
                        toClient.begin(),
                        toClient.begin() + static_cast<std::ptrdiff_t>(*n));
            }
            if (!toAgent.empty() && (fds[1].revents & POLLOUT) != 0) {
                const auto n = io::WriteSome(agent.Get(), ByteSpan{toAgent});
                if (n)
                    toAgent.erase(
                        toAgent.begin(),
                        toAgent.begin() + static_cast<std::ptrdiff_t>(*n));
            }
            if ((fds[0].revents | fds[1].revents) &
                (POLLERR | POLLHUP | POLLNVAL)) {
                // A closed side: what it said is already read above.
                if (((fds[0].revents | fds[1].revents) & POLLIN) == 0) return;
            }
        }
    }

    io::Listener Listener_;
    std::uint16_t AgentPort_;
    std::thread Thread_;
    std::atomic<bool> Stop_{false};
    std::atomic<bool> Blackhole_{false};
    std::atomic<bool> Cut_{false};
    std::atomic<int> Connections_{0};
    std::mutex Mutex_;
    std::vector<std::byte> Hello_;
};

/// A terminal: what a stream of bytes leaves on a screen.
struct ScreenOf {
    std::unique_ptr<zet::grid::Grid> Grid;
    zet::grid::VtParser Parser;

    explicit ScreenOf(std::string_view bytes) {
        auto made = zet::grid::Grid::Create(40, 6, 0);
        REQUIRE(made.has_value());
        Grid.reset(made.value());
        Parser.Feed(reinterpret_cast<const std::byte*>(bytes.data()),
                    bytes.size(), *Grid);
    }

    [[nodiscard]] std::string Row(std::uint16_t row) const {
        std::string s;
        for (std::uint16_t c = 0; c < Grid->Cols(); ++c) {
            const char32_t cp = Grid->CellAt(row, c).Codepoint;
            s.push_back(cp == 0 ? ' ' : static_cast<char>(cp));
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
};

/// Holds SIGWINCH back from the threads this one starts, so that a signal sent
/// to the process waits for the client's signalfd.
struct BlockWinch {
    BlockWinch() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGWINCH);
        ::pthread_sigmask(SIG_BLOCK, &set, &Old);
    }
    ~BlockWinch() { ::pthread_sigmask(SIG_SETMASK, &Old, nullptr); }
    BlockWinch(const BlockWinch&) = delete;
    BlockWinch& operator=(const BlockWinch&) = delete;
    sigset_t Old{};
};

class ClientRig {
public:
    struct Options {
        zet::predict::EDisplayMode Mode{zet::predict::EDisplayMode::Always};
        std::uint16_t Ceiling{wire::PROTOCOL_VERSION_GRID};
        /// How long the client keeps reconnecting once the agent is gone.
        std::chrono::milliseconds GiveUp{1500};
        /// Notices go to stdout, through the overlay's undo, as on a tty.
        bool NoticesInline{false};
        /// Reconnect path (ZET_TEST_FORCE_PATH).
        session::EForcePath ForcePath{session::EForcePath::None};
        /// Answers SIGWINCH; empty means the tty, which the test has none.
        std::function<std::optional<wire::ResizeMessage>()> SizeSource{};
        /// Stdout is a regular file, which the poller refuses to watch: the
        /// client fails ("the terminal went away") as soon as it has bytes
        /// queued for it, and its queue is then as Finish finds it.
        bool StdoutIsFile{false};
        /// The client asks the terminal where its cursor is (round C).
        bool Calibrate{false};
        /// The test's terminal answers the query: `CSI r;c R CSI ? 6 2 c`.
        bool Answer{true};
        /// With Answer: only this many queries are answered, the rest meet a
        /// silent terminal.
        std::size_t AnswerMax{~std::size_t{0}};
        /// The client cannot rebuild its mirror from this snapshot (from 1).
        unsigned LoseMirrorAtSnapshot{0};
    };

    ClientRig(std::uint8_t seed, Options options)
        : Seed_(seed), Options_(options) {
        Limits_ = TinyLimits();
        Limits_.ResendMax = 65536;
        Limits_.DisconnectMax = 65536;
        Limits_.ForcePath = Options_.ForcePath;

        auto pty = io::OpenPty();
        REQUIRE(pty.has_value());
        termios t{};
        REQUIRE(::tcgetattr(pty->Slave.Get(), &t) == 0);
        ::cfmakeraw(&t);
        REQUIRE(::tcsetattr(pty->Slave.Get(), TCSANOW, &t) == 0);
        REQUIRE(io::SetNonBlocking(pty->Slave.Get()).has_value());
        Slave_ = std::move(pty->Slave);
        Master_ = std::move(pty->Master);

        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        AgentPort_ = listener->Port;
        auto agentAddr = io::Resolve("127.0.0.1", AgentPort_);
        REQUIRE(agentAddr.has_value());
        Wake_ = agentAddr->Items[0];
        auto proxyListener = io::ListenTcp(0);
        REQUIRE(proxyListener.has_value());
        Proxy_ =
            std::make_unique<SpyProxy>(std::move(*proxyListener), AgentPort_);
        auto addresses = io::Resolve("127.0.0.1", Proxy_->Port());
        REQUIRE(addresses.has_value());

        auto in = io::MakePipe();
        REQUIRE(in.has_value());
        REQUIRE(io::SetNonBlocking(in->Read.Get()).has_value());
        StdinRead_ = std::move(in->Read);
        StdinWrite_ = std::move(in->Write);
        auto out = io::MakePipe();
        REQUIRE(out.has_value());
        REQUIRE(io::SetNonBlocking(out->Read.Get()).has_value());
        REQUIRE(io::SetNonBlocking(out->Write.Get()).has_value());
        StdoutRead_ = std::move(out->Read);
        StdoutWrite_ = std::move(out->Write);
        if (Options_.StdoutIsFile) {
            char path[] = "/tmp/zet_predict_stdout_XXXXXX";
            io::UniqueFd file{::mkstemp(path)};
            REQUIRE(file.Get() >= 0);
            io::UniqueFd reader{::open(path, O_RDONLY)};
            REQUIRE(reader.Get() >= 0);
            ::unlink(path);
            StdoutWrite_ = std::move(file);
            StdoutRead_ = std::move(reader);
        }
        char errPath[] = "/tmp/zet_predict_stderr_XXXXXX";
        Stderr_ = io::UniqueFd{::mkstemp(errPath)};
        REQUIRE(Stderr_.Get() >= 0);
        ::unlink(errPath);

        char logPath[] = "/tmp/zet_overlay_log_XXXXXX";
        io::UniqueFd logFd{::mkstemp(logPath)};
        REQUIRE(logFd.Get() >= 0);
        LogPath_ = logPath;
        ::unlink(logPath);  // the client creates it again, if it logs

        Agent_ = std::thread{[this, l = std::move(*listener)]() mutable {
            std::ignore = RunAgentForTest(agent::AgentTestConfig{
                .Secrets = wire::DeriveSessionSecrets(MakeKey(Seed_)),
                .Limits = Limits_,
                .Listen = std::move(l),
                .PtyReadFd = std::move(Master_),
                .Ceiling = Options_.Ceiling,
                .GiveUpAfter = std::chrono::seconds{120},
                .Inspect = nullptr,
                .Stop = &StopAgent_,
                .InputToPty = true,
            });
        }};
        Client_ = std::async(std::launch::async, [this, a = *addresses] {
            return client::RunSessionForTest(client::SessionTestConfig{
                .Secrets = wire::DeriveSessionSecrets(MakeKey(Seed_)),
                .Limits = Limits_,
                .Addresses = a,
                .StdoutFd = StdoutWrite_.Get(),
                .StderrFd = Stderr_.Get(),
                .StdinFd = StdinRead_.Get(),
                .NoticesInline = Options_.NoticesInline,
                .Ceiling = Options_.Ceiling,
                .BackoffMax = 20ms,
                .GiveUpAfter = Options_.GiveUp,
                .Predict = Options_.Mode,
                .OverlayLog = LogPath_,
                .Calibrate = Options_.Calibrate,
                .SizeSource = Options_.SizeSource,
                .LoseMirrorAtSnapshot = Options_.LoseMirrorAtSnapshot,
            });
        });
    }

    ~ClientRig() { Finish(); }

    ClientRig(const ClientRig&) = delete;
    ClientRig& operator=(const ClientRig&) = delete;

    /// The user types.
    void Keys(std::string_view keys) {
        std::size_t done = 0;
        while (done < keys.size()) {
            const auto n = io::WriteSome(
                StdinWrite_.Get(),
                ByteSpan{reinterpret_cast<const std::byte*>(keys.data()) + done,
                         keys.size() - done});
            if (n) {
                done += *n;
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    /// The shell prints.
    void Print(std::string_view text) {
        std::size_t done = 0;
        while (done < text.size()) {
            const auto n = io::WriteSome(
                Slave_.Get(),
                ByteSpan{reinterpret_cast<const std::byte*>(text.data()) + done,
                         text.size() - done});
            if (n) {
                done += *n;
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        AgentStream_.append(text);
    }

    /// The shell prints from another thread, for output the pipeline cannot
    /// take at once (the client's terminal is not read). Ends with Finish.
    void PrintAsync(std::string text) {
        PrinterTotal_ = text.size();
        Printer_ = std::thread{[this, t = std::move(text)] {
            std::size_t done = 0;
            while (done < t.size() && !PrinterStop_) {
                const auto n = io::WriteSome(
                    Slave_.Get(),
                    ByteSpan{
                        reinterpret_cast<const std::byte*>(t.data()) + done,
                        t.size() - done});
                if (n) {
                    done += *n;
                    PrinterDone_ = done;
                } else {
                    std::this_thread::sleep_for(1ms);
                }
            }
        }};
    }

    /// How much of PrintAsync's text has been taken by the pty, and its size.
    [[nodiscard]] std::size_t PrinterDone() const { return PrinterDone_; }
    [[nodiscard]] std::size_t PrinterTotal() const { return PrinterTotal_; }

    /// The terminal takes at most `bytes` of what the client wrote, then the
    /// test looks at the rest as Pump does. For a terminal that is slow.
    void DrainSome(std::size_t bytes) {
        std::array<std::byte, 4096> buf{};
        const auto n = io::ReadSome(
            StdoutRead_.Get(),
            MutableByteSpan{buf}.first(std::min(bytes, buf.size())));
        if (n && *n != 0) {
            Out_.append(reinterpret_cast<const char*>(buf.data()), *n);
        }
        AnswerQueries();
        for (;;) {
            const auto m = io::ReadSome(Slave_.Get(), MutableByteSpan{buf});
            if (!m || *m == 0) break;
            ShellIn_.append(reinterpret_cast<const char*>(buf.data()), *m);
        }
    }

    /// Lets time pass, reading what the client writes and what the shell is
    /// given.
    void Pump(std::chrono::milliseconds span) {
        const auto until = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < until) {
            Drain();
            std::this_thread::sleep_for(2ms);
        }
        Drain();
    }

    bool WaitFor(const std::function<bool()>& done,
                 std::chrono::milliseconds limit = 5000ms) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until) {
            Drain();
            if (done()) return true;
            std::this_thread::sleep_for(2ms);
        }
        Drain();
        return done();
    }

    /// What the shell has been given so far (the agent's pty input).
    [[nodiscard]] const std::string& ShellInput() const { return ShellIn_; }
    /// Everything the client has written to its terminal.
    [[nodiscard]] const std::string& Out() const { return Out_; }
    /// Everything the shell has printed: what the agent's stream is.
    [[nodiscard]] const std::string& AgentStream() const {
        return AgentStream_;
    }

    /// The user types a key, the shell reads it and prints it back, and the
    /// echo-ack has time to come: one confirming exchange.
    void TypeAndEcho(char key) {
        const std::size_t seen = ShellIn_.size();
        Keys(std::string_view{&key, 1});
        REQUIRE(WaitFor([&] { return ShellIn_.size() > seen; }));
        Print(std::string_view{&key, 1});
        Pump(350ms);
    }

    /// A prompt after the user's first Enter: the client is connected by then,
    /// and the agent's opening EchoAck, which comes first, has been taken. Two
    /// echoed keys of different glyphs confirm an epoch. The terminal then
    /// reads "$ aB" with the cursor after it.
    void Warm() {
        Keys("\n");
        REQUIRE(
            WaitFor([&] { return ShellIn_.find('\n') != std::string::npos; }));
        Print("\r$ ");
        Pump(100ms);
        TypeAndEcho('a');
        TypeAndEcho('B');
    }

    [[nodiscard]] std::vector<OverlayInsert> Inserts() {
        std::vector<OverlayInsert> all;
        const int fd = ::open(LogPath_.c_str(), O_RDONLY);
        if (fd < 0) return all;
        std::string text;
        char buf[512];
        for (ssize_t n; (n = ::read(fd, buf, sizeof(buf))) > 0;) {
            text.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
        std::size_t at = 0;
        while (at < text.size()) {
            char* end = nullptr;
            const auto offset = std::strtoull(text.c_str() + at, &end, 10);
            const auto length = std::strtoull(end, &end, 10);
            // The client logs an insert when it queues it and writes it a turn
            // later: one that has not reached Out_ yet is not seen yet, or a
            // test reading its bytes would find them cut short.
            if (offset + length <= Out_.size()) {
                all.push_back({offset, length});
            }
            at = static_cast<std::size_t>(end - text.c_str()) + 1;
        }
        return all;
    }

    /// The client's stdout without the overlay: the agent's stream, if the
    /// overlay only ever inserted.
    [[nodiscard]] std::string WithoutOverlay() {
        std::string kept;
        std::size_t at = 0;
        for (const OverlayInsert& i : Inserts()) {
            REQUIRE(i.Offset >= at);
            REQUIRE(i.Offset + i.Length <= Out_.size());
            kept.append(Out_, at, i.Offset - at);
            at = i.Offset + i.Length;
        }
        kept.append(Out_, at, std::string::npos);
        return kept;
    }

    [[nodiscard]] std::string InsertBytes(const OverlayInsert& i) const {
        return Out_.substr(i.Offset, i.Length);
    }

    [[nodiscard]] SpyProxy& Proxy() { return *Proxy_; }

    /// The client session has ended on its own.
    [[nodiscard]] bool ClientEnded() {
        return Client_.valid() &&
               Client_.wait_for(0ms) == std::future_status::ready;
    }

    /// How the client session ended; valid after Finish.
    [[nodiscard]] int ClientStatus() const { return Status_; }

    /// Whether the client built the stdin demultiplexer; valid after Finish.
    [[nodiscard]] bool CalibrationCreated() const {
        return CalibrationCreated_;
    }
    /// Queries the client wrote to its terminal; valid after Finish.
    [[nodiscard]] std::uint32_t QueriesWritten() const {
        return QueriesWritten_;
    }
    /// Queries seen in the client's stdout so far.
    [[nodiscard]] std::size_t QueriesSeen() const { return QueriesSeen_; }

    /// What the client wrote to its stderr.
    [[nodiscard]] std::string ErrText() const {
        std::string text(4096, '\0');
        const ssize_t n = ::pread(Stderr_.Get(), text.data(), text.size(), 0);
        text.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        return text;
    }

    /// Whether the client's Hello carried the TLV of `type` with the value 1.
    [[nodiscard]] bool HelloOffers(std::uint16_t type) {
        const auto hello = Proxy_->Hello();
        const std::byte want[6] = {static_cast<std::byte>(type >> 8),
                                   static_cast<std::byte>(type & 0xFF),
                                   std::byte{0},
                                   std::byte{2},
                                   std::byte{0},
                                   std::byte{1}};
        for (std::size_t i = 0; i + 6 <= hello.size(); ++i) {
            if (std::equal(want, want + 6,
                           hello.begin() + static_cast<std::ptrdiff_t>(i))) {
                return true;
            }
        }
        return false;
    }

    /// Ends the session: stops the agent (its listener closing is what makes
    /// the client give up), waits for the client, and takes what it wrote.
    void Finish() {
        if (std::exchange(Done_, true)) return;
        PrinterStop_ = true;
        if (Printer_.joinable()) Printer_.join();
        StopAgent_ = true;
        if (Agent_.joinable()) {
            auto waker = io::StartConnect(Wake_);
            std::this_thread::sleep_for(100ms);
            Agent_.join();
        }
        Proxy_->Stop();
        if (Client_.valid()) {
            const auto until = std::chrono::steady_clock::now() + 15s;
            while (Client_.wait_for(5ms) != std::future_status::ready) {
                Drain();
                if (std::chrono::steady_clock::now() > until) {
                    FAIL_CHECK("the client session did not end");
                    std::quick_exit(1);
                }
            }
            const auto result = Client_.get();
            Status_ = result.Status;
            CalibrationCreated_ = result.CalibrationCreated;
            QueriesWritten_ = result.QueriesWritten;
        }
        Drain();
    }

private:
    void Drain() {
        std::array<std::byte, 4096> buf{};
        for (;;) {
            const auto n =
                io::ReadSome(StdoutRead_.Get(), MutableByteSpan{buf});
            if (!n || *n == 0) break;
            Out_.append(reinterpret_cast<const char*>(buf.data()), *n);
        }
        AnswerQueries();
        for (;;) {
            const auto n = io::ReadSome(Slave_.Get(), MutableByteSpan{buf});
            if (!n || *n == 0) break;
            ShellIn_.append(reinterpret_cast<const char*>(buf.data()), *n);
        }
    }

    /// The terminal: follows what the client wrote, answers a query from its
    /// cursor when the query has gone by.
    void AnswerQueries() {
        static constexpr std::string_view QUERY = "\x1b[6n\x1b[c";
        if (!Term_) {
            auto made = zet::grid::Grid::Create(80, 24, 0);
            REQUIRE(made.has_value());
            Term_.reset(made.value());
        }
        for (;;) {
            const std::size_t at = Out_.find(QUERY, TermAt_);
            const std::size_t upTo = at == std::string::npos ? Out_.size() : at;
            // A partial query at the end waits for the rest.
            const std::size_t safe =
                at == std::string::npos && Out_.size() >= TermAt_
                    ? (Out_.size() > QUERY.size() ? Out_.size() - QUERY.size()
                                                  : 0)
                    : upTo;
            const std::size_t feedTo = std::max(safe, TermAt_);
            TermParser_.Feed(
                reinterpret_cast<const std::byte*>(Out_.data()) + TermAt_,
                feedTo - TermAt_, *Term_);
            TermAt_ = feedTo;
            if (at == std::string::npos) return;
            TermAt_ = at + QUERY.size();
            ++QueriesSeen_;
            if (Options_.Answer && QueriesSeen_ <= Options_.AnswerMax) {
                const std::string reply =
                    "\x1b[" + std::to_string(Term_->CursorRow() + 1) + ";" +
                    std::to_string(Term_->CursorCol() + 1) + "R\x1b[?62;c";
                Keys(reply);
            }
        }
    }

    std::uint8_t Seed_;
    Options Options_;
    std::unique_ptr<zet::grid::Grid> Term_;
    zet::grid::VtParser TermParser_;
    std::size_t TermAt_{0};
    std::size_t QueriesSeen_{0};
    bool CalibrationCreated_{false};
    std::uint32_t QueriesWritten_{0};
    session::SessionLimits Limits_;
    io::UniqueFd Slave_;
    io::UniqueFd Master_;
    io::UniqueFd StdinRead_;
    io::UniqueFd StdinWrite_;
    io::UniqueFd StdoutRead_;
    io::UniqueFd StdoutWrite_;
    io::UniqueFd Stderr_;
    std::uint16_t AgentPort_{0};
    io::SocketAddress Wake_;
    std::string LogPath_;
    std::string Out_;
    std::string ShellIn_;
    std::string AgentStream_;
    std::unique_ptr<SpyProxy> Proxy_;
    std::atomic<bool> StopAgent_{false};
    std::thread Agent_;
    std::thread Printer_;
    std::atomic<bool> PrinterStop_{false};
    std::atomic<std::size_t> PrinterDone_{0};
    std::atomic<std::size_t> PrinterTotal_{0};
    std::future<client::SessionTestResult> Client_;
    bool Done_{false};
    int Status_{-1};
};

}  // namespace
