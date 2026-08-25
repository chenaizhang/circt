// RUN: circt-opt --convert-hw-to-systemc %s | FileCheck %s
// RUN: circt-opt --convert-hw-to-systemc %s | circt-translate --export-systemc | FileCheck %s --check-prefix=CPP

// Input block arguments contain only inputs.  Outputs are deliberately
// interleaved with inputs in the module port list to guard the HW-to-SystemC
// argument/port mapping.
// CHECK-LABEL: systemc.module @interleaved
// CHECK-SAME: %a: !systemc.in<!systemc.uint<8>>
// CHECK-SAME: %y: !systemc.out<!systemc.uint<16>>
// CHECK-SAME: %b: !systemc.in<!systemc.uint<8>>
// CHECK-SAME: %z: !systemc.out<!systemc.uint<8>>
// CHECK: %{{.*}} = comb.concat %{{.*}}, %{{.*}} : i8, i8
// CHECK: systemc.signal.write %y
// CHECK: systemc.signal.write %z

// CPP-LABEL: SC_MODULE(interleaved) {
// CPP: sc_in<sc_uint<8>> a;
// CPP: sc_out<sc_uint<16>> y;
// CPP: sc_in<sc_uint<8>> b;
// CPP: sc_out<sc_uint<8>> z;
hw.module @interleaved(in %a : i8, out y : i16, in %b : i8, out z : i8) {
  %wide = comb.concat %a, %b : i8, i8
  hw.output %wide, %b : i16, i8
}
