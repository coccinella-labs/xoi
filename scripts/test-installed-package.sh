#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
if [[ ! -f "$build_dir/cmake_install.cmake" ]]; then
  echo "usage: $0 <configured-build-directory>" >&2
  exit 2
fi
build_dir="$(cd "$build_dir" && pwd)"
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"

install_prefix="$build_dir/package-install"
consumer_build="$build_dir/package-consumer"

cmake --install "$build_dir" --prefix "$install_prefix"
cmake -S "$repo_dir/tests/consumer" -B "$consumer_build" \
  -DCMAKE_PREFIX_PATH="$install_prefix" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$consumer_build" --parallel
"$consumer_build/xoi_consumer_smoke"
