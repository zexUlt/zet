# zet

A terminal session that survives what breaks ssh: a change of network, a laptop
going to sleep, a connection dropping mid-command. The session keeps running on
the remote host and resumes byte for byte — nothing typed and nothing printed is
lost. Written from scratch in C++23, with its own protocol; there is no wire
compatibility with EternalTerminal and no shared code with it.

## Status

In development. There are no working binaries: `zet`, `zet-agent` and
`zet-muxd` do not exist yet as programs you can run. What is in the tree is the
protocol core being built at milestone M1 — framing, AEAD, key derivation — plus
the build and CI machinery around it. If you came here to use zet, there is
nothing to install.

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
