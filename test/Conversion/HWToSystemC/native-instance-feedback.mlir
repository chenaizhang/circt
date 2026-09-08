// RUN: circt-opt --convert-hw-to-systemc %s | FileCheck %s

// Sibling modules may legally form a structural feedback path when the state
// that breaks the cycle lives inside a child.  The parent must represent this
// as bound SystemC channels rather than requiring the hw.instance operations
// to form an acyclic SSA expression graph.

hw.module @Left(in %ready: i1, out valid: i1) {
  hw.output %ready : i1
}

hw.module @Right(in %valid: i1, out ready: i1) {
  hw.output %valid : i1
}

hw.module @Top(out observed: i1) {
  %valid = hw.instance "left" @Left(ready: %ready: i1) -> (valid: i1)
  %ready = hw.instance "right" @Right(valid: %valid: i1) -> (ready: i1)
  hw.output %valid : i1
}

// CHECK-LABEL: systemc.module @Top
// CHECK: %[[RIGHT_READY:.*]] = systemc.signal : !systemc.signal<i1>
// CHECK: systemc.sensitive {{.*}}%[[RIGHT_READY]]
// CHECK: systemc.instance.bind_port {{.*}}["ready"] to %[[RIGHT_READY]]
