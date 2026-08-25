// RUN: circt-opt --systemc-wrap-verilated-instances --convert-hw-to-systemc \
// RUN:   --systemc-lower-instance-interop --systemc-lower-container-interop %s \
// RUN:   | FileCheck %s

hw.module.extern @Leaf (in %a: i32, out c: i32)

// CHECK: emitc.include "VLeaf.h"
// CHECK: systemc.module @Top
// CHECK: %{{.*}} = systemc.cpp.variable : !emitc.ptr<!emitc.opaque<"VLeaf">>
// CHECK: "emitc.constant"()
// CHECK: systemc.cpp.new(%{{.*}}) : (!emitc.opaque<"sc_core::sc_module_name">) -> !emitc.ptr<!emitc.opaque<"VLeaf">>
// CHECK: systemc.cpp.assign {{.*}} = {{.*}}
// CHECK: systemc.cpp.member_access {{.*}} arrow "eval"
// CHECK: systemc.cpp.call_indirect
// CHECK: systemc.cpp.delete {{.*}} : !emitc.ptr<!emitc.opaque<"VLeaf">>
hw.module @Top (in %x: i32, out y: i32) {
  %c = hw.instance "leaf" @Leaf (a: %x: i32) -> (c: i32)
  hw.output %c : i32
}
