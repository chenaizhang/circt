// RUN: circt-opt --convert-hw-to-systemc --verify-diagnostics --split-input-file %s

// expected-error @+2 {{module parameters not supported yet}}
// expected-error @+1 {{failed to legalize operation 'hw.module'}}
hw.module @someModule<p1: i42 = 17, p2: i1>() {}

// -----

// expected-error @+2 {{inout arguments not supported yet}}
// expected-error @+1 {{failed to legalize operation 'hw.module'}}
hw.module @someModule(inout %in0: i32) {}

// -----

// expected-error @+2 {{cannot order HW body before SystemC conversion; unresolved dependency cycle remains after sequential state extraction}}
// expected-error @+1 {{failed to legalize operation 'hw.module'}}
hw.module @graphRegionToSSACFG(in %in0: i32) {
    // expected-note @+1 {{operation in unresolved dependency cycle: comb.add}}
    %0 = comb.add %in0, %1 : i32
    // expected-note @+1 {{operation in unresolved dependency cycle: comb.add}}
    %1 = comb.add %in0, %0 : i32
}
