#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Agent delivery (design.md §4, "Доставка агента (M5.5)").
///
/// When --agent PATH is not given, zet delivers zet-agent to the remote host
/// itself before bootstrap: one ssh -T, no extra authentication step.
namespace zet::client {

// ---------------------------------------------------------------------------
// Installer script
// ---------------------------------------------------------------------------

/// The POSIX sh one-liner that runs on the remote side (design.md §4,
/// "Обмен"). No `!` and no literal newlines: the user's login shell may be
/// csh, which expands `!` in history substitution even inside single quotes.
/// `exec /bin/sh -c` at the front of the remote command shields the rest from
/// the login shell's syntax; the installer itself is for /bin/sh only.
///
/// What it does, in order:
///   1. Creates and chmod-0700s the cache directory.
///   2. Probes noexec by writing, chmod +x-ing, and running a small script.
///   3. Prints ZET-PROBE 1 <uname -m> exec|noexec.
///   4. Reads one line: `<hash> <size>` from the client.
///   5. If hash/zet-agent is executable: prints ZET-HAVE and falls through to
///   exec.
///   6. Otherwise prints ZET-SEND, receives <size> raw bytes via head -c,
///      verifies the byte count, chmod 0755 + renames, cleans up stale hash
///      dirs (skipping any that contain a .part file), prints ZET-STORED or
///      ZET-FAILED <reason>.
///   7. exec <hash>/zet-agent --bootstrap; from here §4 bootstrap is unchanged.
///
/// The constant is tested (tests/client/deliver_test.cpp) to have no `!`, no
/// newline, and to survive ShellQuote.
/// The cleanup loop checks kill -0 on each .part.<pid> file so that stale
/// partial files left by a crashed installer do not protect a dead hash dir
/// forever.  A live .part (kill -0 succeeds) means delivery is still in
/// flight from another client: leave that dir alone.  The remaining race —
/// cleanup by another client can remove a dir between its ZET-HAVE and exec
/// → bootstrap failure — is accepted; the next connect reinstalls.
inline constexpr std::string_view INSTALLER_SCRIPT =
    "d=${XDG_CACHE_HOME:-$HOME/.cache}/zet; "
    "mkdir -p \"$d\"; "
    "chmod 0700 \"$d\"; "
    "p=\"$d/.probe.$$\"; "
    "printf 'exit 0' >\"$p\"; "
    "chmod +x \"$p\" && \"$p\" >/dev/null 2>&1 && x=exec || x=noexec; "
    "rm -f \"$p\"; "
    "printf '\\nZET-PROBE 1 %s %s\\n' \"$(uname -m)\" \"$x\"; "
    "read -r hs || exit 1; "
    "h=${hs%% *}; "
    "s=${hs##* }; "
    "t=\"$d/$h\"; "
    "mkdir -p \"$t\"; "
    "chmod 0700 \"$t\"; "
    "if test -x \"$t/zet-agent\"; then "
    "printf 'ZET-HAVE\\n'; "
    "else "
    "printf 'ZET-SEND\\n'; "
    "if head -c \"$s\" >\"$t/.part.$$\"; then "
    "n=$(wc -c <\"$t/.part.$$\"); "
    "n=\"${n##* }\"; "
    "if test \"$n\" -eq \"$s\" 2>/dev/null; then "
    "if chmod 0755 \"$t/.part.$$\" && mv \"$t/.part.$$\" \"$t/zet-agent\"; "
    "then "
    "for _pf in \"$t\"/.part.*; do test -f \"$_pf\" || continue; "
    "_pp=\"${_pf##*.}\"; kill -0 \"$_pp\" 2>/dev/null || rm -f \"$_pf\"; done; "
    "for od in \"$d\"/*/; do case $od in \"$t/\") ;; "
    "*) _lv=0; for _pf in \"$od\".part.*; do test -f \"$_pf\" || continue; "
    "_pp=\"${_pf##*.}\"; kill -0 \"$_pp\" 2>/dev/null && _lv=1 && break; "
    "done; test \"$_lv\" = 0 && rm -rf \"$od\";; "
    "esac; done; "
    "printf 'ZET-STORED\\n'; "
    "else "
    "rm -f \"$t/.part.$$\"; "
    "printf 'ZET-FAILED rename\\n'; "
    "fi; "
    "else "
    "rm -f \"$t/.part.$$\"; "
    "printf 'ZET-FAILED size\\n'; "
    "fi; "
    "else "
    "rm -f \"$t/.part.$$\"; "
    "printf 'ZET-FAILED write\\n'; "
    "fi; "
    "fi; "
    "exec \"$t/zet-agent\" --bootstrap";

// ---------------------------------------------------------------------------
// Agent binary lookup
// ---------------------------------------------------------------------------

/// Compile-time default agent directory; overridden by ZET_AGENT_DIR.
/// Defined here so the install() work (branch `releases`) can hook it via a
/// configure_file-generated header without touching deliver.cpp.
#ifndef ZET_DEFAULT_AGENT_DIR
#define ZET_DEFAULT_AGENT_DIR "/usr/local/libexec/zet"
#endif

/// Architectures v1 knows how to deliver.
inline constexpr std::string_view AGENT_ARCH_X86_64 = "x86_64";
inline constexpr std::string_view AGENT_ARCH_AARCH64 = "aarch64";

/// Reads the agent binary for `arch` from `agentDir`
/// (`<agentDir>/zet-agent-<arch>`). Returns the binary on success, or an
/// empty vector if the file cannot be read (the caller prints a clear error).
[[nodiscard]] std::vector<std::byte> LoadAgentBinary(
    std::string_view agentDir, std::string_view arch) noexcept;

// ---------------------------------------------------------------------------
// Hash → hex
// ---------------------------------------------------------------------------

/// Hex string of BLAKE2b-256 of `data` (32 bytes → 64 hex chars).
[[nodiscard]] std::string HashHex(std::span<const std::byte> data) noexcept;

}  // namespace zet::client
