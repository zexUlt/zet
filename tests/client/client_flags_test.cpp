/// The --predict flag of zet (design.md §9): a value other than adaptive,
/// always or never, or no value at all, is a usage error before anything
/// else happens; the three values and both spellings are accepted.
///
/// Main stops at --version after the options are read, which is how a valid
/// line is told from an invalid one without a session.
#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

#include "client.hpp"

namespace {

constexpr int USAGE = 2;

int Run(std::vector<std::string> args) {
    args.insert(args.begin(), "zet");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    return zet::client::Main(std::span<char*>{argv});
}

}  // namespace

TEST_CASE("--predict accepts adaptive, always and never, in both spellings") {
    for (const char* mode : {"adaptive", "always", "never"}) {
        CHECK(Run({std::string{"--predict="} + mode, "--version"}) == 0);
        CHECK(Run({"--predict", mode, "--version"}) == 0);
    }
}

TEST_CASE("--predict with another value or none is a usage error") {
    CHECK(Run({"--predict=bogus", "--version"}) == USAGE);
    CHECK(Run({"--predict=", "--version"}) == USAGE);
    CHECK(Run({"--predict=Never", "--version"}) == USAGE);
    CHECK(Run({"--predict", "bogus", "--version"}) == USAGE);
    CHECK(Run({"--predict", "--version"}) == USAGE);  // eats --version
    CHECK(Run({"host", "--predict"}) == USAGE);       // no value
    CHECK(Run({"--predict"}) == USAGE);
}
