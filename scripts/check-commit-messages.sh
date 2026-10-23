#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
  echo "usage: $0 <base-sha> <head-sha>" >&2
  exit 2
fi

base_sha="$1"
head_sha="$2"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! git cat-file -e "${head_sha}^{commit}" 2>/dev/null; then
  echo "error: invalid head commit: $head_sha" >&2
  exit 2
fi

if [[ "$base_sha" =~ ^0+$ ]] ||
  ! git cat-file -e "${base_sha}^{commit}" 2>/dev/null; then
  if git cat-file -e 'origin/main^{commit}' 2>/dev/null; then
    commits="$(git rev-list "$head_sha" --not origin/main)"
  else
    commits="$(git rev-list "$head_sha")"
  fi
  if [[ -z "$commits" ]]; then
    commits="$head_sha"
  fi
else
  commits="$(git rev-list "${base_sha}..${head_sha}")"
fi

if [[ -z "$commits" ]]; then
  echo "No commits to check."
  exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

while IFS= read -r commit; do
  git cat-file commit "$commit" |
    awk 'BEGIN { in_message = 0 } !in_message && /^$/ { in_message = 1; next } in_message { print }' \
      > "$tmpdir/message"
  if ! "$repo_root/scripts/hooks/commit-msg" "$tmpdir/message"; then
    echo "error: invalid commit message in $commit" >&2
    exit 1
  fi
done <<< "$commits"
