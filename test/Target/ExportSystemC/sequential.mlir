// RUN: circt-translate --export-systemc %s | FileCheck %s

emitc.include <"systemc.h">

systemc.module @edge_detector(%clk: !systemc.in<i1>,
                              %edge: !systemc.out<i1>) {
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %clk : !systemc.in<i1>
  }
  %innerLogic = systemc.func {
    %posedge = systemc.signal.posedge %clk : !systemc.in<i1>
    systemc.signal.write %edge, %posedge : !systemc.out<i1>
  }
}

// CHECK: SC_CTOR(edge_detector) {
// CHECK: SC_METHOD(innerLogic);
// CHECK: sensitive << clk;
// CHECK: void innerLogic() {
// CHECK: edge.write(clk.posedge());
// CHECK-NOT: UNSUPPORTED OPERATION
