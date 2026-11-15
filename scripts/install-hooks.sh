#!/usr/bin/env bash
# Points this clone's git at the hooks in scripts/githooks/: G086's commit-msg and pre-push check that a commit changing UI code
# carries a `Design-system:` trailer (plan 0025 D14).
#
#   bash scripts/install-hooks.sh            set core.hooksPath in this clone
#   bash scripts/install-hooks.sh --check    say whether it is set; exit 1 when not
#
# A hooksPath already pointing somewhere else is not overwritten: call these hooks from there.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WANT="scripts/githooks"
HAVE="$(git -C "$ROOT" config --get core.hooksPath || true)"
case "${1:-}" in
  --check) [ "$HAVE" = "$WANT" ] && { echo "install-hooks: installed ($WANT)"; exit 0; }
           echo "install-hooks: not installed (core.hooksPath is '${HAVE:-unset}'); run bash scripts/install-hooks.sh" >&2; exit 1 ;;
  "") ;;
  *) echo "usage: install-hooks.sh [--check]" >&2; exit 2 ;;
esac
if [ -n "$HAVE" ] && [ "$HAVE" != "$WANT" ]; then
  echo "install-hooks: core.hooksPath is already '$HAVE'; not overwriting it. Call $WANT/commit-msg and pre-push from there." >&2
  exit 1
fi
git -C "$ROOT" config core.hooksPath "$WANT"
echo "install-hooks: core.hooksPath = $WANT (a commit changing UI code needs a Design-system: trailer)"
