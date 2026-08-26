// RUN: circt-opt --systemc-lower-instance-interop %s | FileCheck %s

hw.module.extern @Wide (in %a : i192, out c: i192)

hw.module @Top (in %x: i192, out y: i192) {
  // CHECK: interop.procedural.update
  // CHECK: %[[MEMBER:.+]] = systemc.cpp.member_access {{%.+}} arrow "a" : {{.*}} -> !emitc.opaque<"VlWide<6>">
  // CHECK: %[[SOURCE:.+]] = systemc.convert {{%.+}} : (i192) -> !systemc.biguint<192>
  // CHECK: systemc.cpp.call_opaque "circt_systemc::assign_wide"(%[[MEMBER]], %[[SOURCE]]) : (!emitc.opaque<"VlWide<6>">, !systemc.biguint<192>) -> ()
  // CHECK: %[[OUTPUT:.+]] = systemc.cpp.call_opaque "circt_systemc::read_wide<192>"({{%.+}}) : (!emitc.opaque<"VlWide<6>">) -> !systemc.biguint<192>
  // CHECK: systemc.convert %[[OUTPUT]] : (!systemc.biguint<192>) -> i192
  %c = systemc.interop.verilated "wide" @Wide (a: %x: i192) -> (c: i192)
  hw.output %c : i192
}
