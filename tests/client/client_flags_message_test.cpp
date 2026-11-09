/// A bad --predict is reported by name before the usage text (design.md §9),
/// as the other errors of the client are: "zet: <what>: <why>" on stderr, then
/// the usage, status 2.
#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "client.hpp"

namespace {

struct Result {
    int Status;
    std::string Stderr;
};

/// Main with `args`, and what it wrote to stderr.
Result Run(std::vector<std::string> args) {
    args.insert(args.begin(), "zet");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    char path[] = "/tmp/zet_flags_stderr_XXXXXX";
    const int file = ::mkstemp(path);
    REQUIRE(file >= 0);
    ::unlink(path);
    const int saved = ::dup(STDERR_FILENO);
    REQUIRE(saved >= 0);
    REQUIRE(::dup2(file, STDERR_FILENO) >= 0);
    const int status = zet::client::Main(std::span<char*>{argv});
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::string text(4096, '\0');
    const ssize_t n = ::pread(file, text.data(), text.size(), 0);
    text.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    ::close(file);
    return {status, text};
}

}  // namespace

TEST_CASE("--predict with a bad value names it, then prints the usage") {
    const Result r = Run({"--predict=sometimes", "host"});
    CHECK(r.Status == 2);
    const auto named = r.Stderr.find("'sometimes'");
    const auto usage = r.Stderr.find("usage:");
    REQUIRE(named != std::string::npos);
    REQUIRE(usage != std::string::npos);
    CHECK(named < usage);
    CHECK(r.Stderr.starts_with("zet: --predict: "));
}

TEST_CASE("--predict with a value of two words spells it with the space form") {
    const Result r = Run({"--predict", "Always", "host"});
    CHECK(r.Status == 2);
    CHECK(r.Stderr.find("'Always'") != std::string::npos);
}

TEST_CASE("--predict with no value at the end says so") {
    const Result r = Run({"host", "--predict"});
    CHECK(r.Status == 2);
    CHECK(r.Stderr.starts_with("zet: --predict: needs a mode"));
    CHECK(r.Stderr.find("usage:") != std::string::npos);
}

TEST_CASE("a value outside printable ASCII is not echoed to the terminal") {
    const Result r = Run({"--predict=\x1b[31mred", "host"});
    CHECK(r.Status == 2);
    CHECK(r.Stderr.find('\x1b') == std::string::npos);
}
