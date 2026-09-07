#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
build_dir=${CIRCT_BUILD_DIR:-$repo_root/build}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

"$build_dir/bin/circt-opt" --convert-hw-to-systemc \
  "$repo_root/test/Conversion/HWToSystemC/sequential.mlir" |
  "$build_dir/bin/circt-translate" --export-systemc \
    -o "$work_dir/memory_models.hpp"

cp "$(dirname "${BASH_SOURCE[0]}")/tb.cpp" "$work_dir/tb.cpp"
c++ -std=c++17 "$work_dir/tb.cpp" -o "$work_dir/memory-runtime" \
  $(pkg-config --cflags --libs systemc)
"$work_dir/memory-runtime"
