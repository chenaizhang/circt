// RUN: circt-opt --systemc-wrap-verilated-instances %s | FileCheck %s
// RUN: circt-opt --systemc-wrap-verilated-instances="modules=Leaf" %s | FileCheck %s

hw.module.extern @Leaf (in %a: i32, out c: i32)

// CHECK-LABEL: hw.module @Top
// CHECK: %{{.*}} = systemc.interop.verilated "leaf" @Leaf (a: %{{.*}}: i32) -> (c: i32)
// CHECK-NOT: hw.instance
hw.module @Top (in %x: i32, out y: i32) {
  %c = hw.instance "leaf" @Leaf (a: %x: i32) -> (c: i32)
  hw.output %c : i32
}
