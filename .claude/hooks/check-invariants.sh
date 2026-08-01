#!/usr/bin/env bash
# PostToolUse(Write|Edit): warn when a C++ file breaks a zet invariant.
# Advisory only — feeds context back to the model, never blocks.
# Authoritative enforcement lives in CI; see .claude/rules/invariants.md
set -uo pipefail

file=$(python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
resp = d.get("tool_response") or {}
print(resp.get("filePath") or (d.get("tool_input") or {}).get("file_path") or "")
' 2>/dev/null)

[ -n "$file" ] || exit 0

case "$file" in
    *.cpp|*.hpp|*.cc|*.cxx|*.h) ;;
    *) exit 0 ;;
esac

[ -f "$file" ] || exit 0

python3 - "$file" <<'PY'
import json, os, re, sys

path = sys.argv[1]
try:
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
except OSError:
    sys.exit(0)

# Invariants 1, 3 and the size lever apply to protocol/session/crypto code.
# *_main.cpp is the one place allowed to terminate the process.
core = re.search(r"(^|/)(wire|session|crypto|core)/", path) is not None
main_file = path.endswith("_main.cpp") or path.endswith("main.cpp")

CHECKS = [
    (r"\b(abort|_exit|exit)\s*\(",        "1", "аварийное завершение", not main_file),
    (r"\bstd::terminate\s*\(",            "1", "аварийное завершение", not main_file),
    (r"(^|[^_\w])assert\s*\(",            "1", "assert вместо обработки ошибки", not main_file),
    (r"\b(usleep|nanosleep)\s*\(",        "3", "sleep в коде", True),
    (r"\bsleep_for\s*\(",                 "3", "sleep в коде", True),
    (r"#\s*include\s*<iostream>",         "6", "iostream — десятки КБ к бинарю", True),
    (r"#\s*include\s*<regex>",            "6", "regex — сотни КБ к бинарю", True),
]

hits = []
for num, text in enumerate(lines, 1):
    stripped = text.lstrip()
    if stripped.startswith("//") or stripped.startswith("*"):
        continue
    for pattern, inv, why, active in CHECKS:
        if active and re.search(pattern, text):
            hits.append(f"  {os.path.basename(path)}:{num} — инвариант {inv}: {why}")

if core:
    for num, text in enumerate(lines, 1):
        if re.search(r"\.at\s*\(|\[\s*\d+\s*\]", text) and "//" not in text.split("[")[0]:
            hits.append(f"  {os.path.basename(path)}:{num} — инвариант 10: индексация в core, нужен ByteReader")
            break

if not hits:
    sys.exit(0)

seen, uniq = set(), []
for h in hits:
    if h not in seen:
        seen.add(h)
        uniq.append(h)
uniq = uniq[:10]

body = "Возможное нарушение инвариантов zet (.claude/rules/invariants.md):\n" + "\n".join(uniq)
print(json.dumps({
    "systemMessage": f"⚠ инварианты zet: {len(uniq)} совпадений в {os.path.basename(path)}",
    "hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": body + "\n\nПроверь и либо исправь, либо объясни, почему это допустимо здесь.",
    },
}, ensure_ascii=False))
PY

exit 0
