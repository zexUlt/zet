#!/usr/bin/env bash
# PreToolUse(Bash, if git commit): block commits carrying attribution trailers
# or leaked chat context. Backstop for the `attribution` setting.
set -uo pipefail

command -v jq >/dev/null 2>&1 || exit 0

cmd=$(jq -r '.tool_input.command // empty')
[ -n "$cmd" ] || exit 0

found=$(printf '%s' "$cmd" | grep -oiE 'co-authored-by|generated with|assisted-by|claude-session|noreply@anthropic\.com|🤖' \
        | sort -uf | tr '\n' ',' | sed 's/,$//; s/,/, /g')
[ -n "$found" ] || exit 0

jq -nc --arg found "$found" '{
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    permissionDecision: "deny",
    permissionDecisionReason: (
      "В сообщении коммита найдено: " + $found + ". " +
      "По правилу .claude/rules/git.md сообщение содержит только суть изменения — " +
      "без трейлеров атрибуции и следов обсуждения. Перепиши сообщение."
    )
  }
}'

exit 0
