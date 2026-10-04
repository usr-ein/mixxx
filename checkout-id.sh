#!/usr/bin/env bash
# The Dockerfile's CHECKOUT_ID for this checkout: nothing for the main one,
# "-wt-<name>-<hash>" for a git worktree of the repo around it.
#
#   docker buildx build ... --build-arg CHECKOUT_ID="$(./checkout-id.sh)" .
#
# Every checkout gets its own build tree in Docker's cache. ninja decides what
# to rebuild by mtime, so two checkouts taking turns in one tree each ship the
# other's object files -- whichever were newer -- and nothing errors. The
# compiler cache stays shared: it hashes contents, so a worktree's first build
# is mostly cache hits. CHECKOUT_ID set in the environment wins.
set -euo pipefail
cd "$(dirname "$0")"

if [ -n "${CHECKOUT_ID:-}" ]; then
	printf '%s\n' "$CHECKOUT_ID"
	exit 0
fi
# The superproject's checkout: the main one, or a worktree of it. A worktree's
# git dir is under the main one's .git/worktrees/; the main checkout's is the
# common dir itself.
SUPER="$(git rev-parse --show-superproject-working-tree 2>/dev/null || true)"
[ -n "$SUPER" ] || SUPER="$(git rev-parse --show-toplevel)"
gitdir="$(git -C "$SUPER" rev-parse --absolute-git-dir)"
common="$(git -C "$SUPER" rev-parse --path-format=absolute --git-common-dir)"
if [ "$gitdir" = "$common" ]; then
	printf '\n'
else
	name="$(basename "$SUPER" | tr -c 'A-Za-z0-9._-' '-' | cut -c1-40)"
	printf -- '-wt-%s-%s\n' "$name" "$(printf '%s' "$SUPER" | shasum | cut -c1-8)"
fi
