#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
  echo "usage: $0 <base-sha> <head-sha>" >&2
  exit 2
fi

base_sha="$1"
head_sha="$2"
if [[ "$base_sha" =~ ^0+$ ]]; then
  commits="$(git rev-list "$head_sha")"
else
  commits="$(git rev-list "${base_sha}..${head_sha}")"
fi

if [[ -z "$commits" ]]; then
  echo "No commits to check."
  exit 0
fi

while IFS= read -r commit; do
  subject="$(git show -s --format=%s "$commit")"
  message="$(git show -s --format=%B "$commit")"

  if [[ ! "$subject" =~ ^.+\ \(#[0-9]+\)$ ]]; then
    echo "error: $commit has an invalid subject: $subject" >&2
    exit 1
  fi
  if (( ${#subject} > 72 )); then
    echo "error: $commit subject exceeds 72 characters" >&2
    exit 1
  fi
  if [[ "$message" == *$'\xe2\x80\x94'* ]]; then
    echo "error: $commit message contains an em dash" >&2
    exit 1
  fi
done <<< "$commits"
