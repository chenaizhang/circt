#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CIRCT_ROOT=${CIRCT_ROOT:-$(cd "$ROOT/../../../../" && pwd)}
BUILD_DIR=${CIRCT_BUILD_DIR:-$CIRCT_ROOT/build}
WORK_DIR=${WORK_DIR:-${TMPDIR:-/tmp}/circt-verilated-mixed}
VERILATOR_ROOT=${VERILATOR_ROOT:-$(
  verilator -V | sed -n 's/^ *VERILATOR_ROOT *= *//p' | head -n 1
)}

if [[ -z "$VERILATOR_ROOT" || ! -f "$VERILATOR_ROOT/include/verilated.cpp" ]]; then
  echo "cannot locate the Verilator runtime sources" >&2
  exit 1
fi

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/verilated" "$WORK_DIR/generated"

verilator --cc --Mdir "$WORK_DIR/verilated" --top-module Leaf \
  "$ROOT/Leaf.sv"
make -C "$WORK_DIR/verilated" -f VLeaf.mk VLeaf__ALL.a

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
  -I"$VERILATOR_ROOT/include" -I"$VERILATOR_ROOT/include/vltstd" \
  "$WORK_DIR/tb.cpp" "$WORK_DIR/verilated/VLeaf__ALL.a" \
  "$VERILATOR_ROOT/include/verilated.cpp" \
  "$VERILATOR_ROOT/include/verilated_threads.cpp" \
  -lsystemc -pthread -o "$WORK_DIR/mixed-sim"

"$WORK_DIR/mixed-sim"

cat > "$WORK_DIR/aggregate.mlir" <<'MLIR'
!Cfg = !hw.struct<enable: i1, mode: i3>
!Pixels = !hw.array<2xi8>

hw.module @AggregateLeaf(in %cfg: !Cfg, in %pixels: !Pixels, out result: i3) {
  %mode = hw.struct_extract %cfg["mode"] : !Cfg
  hw.output %mode : i3
}

hw.module @AggregateTop(in %cfg: !Cfg, in %pixels: !Pixels, out result: i3) {
  %result = hw.instance "leaf" @AggregateLeaf(
    cfg: %cfg: !Cfg,
    pixels: %pixels: !Pixels
  ) -> (result: i3)
  hw.output %result : i3
}
MLIR

"$BUILD_DIR/bin/circt-opt" \
  --hw-flatten-io="flatten-arrays=true join-char=_" \
  --hw-aggregate-to-comb --hw-convert-bitcasts --hw-aggregate-to-comb \
  "$WORK_DIR/aggregate.mlir" -o "$WORK_DIR/aggregate-prepared.mlir"

# The Verilator ABI is generated from CIRCT's flattened SystemVerilog, not
# from the original aggregate-port RTL.  This makes both sides use the exact
# same cfg_enable/cfg_mode/pixels_0/pixels_1 member names.
"$BUILD_DIR/bin/circt-opt" --export-verilog \
  "$WORK_DIR/aggregate-prepared.mlir" -o /dev/null \
  > "$WORK_DIR/aggregate-flat.sv"
verilator --cc --Mdir "$WORK_DIR/verilated-aggregate" \
  --top-module AggregateLeaf --prefix VAggregateLeaf \
  "$WORK_DIR/aggregate-flat.sv"
make -C "$WORK_DIR/verilated-aggregate" -f VAggregateLeaf.mk \
  VAggregateLeaf__ALL.a

"$BUILD_DIR/bin/circt-opt" \
  --systemc-wrap-verilated-instances="modules=AggregateLeaf" \
  --symbol-dce \
  --convert-hw-to-systemc="prepared-input=true" \
  --systemc-lower-instance-interop \
  --systemc-lower-container-interop \
  "$WORK_DIR/aggregate-prepared.mlir" > "$WORK_DIR/aggregate-lowered.mlir"
"$BUILD_DIR/bin/circt-translate" --export-systemc \
  "$WORK_DIR/aggregate-lowered.mlir" > "$WORK_DIR/generated/AggregateTop.h"

cp "$ROOT/tb_aggregate.cpp" "$WORK_DIR/tb_aggregate.cpp"
g++ -std=c++17 -I"$WORK_DIR/generated" -I"$WORK_DIR/verilated-aggregate" \
  -I"$VERILATOR_ROOT/include" -I"$VERILATOR_ROOT/include/vltstd" \
  "$WORK_DIR/tb_aggregate.cpp" \
  "$WORK_DIR/verilated-aggregate/VAggregateLeaf__ALL.a" \
  "$VERILATOR_ROOT/include/verilated.cpp" \
  "$VERILATOR_ROOT/include/verilated_threads.cpp" \
  -lsystemc -pthread -o "$WORK_DIR/aggregate-mixed-sim"

"$WORK_DIR/aggregate-mixed-sim"
echo "verilated mixed SystemC scalar and aggregate tests: PASS"
