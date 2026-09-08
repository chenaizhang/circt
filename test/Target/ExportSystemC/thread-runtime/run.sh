#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
build_dir=${CIRCT_BUILD_DIR:-$repo_root/build}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

"$build_dir/bin/circt-translate" --export-systemc \
  "$(dirname "${BASH_SOURCE[0]}")/model.mlir" \
  -o "$work_dir/timed_thread.hpp"
cp "$(dirname "${BASH_SOURCE[0]}")/tb.cpp" "$work_dir/tb.cpp"
c++ -std=c++17 "$work_dir/tb.cpp" -o "$work_dir/thread-runtime" \
  $(pkg-config --cflags --libs systemc)
"$work_dir/thread-runtime"
