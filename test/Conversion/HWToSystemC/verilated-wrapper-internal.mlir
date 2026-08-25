// RUN: circt-opt --hw-flatten-io --hw-aggregate-to-comb \
// RUN:   --hw-convert-bitcasts --hw-aggregate-to-comb \
// RUN:   --systemc-wrap-verilated-instances="modules=Leaf" \
// RUN:   --convert-hw-to-systemc="prepared-input=true" \
// RUN:   --systemc-lower-instance-interop --systemc-lower-container-interop %s \
// RUN:   | FileCheck %s

// An explicitly selected internal module becomes a Verilator black box after
// the aggregate preparation stage.  Its RTL body must not be lowered again.
hw.module @Leaf (in %a: i32, out c: i32) {
  %sum = comb.add %a, %a : i32
  hw.output %sum : i32
}

// CHECK: emitc.include "VLeaf.h"
// CHECK: hw.module.extern @Leaf
// CHECK-NOT: comb.add
// CHECK: systemc.module @Top
// CHECK: systemc.cpp.new
hw.module @Top (in %x: i32, out y: i32) {
  %c = hw.instance "leaf" @Leaf (a: %x: i32) -> (c: i32)
  hw.output %c : i32
}
