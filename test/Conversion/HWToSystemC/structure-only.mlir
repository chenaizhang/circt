// RUN: circt-opt --convert-hw-to-systemc="structure-only=true" %s | FileCheck %s --check-prefix=IR
// RUN: circt-opt --convert-hw-to-systemc="structure-only=true" %s | circt-translate --export-systemc | FileCheck %s --check-prefix=CPP

// The structure-only route must not inspect or emit Comb/Seq behavior.  It
// preserves module ports, child instances and shared SSA connection groups.

// IR-LABEL: systemc.module @leaf
// IR: %innerLogic = systemc.func {
// IR-NEXT: }
// CPP: SC_MODULE(leaf) {
// CPP: void innerLogic() {
// CPP-NEXT: }
hw.module @leaf(in %a : i8, out y : i8) {
  %sum = comb.add %a, %a : i8
  hw.output %sum : i8
}

// IR-LABEL: systemc.module private @private_leaf
// IR-NOT: attributes {sym_visibility
hw.module private @private_leaf() {
}

// IR-LABEL: systemc.module @top
// IR: %u0 = systemc.instance.decl @leaf
// IR: %u1 = systemc.instance.decl @leaf
// IR: systemc.instance.bind_port %u0["a"] to %a
// IR: systemc.instance.bind_port %u0["y"] to %net_0
// IR: systemc.instance.bind_port %u1["a"] to %net_0
// IR: systemc.instance.bind_port %u1["y"] to %y
// IR-NOT: comb.add
// CPP: SC_MODULE(top) {
// CPP-DAG: leaf u0{"u0"};
// CPP-DAG: leaf u1{"u1"};
// CPP-DAG: sc_signal<sc_uint<8>> net_0;
// CPP: SC_CTOR(top)
// CPP: u0.a(a);
// CPP: u0.y(net_0);
// CPP: u1.a(net_0);
// CPP: u1.y(y);
hw.module @top(in %a : i8, out y : i8) {
  %u0.y = hw.instance "u0" @leaf(a: %a: i8) -> (y: i8)
  %u1.y = hw.instance "u1" @leaf(a: %u0.y: i8) -> (y: i8)
  hw.output %u1.y : i8
}
