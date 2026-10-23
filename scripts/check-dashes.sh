#!/usr/bin/env bash
# Fails if any tracked text file contains an em dash (U+2014).
set -euo pipefail

if git grep -nIF -- $'\xe2\x80\x94'; then
  echo "error: em dash found; use a colon, comma, period or parentheses" >&2
  exit 1
fi
