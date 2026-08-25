// RUN: circt-opt --verify-each=false \
// RUN:   --systemc-wrap-verilated-instances="modules=Left,Right" \
// RUN:   --convert-hw-to-systemc="prepared-input=true" \
// RUN:   --systemc-lower-instance-interop --systemc-lower-container-interop %s \
// RUN:   | FileCheck %s

// A ready/valid feedback path between Verilator black boxes is a structural
// channel cycle.  It must not be treated as a combinational expression cycle
// while the parent HW graph is reordered into a SystemC process body.
hw.module @Left(in %ready: i1, out valid: i1) {
  hw.output %ready : i1
}

hw.module @Right(in %valid: i1, out ready: i1) {
  hw.output %valid : i1
}

// CHECK: emitc.include "VRight.h"
// CHECK: emitc.include "VLeft.h"
// CHECK: hw.module.extern @Left
// CHECK: hw.module.extern @Right
// CHECK: systemc.module @FeedbackTop
// CHECK: systemc.signal
// CHECK: systemc.signal.read
// CHECK: systemc.signal.write
// CHECK-NOT: unresolved dependency cycle
hw.module @FeedbackTop(in %seed: i1, out done: i1) {
  %valid = hw.instance "left" @Left(ready: %ready: i1) -> (valid: i1)
  %valid2 = hw.instance "left2" @Left(ready: %ready: i1) -> (valid: i1)
  %ready = hw.instance "right" @Right(valid: %valid: i1) -> (ready: i1)
  %combined = comb.xor %valid, %valid2 : i1
  %done = comb.xor %ready, %combined, %seed : i1
  hw.output %done : i1
}
