// RUN: circt-opt --hw-flatten-io="flatten-arrays=true" \
// RUN:   --hw-aggregate-to-comb --hw-convert-bitcasts --hw-aggregate-to-comb \
// RUN:   --systemc-wrap-verilated-instances="modules=Leaf" \
// RUN:   --convert-hw-to-systemc="prepared-input=true" \
// RUN:   --systemc-lower-instance-interop --systemc-lower-container-interop %s \
// RUN:   | FileCheck %s

// The Verilator ABI must see the same scalarized array ports as HWToSystemC.
// Inserting interop before array flattening leaves aggregate operands behind
// and produces an operand-count mismatch during symbol verification.

// CHECK: emitc.include "VLeaf.h"
// CHECK: hw.module.extern @Leaf
// CHECK: systemc.module @Top
// CHECK: "emitc.constant"()
// CHECK: systemc.cpp.new(%{{.*}}) : (!emitc.opaque<"sc_core::sc_module_name">) -> !emitc.ptr<!emitc.opaque<"VLeaf">>
// CHECK-NOT: !hw.array
hw.module @Leaf(in %a: !hw.array<2xi8>, out y: !hw.array<2xi8>) {
  hw.output %a : !hw.array<2xi8>
}

hw.module @Top(in %x: !hw.array<2xi8>, out y: !hw.array<2xi8>) {
  %v = hw.instance "leaf" @Leaf(a: %x: !hw.array<2xi8>) -> (y: !hw.array<2xi8>)
  hw.output %v : !hw.array<2xi8>
}
