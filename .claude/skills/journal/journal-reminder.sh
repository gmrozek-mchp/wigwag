#!/bin/sh
# SessionEnd hook: remind the user if code changed but the journal didn't.
#
# Reminder only. Never blocks, never writes to the journal itself — an auto-generated
# entry is worse than no entry, because it looks like a real one.
#
# Contract: stderr is shown to the user on SessionEnd; stdout is ignored. Always exit 0.
# SessionEnd hooks share a ~1.5 s budget, so this must stay cheap.
#
# Install (in .claude/settings.json):
#   {"hooks": {"SessionEnd": [{"hooks": [
#     {"type": "command", "command": ".claude/skills/journal/journal-reminder.sh"}]}]}}

set -u

# Only meaningful inside a git work tree.
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0

root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
cd "$root" 2>/dev/null || exit 0

# Locate the journal the same way the skill does.
journal=""
for candidate in JOURNAL.md docs/JOURNAL.md; do
	if [ -f "$candidate" ]; then
		journal="$candidate"
		break
	fi
done
[ -n "$journal" ] || exit 0

# Uncommitted changes, excluding the journal itself.
changed=$(git status --porcelain -- . 2>/dev/null | grep -v -- "$journal" | head -n 20)
[ -n "$changed" ] || exit 0

# Did the journal change too? Covers staged, unstaged, and untracked.
if git status --porcelain -- "$journal" 2>/dev/null | grep -q .; then
	exit 0
fi

count=$(printf '%s\n' "$changed" | grep -c .)
printf '\n  journal: %s file(s) changed but %s was not updated.\n' "$count" "$journal" >&2
printf '  Run /journal next session, or before committing, to record what happened and why.\n\n' >&2

exit 0
