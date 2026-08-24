// RUN: circt-translate %s --export-systemc | FileCheck %s

emitc.include <"systemc.h">

// CHECK-LABEL: SC_MODULE(sim_diagnostics)
// CHECK: void run() {
// CHECK: SC_REPORT_ERROR("circt.sim", "conversion failed");
// CHECK: out.read();
systemc.module @sim_diagnostics (%out: !systemc.out<i1>) {
  systemc.ctor {
    systemc.method %run
  }
  %run = systemc.func {
    %message = sim.fmt.literal "conversion failed"
    sim.proc.print %message
    %read = systemc.signal.read %out : !systemc.out<i1>
    %current = systemc.cpp.variable %read : i1
  }
}
