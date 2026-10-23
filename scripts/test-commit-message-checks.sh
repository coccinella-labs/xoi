#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hook="$repo_root/scripts/hooks/commit-msg"
checker="$repo_root/scripts/check-commit-messages.sh"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

expect_hook_result() {
  local expected="$1"
  local name="$2"
  local content="$3"
  printf '%s' "$content" > "$tmpdir/message"
  if "$hook" "$tmpdir/message" >/dev/null 2>&1; then
    actual=accepted
  else
    actual=rejected
  fi
  if [[ "$actual" != "$expected" ]]; then
    echo "error: hook $actual $name message, expected $expected" >&2
    exit 1
  fi
}

expect_hook_result accepted valid 'Add one check (#14)
'
expect_hook_result rejected 'missing issue number' 'Missing issue suffix
'
expect_hook_result rejected 'overlong subject' 'This subject is deliberately much too long to pass the seventy two character limit (#14)
'
expect_hook_result rejected 'em dash' $'Add check \xe2\x80\x94 now (#14)\n'
expect_hook_result rejected 'message body' 'Add check (#14)
Details
'
expect_hook_result rejected 'blank line' 'Add check (#14)

Details
'
expect_hook_result rejected 'signed-off trailer' 'Add check (#14)
Signed-off-by: Example <example@example.com>
'
expect_hook_result rejected 'co-author trailer' 'Add check (#14)
Co-authored-by: Example <example@example.com>
'
expect_hook_result rejected 'generated trailer' 'Add check (#14)
Generated-by: tool
'

git -C "$tmpdir" init -q
git -C "$tmpdir" config user.name Test
git -C "$tmpdir" config user.email test@example.invalid
git -C "$tmpdir" commit --allow-empty -qm 'Base commit (#1)'
base="$(git -C "$tmpdir" rev-parse HEAD)"
git -C "$tmpdir" commit --allow-empty -qm 'Valid change (#14)'
head="$(git -C "$tmpdir" rev-parse HEAD)"
(cd "$tmpdir" && "$checker" "$base" "$head")

for content in \
  'Missing issue suffix' \
  'Add check (#14)
Details' \
  'Add check (#14)

Details' \
  'Add check (#14)
Signed-off-by: Example <example@example.com>' \
  'Add check (#14)
Co-authored-by: Example <example@example.com>' \
  'Add check (#14)
Generated-by: tool'; do
  printf '%s\n' "$content" > "$tmpdir/message"
  git -C "$tmpdir" commit --allow-empty -F "$tmpdir/message" -q
  head="$(git -C "$tmpdir" rev-parse HEAD)"
  if (cd "$tmpdir" && "$checker" "$base" "$head") >/dev/null 2>&1; then
    echo "error: CI checker accepted an invalid commit message" >&2
    exit 1
  fi
done

echo "Commit-message hook and CI checks passed acceptance and rejection cases."
