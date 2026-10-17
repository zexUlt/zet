#include "fake_pty.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>

#include "pty_echo_protocol.hpp"

namespace zet::io::test {

namespace {

/// Moves as much of `from` into `to` as `to` has room for under `limit`, and
/// says how much that was.
std::size_t Pour(std::deque<std::byte>& from, std::deque<std::byte>& to,
                 std::size_t limit) {
    const std::size_t room = limit > to.size() ? limit - to.size() : 0;
    const std::size_t n = std::min(room, from.size());
    const auto end = from.begin() + static_cast<std::ptrdiff_t>(n);
    to.insert(to.end(), from.begin(), end);
    from.erase(from.begin(), end);
    return n;
}

}  // namespace

FakePty FakePty::Echo(Capacity capacity) {
    FakePty pty{capacity};
    pty.Echoes_ = true;
    for (const char c : READY_MARKER) {
        pty.Script_.push_back(static_cast<std::byte>(c));
    }
    pty.Settle();
    return pty;
}

FakePty FakePty::Emitter(Capacity capacity, ByteSpan output) {
    FakePty pty{capacity};
    for (const char c : READY_MARKER) {
        pty.Script_.push_back(static_cast<std::byte>(c));
    }
    pty.Script_.insert(pty.Script_.end(), output.begin(), output.end());
    pty.Settle();
    return pty;
}

IoResult<std::size_t> FakePty::Write(ByteSpan bytes) {
    const std::size_t room = Capacity_.Input - Input_.size();
    if (room == 0) {
        return std::unexpected(Errno{.Code = EAGAIN, .Call = "write"});
    }
    const std::size_t n = std::min(room, bytes.size());
    Input_.insert(Input_.end(), bytes.begin(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(n));
    Settle();
    return n;
}

IoResult<std::size_t> FakePty::Read(MutableByteSpan into) {
    if (Output_.empty()) {
        if (Exited_) {
            return 0;
        }
        return std::unexpected(Errno{.Code = EAGAIN, .Call = "read"});
    }
    const std::size_t n = std::min(into.size(), Output_.size());
    std::copy_n(Output_.begin(), n, into.begin());
    Output_.erase(Output_.begin(),
                  Output_.begin() + static_cast<std::ptrdiff_t>(n));
    Settle();
    return n;
}

IoResult<void> FakePty::Resize(WindowSize size) {
    if (Echoes_ && !Exited_) {
        std::array<char, WINSIZE_REPORT_MAX> report{};
        const int length = std::snprintf(
            report.data(), report.size(), WINSIZE_REPORT_FORMAT,
            static_cast<unsigned>(size.Rows), static_cast<unsigned>(size.Cols));
        for (int i = 0; i < length; ++i) {
            Script_.push_back(
                static_cast<std::byte>(report[static_cast<std::size_t>(i)]));
        }
        Settle();
    }
    return {};
}

bool FakePty::Readable() const noexcept { return !Output_.empty() || Exited_; }

bool FakePty::Writable() const noexcept {
    return Input_.size() < Capacity_.Input;
}

void FakePty::Settle() {
    for (;;) {
        // What the child says on its own goes out before it echoes anything
        // more, as the real one writes its report between two reads.
        std::size_t moved = Pour(Script_, Output_, Capacity_.Output);
        if (Script_.empty()) {
            moved += Pour(Child_, Output_, Capacity_.Output);
            if (Echoes_) {
                moved += Pour(Input_, Child_, Capacity_.Child);
            }
        }
        if (moved == 0) {
            break;
        }
    }
    if (!Echoes_ && Script_.empty()) {
        Exited_ = true;
    }
}

}  // namespace zet::io::test
