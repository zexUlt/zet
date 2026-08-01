#!/usr/bin/env bash
# Exercises the Claude Code hooks in .claude/hooks with synthetic stdin payloads.
# Run from anywhere: tools/test_hooks.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
hooks="$root/.claude/hooks"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

ok()   { printf '  ok    %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  %s\n     %s\n' "$1" "$2"; fail=$((fail + 1)); }
skip() { printf '  skip  %s (%s)\n' "$1" "$2"; }

edit_payload() {
    printf '{"tool_name":"Edit","tool_input":{"file_path":"%s"}}' "$1"
}
bash_payload() {
    printf '{"tool_name":"Bash","tool_input":{"command":%s}}' "$(printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')"
}

echo "format-cpp.sh"
if ! command -v clang-format >/dev/null 2>&1; then
    skip "formats C++" "clang-format отсутствует"
else
    printf 'namespace zet {\nint  f( int   x ){\nreturn x;\n}\n}\n' > "$tmp/probe.cpp"
    edit_payload "$tmp/probe.cpp" | "$hooks/format-cpp.sh"
    if grep -q 'int f(int x)' "$tmp/probe.cpp"; then
        ok "formats C++"
    else
        bad "formats C++" "файл не отформатирован: $(cat "$tmp/probe.cpp")"
    fi

    cp "$root/CLAUDE.md" "$tmp/keep.md"
    edit_payload "$tmp/keep.md" | "$hooks/format-cpp.sh"
    if cmp -s "$tmp/keep.md" "$root/CLAUDE.md"; then
        ok "leaves non-C++ alone"
    else
        bad "leaves non-C++ alone" "markdown был изменён"
    fi

    if edit_payload "$tmp/does-not-exist.cpp" | "$hooks/format-cpp.sh"; then
        ok "survives missing file"
    else
        bad "survives missing file" "ненулевой код возврата"
    fi
fi

echo "no-attribution-trailer.sh"
for probe in "Co-Authored-By: X <a@b>" "Generated with Claude Code" "commit 🤖" "see noreply@anthropic.com"; do
    out=$(bash_payload "git commit -m \"fix. $probe\"" | "$hooks/no-attribution-trailer.sh")
    decision=$(printf '%s' "$out" | python3 -c 'import json,sys
raw = sys.stdin.read().strip()
print(json.loads(raw)["hookSpecificOutput"]["permissionDecision"] if raw else "")' 2>/dev/null)
    if [ "$decision" = "deny" ]; then
        ok "blocks: $probe"
    else
        bad "blocks: $probe" "решение=<$decision>"
    fi
done

out=$(bash_payload 'git commit -m "Reject zero-length frame"' | "$hooks/no-attribution-trailer.sh")
if [ -z "$out" ]; then
    ok "allows a clean message"
else
    bad "allows a clean message" "неожиданный вывод: $out"
fi

echo "check-invariants.sh"
printf '#include <iostream>\nnamespace zet::wire {\nvoid f() {\n    abort();\n    assert(1);\n    std::this_thread::sleep_for(x);\n}\n}\n' > "$tmp/bad.cpp"
out=$(edit_payload "$tmp/bad.cpp" | "$hooks/check-invariants.sh")
hits=$(printf '%s' "$out" | python3 -c 'import json,sys
raw = sys.stdin.read().strip()
print(json.loads(raw)["hookSpecificOutput"]["additionalContext"].count("инвариант") if raw else 0)' 2>/dev/null)
if [ "${hits:-0}" -ge 4 ]; then
    ok "flags abort/assert/sleep/iostream ($hits)"
else
    bad "flags abort/assert/sleep/iostream" "найдено $hits, ожидалось >= 4"
fi

printf 'namespace zet::wire {\nint add(int a, int b) { return a + b; }\n}\n' > "$tmp/good.cpp"
out=$(edit_payload "$tmp/good.cpp" | "$hooks/check-invariants.sh")
if [ -z "$out" ]; then
    ok "silent on clean code"
else
    bad "silent on clean code" "неожиданный вывод: $out"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
