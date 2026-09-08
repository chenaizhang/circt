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

// The instance ABI must retain that same input/output interleaving. HW stores
// operands and results in separate ranges, so this specifically guards against
// accidentally declaring or binding all inputs before all outputs.
// CHECK-LABEL: systemc.module @interleaved_parent
// CHECK: %[[INST:.*]] = systemc.instance.decl @interleaved : !systemc.module<interleaved(a: !systemc.in<!systemc.uint<8>>, y: !systemc.out<!systemc.uint<16>>, b: !systemc.in<!systemc.uint<8>>, z: !systemc.out<!systemc.uint<8>>)>
// CHECK: systemc.instance.bind_port %[[INST]]["a"]
// CHECK: systemc.instance.bind_port %[[INST]]["b"]
// CHECK: systemc.instance.bind_port %[[INST]]["y"]
// CHECK: systemc.instance.bind_port %[[INST]]["z"]
hw.module @interleaved_parent(in %a : i8, in %b : i8,
                              out y : i16, out z : i8) {
  %y, %z = hw.instance "child" @interleaved(a: %a: i8, b: %b: i8)
    -> (y: i16, z: i8)
  hw.output %y, %z : i16, i8
}
