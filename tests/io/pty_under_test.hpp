#pragma once

#include <doctest/doctest.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fake_pty.hpp"
#include "pty_echo_protocol.hpp"
#include "pty_test_support.hpp"
#include "zet/io/spawn.hpp"

/// One interface over a real pty with zet_pty_echo on it and over FakePty, so
/// that the contract in pty_contract_test.cpp is the same code for both.
namespace zet::io::test {

class PtyUnderTest {
public:
    PtyUnderTest() = default;
    virtual ~PtyUnderTest() = default;
    PtyUnderTest(const PtyUnderTest&) = delete;
    PtyUnderTest& operator=(const PtyUnderTest&) = delete;
    PtyUnderTest(PtyUnderTest&&) = delete;
    PtyUnderTest& operator=(PtyUnderTest&&) = delete;

    [[nodiscard]] virtual IoResult<std::size_t> Write(ByteSpan bytes) = 0;
    /// 0 is the end of the stream, as ReadPty has it.
    [[nodiscard]] virtual IoResult<std::size_t> Read(MutableByteSpan into) = 0;
    [[nodiscard]] virtual IoResult<void> Resize(WindowSize size) = 0;

    /// Each waits at most `limit` and says whether the condition came. The
    /// fake answers at once: nothing in it moves while the test is not
    /// calling it.
    [[nodiscard]] virtual bool WaitReadable(
        std::chrono::milliseconds limit) = 0;
    [[nodiscard]] virtual bool WaitWritable(
        std::chrono::milliseconds limit) = 0;
    /// The child has closed its end: exited, here.
    [[nodiscard]] virtual bool WaitHangup(std::chrono::milliseconds limit) = 0;
};

/// The path CMake built zet_pty_echo at.
inline constexpr const char* ECHO_CHILD_PATH = ZET_PTY_ECHO_PATH;

inline constexpr WindowSize CONTRACT_SIZE{.Rows = 24, .Cols = 80};

class RealPty final : public PtyUnderTest {
public:
    /// Starts zet_pty_echo with `argv` (its own name first, nullptr last).
    template <std::size_t length>
    explicit RealPty(const std::array<const char*, length>& argv) {
        constexpr std::array<const char*, 1> environment{nullptr};
        auto child = Spawn(SpawnSpec{.Path = ECHO_CHILD_PATH,
                                     .Argv = argv,
                                     .Envp = environment,
                                     .Size = CONTRACT_SIZE});
        REQUIRE(child.has_value());
        Child_ = std::move(*child);
    }

    ~RealPty() override {
        Child_.Master.Reset();
        ::kill(Child_.Pid, SIGKILL);
        int status = 0;
        ::waitpid(Child_.Pid, &status, 0);
    }
    RealPty(const RealPty&) = delete;
    RealPty& operator=(const RealPty&) = delete;
    RealPty(RealPty&&) = delete;
    RealPty& operator=(RealPty&&) = delete;

    IoResult<std::size_t> Write(ByteSpan bytes) override {
        return WriteSome(Child_.Master.Get(), bytes);
    }
    IoResult<std::size_t> Read(MutableByteSpan into) override {
        return ReadPty(Child_.Master.Get(), into);
    }
    IoResult<void> Resize(WindowSize size) override {
        return io::Resize(Child_.Master.Get(), size);
    }
    bool WaitReadable(std::chrono::milliseconds limit) override {
        return PollFor(POLLIN, limit);
    }
    bool WaitWritable(std::chrono::milliseconds limit) override {
        return PollFor(POLLOUT, limit);
    }
    bool WaitHangup(std::chrono::milliseconds limit) override {
        // Asked for nothing, poll still reports a hangup; asking for POLLIN
        // would return at once on output the child left unread.
        return PollFor(0, limit);
    }

    /// For a test that drives the master through its own Poller.
    [[nodiscard]] int Master() const noexcept { return Child_.Master.Get(); }

private:
    bool PollFor(short events, std::chrono::milliseconds limit) {
        pollfd waiting{
            .fd = Child_.Master.Get(), .events = events, .revents = 0};
        for (;;) {
            const int ready =
                ::poll(&waiting, 1, static_cast<int>(limit.count()));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return false;
            }
            if (events == 0) {
                // Only the two poll reports unasked; an error on the master
                // ends the child's side as surely as a hangup does.
                return (waiting.revents & (POLLHUP | POLLERR)) != 0;
            }
            // A hangup wakes a wait for either direction, and the read or
            // write that follows is what tells the caller about it.
            return true;
        }
    }

    Child Child_;
};

class FakePtyUnderTest final : public PtyUnderTest {
public:
    explicit FakePtyUnderTest(FakePty pty) : Pty_(std::move(pty)) {}

    IoResult<std::size_t> Write(ByteSpan bytes) override {
        return Pty_.Write(bytes);
    }
    IoResult<std::size_t> Read(MutableByteSpan into) override {
        return Pty_.Read(into);
    }
    IoResult<void> Resize(WindowSize size) override {
        return Pty_.Resize(size);
    }
    bool WaitReadable(std::chrono::milliseconds /*limit*/) override {
        return Pty_.Readable();
    }
    bool WaitWritable(std::chrono::milliseconds /*limit*/) override {
        return Pty_.Writable();
    }
    bool WaitHangup(std::chrono::milliseconds /*limit*/) override {
        return Pty_.HungUp();
    }

private:
    FakePty Pty_;
};

/// The capacities the fake runs the contract with: the child's buffer is
/// zet_pty_echo's, the kernel's are smaller than Linux's on purpose, so a
/// test that only passes on generous buffers fails on the fake first.
inline constexpr FakePty::Capacity FAKE_CAPACITY{
    .Input = 4096, .Child = CHILD_BUFFER_SIZE, .Output = 4096};

struct RealFactory {
    static std::unique_ptr<PtyUnderTest> Echo() {
        return std::make_unique<RealPty>(
            std::array<const char*, 3>{ECHO_CHILD_PATH, "echo", nullptr});
    }
    static std::unique_ptr<PtyUnderTest> Emitter(std::size_t bytes) {
        const std::string count = std::to_string(bytes);
        return std::make_unique<RealPty>(std::array<const char*, 4>{
            ECHO_CHILD_PATH, "emit", count.c_str(), nullptr});
    }
};

struct FakeFactory {
    static std::unique_ptr<PtyUnderTest> Echo() {
        return std::make_unique<FakePtyUnderTest>(FakePty::Echo(FAKE_CAPACITY));
    }
    static std::unique_ptr<PtyUnderTest> Emitter(std::size_t bytes) {
        std::vector<std::byte> output(bytes);
        for (std::size_t i = 0; i < bytes; ++i) {
            output[i] = static_cast<std::byte>(PatternByte(i));
        }
        return std::make_unique<FakePtyUnderTest>(
            FakePty::Emitter(FAKE_CAPACITY, ByteSpan{output}));
    }
};

/// Reads everything there is right now into `into`. False at the end of the
/// stream.
inline bool Drain(PtyUnderTest& pty, std::vector<std::byte>& into) {
    std::array<std::byte, CHILD_BUFFER_SIZE> chunk{};
    for (;;) {
        const auto n = pty.Read(MutableByteSpan{chunk});
        if (!n) {
            REQUIRE(n.error().WouldBlock());
            return true;
        }
        if (*n == 0) {
            return false;
        }
        into.insert(into.end(), chunk.begin(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(*n));
    }
}

inline std::string_view AsText(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Reads until `needle` has arrived and returns what came after it.
inline std::vector<std::byte> ReadPast(PtyUnderTest& pty,
                                       std::string_view needle) {
    std::vector<std::byte> seen;
    for (;;) {
        const auto at = AsText(seen).find(needle);
        if (at != std::string_view::npos) {
            return {
                seen.begin() + static_cast<std::ptrdiff_t>(at + needle.size()),
                seen.end()};
        }
        REQUIRE_MESSAGE(pty.WaitReadable(PTY_WAIT_TIMEOUT),
                        "waiting for " << needle << ", got " << AsText(seen));
        REQUIRE(Drain(pty, seen));
    }
}

}  // namespace zet::io::test
