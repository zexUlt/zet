/// The child the pty contract and the 64 MiB burst run against: a program
/// whose behaviour on a terminal is known exactly, which a shell's is not.
///
///     zet_pty_echo echo      raw mode, then every byte read is written back;
///                            a SIGWINCH is answered with WINSIZE_REPORT
///     zet_pty_echo emit <n>  raw mode, then n bytes of PatternByte, then exit
///
/// Both print READY_MARKER once raw mode is on: bytes written before that
/// would meet the line discipline's echo and its line editing instead.

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "pty_echo_protocol.hpp"

namespace {

using namespace zet::io::test;

volatile std::sig_atomic_t resized = 0;

void OnWindowChange(int /*signal*/) { resized = 1; }

/// Writes all of `bytes` to stdout, or reports why it could not.
bool WriteAll(const char* bytes, std::size_t length) {
    while (length > 0) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes, length);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

bool MakeRaw() {
    termios mode{};
    if (::tcgetattr(STDIN_FILENO, &mode) != 0) {
        return false;
    }
    ::cfmakeraw(&mode);
    return ::tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
}

bool ReportSize() {
    winsize size{};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) {
        return false;
    }
    std::array<char, WINSIZE_REPORT_MAX> report{};
    const int length =
        std::snprintf(report.data(), report.size(), WINSIZE_REPORT_FORMAT,
                      size.ws_row, size.ws_col);
    return length > 0 &&
           WriteAll(report.data(), static_cast<std::size_t>(length));
}

int Echo() {
    // Without SA_RESTART, so that a read blocked on an idle terminal returns
    // with EINTR and the report goes out at once.
    struct sigaction action{};
    action.sa_handler = OnWindowChange;
    if (::sigaction(SIGWINCH, &action, nullptr) != 0) {
        return EXIT_FAILURE;
    }
    if (!MakeRaw() || !WriteAll(READY_MARKER.data(), READY_MARKER.size())) {
        return EXIT_FAILURE;
    }
    std::array<char, CHILD_BUFFER_SIZE> buffer{};
    for (;;) {
        if (resized != 0) {
            resized = 0;
            if (!ReportSize()) {
                return EXIT_FAILURE;
            }
        }
        const ssize_t n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            // The master is gone: EIO on Linux, or end of file.
            return EXIT_SUCCESS;
        }
        if (!WriteAll(buffer.data(), static_cast<std::size_t>(n))) {
            return EXIT_SUCCESS;
        }
    }
}

int Emit(const char* count) {
    char* end = nullptr;
    const unsigned long long total = std::strtoull(count, &end, 10);
    if (end == count || *end != '\0') {
        return EXIT_FAILURE;
    }
    if (!MakeRaw() || !WriteAll(READY_MARKER.data(), READY_MARKER.size())) {
        return EXIT_FAILURE;
    }
    std::array<char, CHILD_BUFFER_SIZE> buffer{};
    std::size_t sent = 0;
    while (sent < total) {
        const std::size_t length =
            std::min<std::size_t>(buffer.size(), total - sent);
        for (std::size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<char>(PatternByte(sent + i));
        }
        if (!WriteAll(buffer.data(), length)) {
            return EXIT_FAILURE;
        }
        sent += length;
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "echo" && argc == 2) {
        return Echo();
    }
    if (mode == "emit" && argc == 3) {
        return Emit(argv[2]);
    }
    std::fputs("usage: zet_pty_echo echo | emit <bytes>\n", stderr);
    return EXIT_FAILURE;
}
