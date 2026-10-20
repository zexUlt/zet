# zet

[![CI](https://github.com/zexUlt/zet/actions/workflows/ci.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/zet/actions/workflows/ci.yml?query=branch%3Atrunk)
[![fuzz](https://github.com/zexUlt/zet/actions/workflows/fuzz.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/zet/actions/workflows/fuzz.yml?query=branch%3Atrunk)
[![fuzz soak](https://github.com/zexUlt/zet/actions/workflows/fuzz-soak.yml/badge.svg?event=schedule)](https://github.com/zexUlt/zet/actions/workflows/fuzz-soak.yml?query=event%3Aschedule)
[![macOS e2e](https://github.com/zexUlt/zet/actions/workflows/macos-e2e.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/zet/actions/workflows/macos-e2e.yml?query=branch%3Atrunk)
[![systemd e2e](https://github.com/zexUlt/zet/actions/workflows/systemd-e2e.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/zet/actions/workflows/systemd-e2e.yml?query=branch%3Atrunk)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

A terminal session that survives what breaks ssh: a change of network, a laptop
going to sleep, a connection dropping mid-command. The session keeps running on
the remote host and resumes byte for byte — nothing typed and nothing printed is
lost. Written from scratch in C++23, with its own protocol; there is no wire
compatibility with EternalTerminal and no shared code with it.

## Install

Download a release tarball and `SHA256SUMS` from
[GitHub Releases](https://github.com/zexUlt/zet/releases), verify them,
and unpack.

**Linux (x86_64 or aarch64):**

```sh
# The attestation is the trust root (requires gh ≥ 2.49).
# SHA256SUMS guards against transfer corruption only.
gh attestation verify zet-X.Y.Z-linux-x86_64.tar.gz \
    --repo zexUlt/zet

sha256sum --check --ignore-missing SHA256SUMS

mkdir -p ~/.local
tar -C ~/.local -xzf zet-X.Y.Z-linux-x86_64.tar.gz --strip-components=1
```

**macOS (arm64, macOS 14 or later):**

```sh
gh attestation verify zet-X.Y.Z-macos-arm64.tar.gz \
    --repo zexUlt/zet

shasum -a 256 -c --ignore-missing SHA256SUMS

mkdir -p ~/.local
tar -C ~/.local -xzf zet-X.Y.Z-macos-arm64.tar.gz --strip-components=1
```

After unpacking, `bin/zet` lands under `~/.local`. Linux binaries are
statically linked (musl + libc++); no other dependencies are needed. The
macOS tarball contains `bin/zet` only; the macOS client links `libSystem`
dynamically and `libsodium` statically. `zet --version` prints the release
version.

The Linux tarballs carry agents for both `x86_64` and `aarch64` under
`libexec/zet/`, so the client (M5.5) can bootstrap either server from any
client machine.

## Status

In development, usable on Linux. `zet` and `zet-agent` work end to end:
bootstrap over ssh, a session that survives cuts byte for byte, vim and htop,
an orphan timeout that reaps the shell (milestone M4). The client also builds
and runs on macOS against a Linux agent. `zet-muxd`, one port per machine for
every user's sessions, is in place and under verification (M5).

The roadmap and its acceptance criteria are in
[docs/design.md](docs/design.md), §17.

## What zet is not

- **Not a multiplexer.** tmux and zellij do that better, and zet is transparent
  to them.
- **Not a replacement for ssh.** Bootstrap runs over ssh, and user
  authentication is delegated to it.
- **Not cross-platform in v1.** Linux first, macOS after. Windows is not
  planned.
- **Not a port of EternalTerminal.** No shared lines of code, and the protocol
  is its own.

## Building

```sh
tools/musl_build.sh musl-dev test
```

The build runs inside an Alpine container because that is where the toolchain
that produces the shipped binaries lives: musl and libc++, linked statically, so
that copying `zet-agent` to someone else's host is enough to make it run there.
Docker and a checkout are the only prerequisites; the toolchain is installed
from a cached set of `.apk` files.

The first argument is a CMake preset (`musl-dev`, `musl-asan-ubsan`,
`musl-coverage`, `musl-release`); the second, if present, runs `ctest`.

## Git hooks

One command after cloning, and a merge conflict can no longer be committed by
accident, while unformatted code is fixed before it becomes a commit:

```sh
git config core.hooksPath tools/git-hooks
```

The formatting step runs `clang-format` over the staged `.cpp` and `.hpp`
files, rewrites the ones it disagrees with and stages them again — so a commit
never fails CI over whitespace. It names every file it touched: clang-format
works on whole files, and a source staged hunk by hunk with `git add -p` ends
up fully staged. Without `clang-format` on `PATH` the step warns and lets the
commit through; CI checks the formatting either way.

## Layout

- `src/core` — `zet_core`: byte reader and writer, logging, secrets, time. No
  I/O.
- `src/crypto` — `zet_crypto`: the six-function interface over libsodium, the
  only runtime dependency.
- `src/wire` — `zet_wire`: frame format and protocol state machines, sans-I/O.
- `tests` — doctest unit tests, one file per source unit.
- `tools` — build and CI gates: binary size, forbidden symbols, dependency
  allowlist, coverage.
- `docs` — the two documents below.

## Documents

Both are in Russian; code, identifiers and commit messages are in English.

- [docs/design.md](docs/design.md) — the architecture of zet and the decisions
  behind it, with trade-offs and ADRs. The source of truth.
- [docs/prior-art.md](docs/prior-art.md) — how EternalTerminal, mosh, tmux,
  WireGuard and QUIC solve the same problems, with references into their
  sources.

## Licence

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Licences of
the third-party code linked into the binaries are listed in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
