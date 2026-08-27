// RUN: circt-opt --convert-hw-to-systemc %s | FileCheck %s

hw.module @counter(in %clk: i1, in %reset: i1, in %enable: i1,
                   in %next: i8, out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %value = seq.compreg.ce name "value" %next, %clock, %enable
      reset %reset, %zero : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @counter
// CHECK: %value_state = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: systemc.sensitive {{.*}}%value_state
// CHECK: %[[STATE:.*]] = systemc.signal.read %value_state
// CHECK: %[[ENABLE:.*]] = comb.mux {{.*}}, {{.*}}, {{.*}} : i8
// CHECK: %[[RESET:.*]] = comb.mux {{.*}}, {{.*}}, %[[ENABLE]] : i8
// CHECK: %[[EDGE:.*]] = systemc.signal.posedge %clk : !systemc.in<i1>
// CHECK: %[[NEXT:.*]] = comb.mux %[[EDGE]], %[[RESET]], {{.*}} : i8
// CHECK: systemc.signal.write %value_state
// CHECK-NOT: seq.

// -----

hw.module @async_register(in %clk: i1, in %reset: i1, in %next: i8,
                          out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %value = seq.firreg %next clock %clock reset async %reset, %zero : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @async_register
// CHECK: %[[STATE_SIGNAL:.*]] = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: %[[EDGE:.*]] = systemc.signal.posedge %clk : !systemc.in<i1>
// CHECK: %[[CLOCKED:.*]] = comb.mux %[[EDGE]], {{.*}}, {{.*}} : i8
// CHECK: %[[ASYNC:.*]] = comb.mux {{.*}}, {{.*}}, %[[CLOCKED]] : i8
// CHECK: systemc.signal.write %[[STATE_SIGNAL]]
// CHECK-NOT: seq.

// -----

// Exercise a graph-region feedback edge where the register input is defined
// after the register. Lowering must replace the feedback with a current-state
// signal read and leave the add in the combinational next-state cone.
hw.module @feedback_counter(in %clk: i1, in %reset: i1, out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %one = hw.constant 1 : i8
  %value = seq.compreg name "value" %next, %clock reset %reset, %zero : i8
  %next = comb.add %value, %one : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @feedback_counter
// CHECK: %[[STATE_SIGNAL:.*]] = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: %[[STATE_READ:.*]] = systemc.signal.read %[[STATE_SIGNAL]]
// CHECK: %[[CURRENT:.*]] = systemc.convert %[[STATE_READ]] : (!systemc.uint<8>) -> i8
// CHECK: %[[NEXT:.*]] = comb.add {{.*}}, {{.*}} : i8
// CHECK: %[[RESET_NEXT:.*]] = comb.mux {{.*}}, {{.*}}, %[[NEXT]] : i8
// CHECK: %[[EDGE:.*]] = systemc.signal.posedge %clk : !systemc.in<i1>
// CHECK: comb.mux %[[EDGE]], %[[RESET_NEXT]], %[[CURRENT]] : i8
// CHECK: systemc.signal.write %[[STATE_SIGNAL]]
// CHECK-NOT: seq.
