#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CIRCT_ROOT=${CIRCT_ROOT:-$(cd "$ROOT/../../../../" && pwd)}
BUILD_DIR=${CIRCT_BUILD_DIR:-$CIRCT_ROOT/build}
WORK_DIR=${WORK_DIR:-${TMPDIR:-/tmp}/circt-verilated-mixed}

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/verilated" "$WORK_DIR/generated"

verilator --sc --cc --Mdir "$WORK_DIR/verilated" --top-module Leaf \
  "$ROOT/Leaf.sv"
make -C "$WORK_DIR/verilated" -f VLeaf.mk

cat > "$WORK_DIR/input.mlir" <<'MLIR'
hw.module.extern @Leaf (in %a: i32, out c: i32)
hw.module @Top (in %x: i32, out y: i32) {
  %c = hw.instance "leaf" @Leaf (a: %x: i32) -> (c: i32)
  hw.output %c : i32
}
MLIR

"$BUILD_DIR/bin/circt-opt" \
  --systemc-wrap-verilated-instances \
  --convert-hw-to-systemc \
  --systemc-lower-instance-interop \
  --systemc-lower-container-interop \
  "$WORK_DIR/input.mlir" > "$WORK_DIR/lowered.mlir"

"$BUILD_DIR/bin/circt-translate" --export-systemc \
  "$WORK_DIR/lowered.mlir" > "$WORK_DIR/generated/Top.h"

cp "$ROOT/tb.cpp" "$WORK_DIR/tb.cpp"
g++ -std=c++17 -I"$WORK_DIR/generated" -I"$WORK_DIR/verilated" \
  -I/usr/share/verilator/include \
  "$WORK_DIR/tb.cpp" "$WORK_DIR/verilated/libVLeaf.a" \
  "$WORK_DIR/verilated/libverilated.a" \
  -lsystemc -pthread -o "$WORK_DIR/mixed-sim"

"$WORK_DIR/mixed-sim"
echo "verilated mixed SystemC test: PASS"
