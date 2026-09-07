// RUN: circt-opt --convert-hw-to-systemc %s | FileCheck %s
// RUN: circt-opt --convert-hw-to-systemc %s | circt-translate --export-systemc | FileCheck %s --check-prefix=CPP

hw.module.extern @leaf(in %value: i8, out result: i8) attributes {
  hw.hierarchy.frontier,
  hw.hierarchy.depth = 1 : i64
}

hw.module @top(in %a: i8, in %b: i8, out result: i8) {
  %sum = comb.add %a, %b : i8
  %result = hw.instance "leaf" @leaf(value: %sum: i8) -> (result: i8)
  hw.output %result : i8
}

// CHECK-LABEL: systemc.module @leaf
// CHECK: systemc.hierarchy.frontier
// CHECK: systemc.method %behaviorSlot
// CHECK-LABEL: systemc.module @top
// CHECK: comb.add
// CHECK: systemc.instance.decl @leaf

// CPP-LABEL: SC_MODULE(leaf)
// CPP: SC_METHOD(behaviorSlot);
// CPP-LABEL: SC_MODULE(top)
// CPP: sc_uint<8>(a.read()) + sc_uint<8>(b.read())
// CPP: leaf leaf{"leaf"};
// CPP-NOT: UNSUPPORTED OPERATION
