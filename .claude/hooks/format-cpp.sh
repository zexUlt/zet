#!/usr/bin/env bash
# PostToolUse(Write|Edit): run clang-format on C++ files that were just written.
set -uo pipefail

command -v jq >/dev/null 2>&1 || exit 0
command -v clang-format >/dev/null 2>&1 || exit 0

file=$(jq -r '.tool_response.filePath // .tool_input.file_path // empty')
[ -n "$file" ] || exit 0

case "$file" in
    *.cpp|*.hpp|*.cc|*.cxx|*.h) ;;
    *) exit 0 ;;
esac

[ -f "$file" ] || exit 0

clang-format -i "$file" 2>/dev/null || true
exit 0
