// RUN: circt-opt --convert-hw-to-systemc --verify-diagnostics %s | FileCheck %s

// Aggregate ports are flattened into scalar ports (with '_' join) and
// aggregate operations are lowered to comb before the conversion.

// CHECK: emitc.include <"systemc.h">

// CHECK-LABEL: systemc.module @child (%a_0: !systemc.in<!systemc.uint<4>>, %a_1: !systemc.in<!systemc.uint<4>>, %sum: !systemc.out<!systemc.uint<4>>)
hw.module @child(in %a: !hw.array<2xi4>, out sum: i4) {
  // CHECK: comb.add
  // CHECK: systemc.signal.write %sum
  %c0_i1 = hw.constant 0 : i1
  %c1_i1 = hw.constant 1 : i1
  %e0 = hw.array_get %a[%c0_i1] : !hw.array<2xi4>, i1
  %e1 = hw.array_get %a[%c1_i1] : !hw.array<2xi4>, i1
  %s = comb.add %e0, %e1 : i4
  hw.output %s : i4
}

// CHECK-LABEL: systemc.module @parent (%cfg_a: !systemc.in<!systemc.uint<8>>, %cfg_b: !systemc.in<!systemc.uint<4>>, %data_0: !systemc.in<!systemc.uint<4>>, %data_1: !systemc.in<!systemc.uint<4>>, %res: !systemc.out<!systemc.uint<8>>)
hw.module @parent(in %cfg: !hw.struct<a: i8, b: i4>, in %data: !hw.array<2xi4>, out res: i8) {
  // CHECK: %c = systemc.instance.decl @child : !systemc.module<child(a_0: !systemc.in<!systemc.uint<4>>, a_1: !systemc.in<!systemc.uint<4>>, sum: !systemc.out<!systemc.uint<4>>)>
  // CHECK: systemc.instance.bind_port
  // CHECK: systemc.signal.write %res
  // CHECK-NOT: hw.struct
  // CHECK-NOT: hw.array
  %0 = hw.instance "c" @child(a: %data: !hw.array<2xi4>) -> (sum: i4)
  %1 = hw.struct_inject %cfg["b"], %0 : !hw.struct<a: i8, b: i4>
  %2 = hw.struct_extract %1["a"] : !hw.struct<a: i8, b: i4>
  hw.output %2 : i8
}
